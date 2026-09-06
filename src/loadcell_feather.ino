// ---------------------------------------------------------------------------
// loadcell_feather.ino
//
// Adafruit ESP32-S2 Reverse TFT Feather + NAU7802 (STEMMA QT / Qwiic)
//
//   * reads a load cell through the NAU7802 24-bit ADC
//   * shows IP address, battery %, and live load on the built-in 240x135 TFT
//   * joins a WiFi AP, serves a browser GUI at http://<ip>/
//   * streams samples over a WebSocket (port 81) to that GUI
//   * also streams plain CSV over raw TCP (port 3333) for scripts
//   * lets the GUI read/write any NAU7802 register
//
// Board: Tools -> Board -> ESP32 Arduino -> "Adafruit Feather ESP32-S2 Reverse TFT"
//        Tools -> USB CDC On Boot -> Enabled
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_MAX1704X.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <DNSServer.h>
#include <Preferences.h>

#include "config.h"
#include "nau7802.h"
#include "webpage.h"
#include "portal.h"

// ---------------------------------------------------------------------------
// globals
// ---------------------------------------------------------------------------
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Adafruit_MAX17048 maxlipo;
NAU7802         adc;
WebServer       http(HTTP_PORT);
WebSocketsServer ws(WS_PORT);
WiFiServer      rawServer(RAW_TCP_PORT);
WiFiClient      rawClients[MAX_RAW_CLIENTS];

// --- WiFi provisioning: if the saved network can't be joined, fall back to
// a setup hotspot + captive portal instead of just running with no network.
Preferences prefs;
DNSServer   dnsServer;
static const char *AP_SSID = "ContactPressure-Setup";
enum WifiMode { WIFI_MODE_CONNECTED, WIFI_MODE_PROVISION };
WifiMode wifiMode = WIFI_MODE_CONNECTED;

// --- measurement state, per NAU7802 channel (0 = CH1, 1 = CH2)
static const uint8_t NCHAN = 2;
int32_t  offsetCounts[NCHAN]  = {0, 0};              // tare, in raw counts
float    countsPerUnit[NCHAN] = {DEFAULT_SCALE, DEFAULT_SCALE};
char     unitLabel[NCHAN][8]  = {DEFAULT_UNITS, DEFAULT_UNITS};
uint8_t  avgWindow[NCHAN]     = {DEFAULT_AVG, DEFAULT_AVG};
int32_t  avgBuf[NCHAN][64];
uint8_t  avgIdx[NCHAN] = {0, 0}, avgFill[NCHAN] = {0, 0};
int64_t  avgSum[NCHAN] = {0, 0};

int32_t  lastRaw[NCHAN]   = {0, 0};
float    lastUnits[NCHAN] = {0, 0};
bool     adcOk = false;
bool     streaming = true;

// --- round-robin channel switching: the NAU7802's offset/gain calibration
// registers are per-channel, and the analog front end needs one full
// conversion to settle after CHS changes, so that conversion is discarded.
uint8_t  curChan = 0;
bool     chanSettling = false;

// --- rate measurement (combined across both channels)
uint32_t sampleCount = 0;
float    actualSps = 0;
uint32_t rateWindowStart = 0;

// --- outgoing batch, per channel
static const uint8_t BATCH_MAX = 32;
int32_t  batchRaw[NCHAN][BATCH_MAX];
uint8_t  batchN[NCHAN] = {0, 0};

uint32_t tLastBatch = 0, tLastStatus = 0, tLastTft = 0;

// --- battery
float    vbat = 0;
int      battPct = 0;

// --- display state: one channel shown at a time, screen can be blanked
uint8_t  dispChan = 0;      // which channel tftUpdate() shows
bool     screenOn  = true;  // false while blanked by a D1 long-press

// ---------------------------------------------------------------------------
// button debouncing
//
// A mechanical button doesn't cleanly go from open to closed: the contacts
// physically bounce for a few ms, so a naive digitalRead() edge check can see
// several spurious transitions on a single press. This tracks how long the
// raw reading has been stable and only commits it as the button's real level
// once it's held past MS - a transition shorter than that is bounce noise
// and gets ignored. Call update() every loop() iteration (not gated to some
// polling interval) so the debounce timing itself stays accurate. Edge
// detection (rising/falling) is done by the caller comparing successive
// update() results, since D1 needs both edges to tell a short tap from a
// long hold.
// ---------------------------------------------------------------------------
struct Debounce {
  static const uint32_t MS = 30;
  bool     raw = false;      // last raw sample seen
  bool     stable = false;   // debounced, committed level
  uint32_t tChange = 0;      // when `raw` last changed

  bool update(bool level) {
    uint32_t now = millis();
    if (level != raw) { raw = level; tChange = now; }
    if (now - tChange >= MS) stable = raw;
    return stable;
  }
};
Debounce db0, db1, db2;

// ---------------------------------------------------------------------------
// battery: MAX17048 fuel gauge over I2C
// ---------------------------------------------------------------------------
bool battOk = false;

void readBattery() {
  if (!battOk) return;
  vbat    = maxlipo.cellVoltage();
  battPct = (int)constrain(maxlipo.cellPercent(), 0.0f, 100.0f);
}

// ---------------------------------------------------------------------------
// averaging helpers
// ---------------------------------------------------------------------------
void resetAverage(uint8_t ch) {
  avgIdx[ch] = 0; avgFill[ch] = 0; avgSum[ch] = 0;
}

int32_t pushAverage(uint8_t ch, int32_t v) {
  if (avgWindow[ch] < 1)  avgWindow[ch] = 1;
  if (avgWindow[ch] > 64) avgWindow[ch] = 64;
  if (avgFill[ch] == avgWindow[ch]) avgSum[ch] -= avgBuf[ch][avgIdx[ch]];
  else                              avgFill[ch]++;
  avgBuf[ch][avgIdx[ch]] = v;
  avgSum[ch] += v;
  avgIdx[ch] = (avgIdx[ch] + 1) % avgWindow[ch];
  return (int32_t)(avgSum[ch] / avgFill[ch]);
}

float toUnits(uint8_t ch, int32_t raw) {
  if (countsPerUnit[ch] == 0) return 0;
  return (float)(raw - offsetCounts[ch]) / countsPerUnit[ch];
}

// ---------------------------------------------------------------------------
// TFT
// ---------------------------------------------------------------------------
#define COL_BG      ST77XX_BLACK
#define COL_LABEL   0x8410            // grey
#define COL_VALUE   ST77XX_WHITE
#define COL_OK      0x07E0
#define COL_WARN    0xFD20
#define COL_BAD     0xF800
#define COL_ACCENT  0x05FF            // cyan
#define COL_PURPLE  0x8010            // purple
#define COL_LTGREY  0xC618            // lighter/"whiter" grey than COL_LABEL
#define COL_TAMU    0x5000            // Texas A&M maroon

String pad(const String &s, int n) {
  String r = s;
  while ((int)r.length() < n) r += ' ';
  return r;
}

// left-edge legend column (button id + function) and a right-edge column
// marking the physical hard-reset button, bracketing the CH1/CH2 reading
// and status area in the middle.
static const int LEGEND_W    = 34;
static const int RIGHT_COL_W = 34;
static const int MID_RIGHT   = 240 - RIGHT_COL_W;   // right edge of the middle area

// Renders text into an off-screen 1-bit buffer, then stamps it onto the
// display rotated 90 clockwise (so it reads top-to-bottom). Simpler and
// safer than juggling the display's global setRotation() mid-draw, since it
// can't disturb anything else's orientation if the math here is off.
void drawVerticalLabel(int x, int y, const char *text, uint16_t color, uint8_t size = 1) {
  GFXcanvas1 canvas(strlen(text) * 6 * size, 8 * size);
  canvas.setTextSize(size);
  canvas.setTextColor(1);
  canvas.setCursor(0, 0);
  canvas.print(text);

  int w = canvas.width(), h = canvas.height();
  for (int sy = 0; sy < h; sy++) {
    for (int sx = 0; sx < w; sx++) {
      if (canvas.getPixel(sx, sy)) {
        tft.drawPixel(x + (h - 1 - sy), y + sx, color);
      }
    }
  }
}

void tftStatic() {
  tft.fillScreen(COL_BG);
  tft.drawFastVLine(LEGEND_W, 0, 135, COL_LABEL);
  tft.drawFastVLine(MID_RIGHT, 0, 135, COL_LABEL);
  tft.drawFastHLine(LEGEND_W, 10, MID_RIGHT - LEGEND_W, COL_LABEL);
  tft.drawFastHLine(LEGEND_W, 118, MID_RIGHT - LEGEND_W, COL_LABEL);

  // button legend, one entry per third of the screen height, D0 top -> D2 bottom
  static const char    *ID[3]    = {"D0", "D1", "D2"};
  static const char    *FUNC[3]  = {"TARE", "CHAN", "POWER"};
  static const uint16_t COLOR[3] = {ST77XX_BLUE, COL_VALUE, COL_BAD};
  tft.setTextSize(1);
  for (uint8_t i = 0; i < 3; i++) {
    int y0 = i * 45;
    tft.setTextColor(COLOR[i], COL_BG);
    tft.setCursor(3, y0 + 16); tft.print(ID[i]);
    tft.setCursor(3, y0 + 28); tft.print(FUNC[i]);
    if (i) tft.drawFastHLine(0, y0, LEGEND_W, COL_LABEL);
  }

  // right column: where the physical hard-reset button is, written
  // vertically (rotated 90) since the column itself is only 34px wide
  drawVerticalLabel(MID_RIGHT + 9, 1, "HARD REBOOT", COL_WARN, 2);
}

void tftUpdate() {
  if (!screenOn || wifiMode == WIFI_MODE_PROVISION) return;
  char buf[40];

  // --- top row: IP address ("IP:" in green, the address itself in grey),
  // then voltage, then the battery bar
  tft.setTextSize(1);
  tft.setCursor(LEGEND_W + 2, 1);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(COL_OK, COL_BG);
    tft.print("IP: ");
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.print(pad(WiFi.localIP().toString(), 13));
  } else {
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.print(pad("connecting...", 19));
  }

  // battery: voltage text right next to the bar, both anchored to the
  // right edge of the middle area (not the screen - the reboot column owns that)
  uint16_t bc = battPct > 40 ? COL_OK : (battPct > 15 ? COL_WARN : COL_BAD);
  int bw = 20, bh = 8, bx = MID_RIGHT - bw - 2, by = 1;
  int fw = constrain(battPct, 0, 100) * (bw - 2) / 100;
  tft.drawRect(bx, by, bw, bh, COL_LABEL);
  tft.fillRect(bx + 1, by + 1, fw, bh - 2, bc);
  tft.fillRect(bx + 1 + fw, by + 1, (bw - 2) - fw, bh - 2, COL_BG);

  tft.setTextColor(COL_VALUE, COL_BG);
  tft.setCursor(bx - 34, by);
  snprintf(buf, sizeof(buf), "%4.2fV", vbat);
  tft.print(buf);

  // --- one big reading for whichever channel D1 has selected
  // ("CH-" smaller and grey, the channel number itself bigger and a
  // lighter/whiter grey - same family, not a different color)
  tft.setTextSize(2);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(LEGEND_W + 2, 14);
  tft.print("CH-");
  tft.setTextSize(3);
  tft.setTextColor(COL_LTGREY, COL_BG);
  snprintf(buf, sizeof(buf), "%u", dispChan + 1);
  tft.print(pad(String(buf), 2));

  // Fixed-width field, sized to always fit at this font size (wrap is off,
  // so anything wider would just clip instead of spilling onto the next
  // line and leaving unerased leftovers there).
  tft.setTextSize(4);
  tft.setTextColor(adcOk ? COL_VALUE : COL_BAD, COL_BG);
  tft.setCursor(LEGEND_W + 2, 52);
  if (adcOk) {
    float v = lastUnits[dispChan];
    if (fabsf(v) < 1000) snprintf(buf, sizeof(buf), "%6.1f", v);
    else                 snprintf(buf, sizeof(buf), "%6.0f", v);
  } else {
    snprintf(buf, sizeof(buf), "   ERR");
  }
  tft.print(buf);

  tft.setTextSize(2);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(LEGEND_W + 2, 90);
  tft.print(pad(String(unitLabel[dispChan]), 6));

  // --- bottom status row: gain, rate, ws clients
  float spsDisp = (isnan(actualSps) || isinf(actualSps)) ? 0.0f : constrain(actualSps, 0.0f, 999.9f);
  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(LEGEND_W + 2, 124);
  snprintf(buf, sizeof(buf), "x%-3u %3uSPS(%5.1f) cl:%u",
           NAU_GAIN_TABLE[adc.getGainIdx()],
           NAU_SPS_TABLE[adc.getSpsIdx()],
           spsDisp,
           ws.connectedClients());
  tft.print(pad(String(buf), 28));
}

// Dedicated screen shown while in the WiFi-setup hotspot: how to join it and
// where to browse if the OS doesn't pop the sign-in page up automatically.
void tftProvisionScreen() {
  tft.fillScreen(COL_BG);
  tft.setTextSize(2);
  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.setCursor(4, 4);
  tft.print("Contact Pressure");

  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(4, 28);
  tft.print("Can't join WiFi. Connect to:");

  tft.setTextColor(COL_WARN, COL_BG);
  tft.setCursor(4, 42);
  tft.print(AP_SSID);

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(4, 62);
  tft.print("Then open in a browser:");

  tft.setTextColor(COL_VALUE, COL_BG);
  tft.setCursor(4, 76);
  tft.print("http://" + WiFi.softAPIP().toString());

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(4, 96);
  tft.print("(skip that if a setup page");
  tft.setCursor(4, 106);
  tft.print(" already popped up)");
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
void sendStatus(int8_t only = -1) {
  JsonDocument d;
  d["t"]      = "s";
  d["ip"]     = WiFi.localIP().toString();
  d["rssi"]   = WiFi.RSSI();
  d["vbat"]   = roundf(vbat * 1000) / 1000.0f;
  d["pct"]    = battPct;
  d["adc"]    = adcOk;
  d["rev"]    = adc.revision();
  d["gain"]   = NAU_GAIN_TABLE[adc.getGainIdx()];
  d["sps"]    = NAU_SPS_TABLE[adc.getSpsIdx()];
  d["ldo"]    = NAU_LDO_TABLE[adc.getLdoIdx()];
  d["sps_act"]= roundf(actualSps * 10) / 10.0f;
  JsonArray ch = d["ch"].to<JsonArray>();
  for (uint8_t i = 0; i < NCHAN; i++) {
    JsonObject o = ch.add<JsonObject>();
    o["offset"] = offsetCounts[i];
    o["scale"]  = countsPerUnit[i];
    o["units"]  = unitLabel[i];
    o["avg"]    = avgWindow[i];
  }
  d["stream"] = streaming;
  d["clients"]= ws.connectedClients();
  d["uptime"] = millis() / 1000;
  d["heap"]   = ESP.getFreeHeap();

  String out;
  serializeJson(d, out);
  if (only < 0) ws.broadcastTXT(out);
  else          ws.sendTXT((uint8_t)only, out);
}

void sendRegs(int8_t only = -1) {
  const uint8_t regs[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,
                          0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,0x13,
                          0x14,0x15,0x16,0x17,0x1B,0x1C,0x1F};
  JsonDocument d;
  d["t"] = "r";
  JsonArray a = d["v"].to<JsonArray>();
  for (uint8_t i = 0; i < sizeof(regs); i++) {
    JsonArray e = a.add<JsonArray>();
    e.add(regs[i]);
    e.add(adc.readReg(regs[i]));
  }
  String out;
  serializeJson(d, out);
  if (only < 0) ws.broadcastTXT(out);
  else          ws.sendTXT((uint8_t)only, out);
}

void sendLog(const char *msg) {
  JsonDocument d;
  d["t"] = "log";
  d["m"] = msg;
  String out;
  serializeJson(d, out);
  ws.broadcastTXT(out);
  Serial.println(msg);
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
// Blocks the round-robin sampler to collect fresh samples from one channel.
// Switches the ADC onto `ch`, discards the settling conversion, then averages
// up to 32 samples. Restores the round-robin state before returning.
int32_t sampleChannelBlocking(uint8_t ch) {
  adc.setChannel(ch + 1);
  bool discarded = false;
  int64_t sum = 0; int n = 0;
  uint32_t t0 = millis();
  while (n < 32 && millis() - t0 < 2000) {
    if (adc.available()) {
      int32_t raw = adc.readRaw();
      if (!discarded) { discarded = true; continue; }   // settling conversion
      sum += raw; n++;
    }
    delay(1);
  }
  adc.setChannel(curChan + 1);
  chanSettling = true;   // resuming round-robin needs its own settling read
  return n ? (int32_t)(sum / n) : 0;
}

void doTare(uint8_t ch) {
  int32_t avg = sampleChannelBlocking(ch);
  offsetCounts[ch] = avg;
  resetAverage(ch);
  char m[24]; snprintf(m, sizeof(m), "ch%u tare done", ch + 1);
  sendLog(m);
}

void doCalibrateWeight(uint8_t ch, float knownWeight) {
  if (knownWeight == 0) { sendLog("cal: weight must be non-zero"); return; }
  int32_t avg = sampleChannelBlocking(ch);
  countsPerUnit[ch] = (float)(avg - offsetCounts[ch]) / knownWeight;
  resetAverage(ch);
  char m[32]; snprintf(m, sizeof(m), "ch%u scale factor updated", ch + 1);
  sendLog(m);
}

void reconfigure() {
  // gain / rate / LDO all need a fresh internal offset calibration, and the
  // NAU7802 keeps separate calibration registers per channel, so both need it.
  bool ok = true;
  for (uint8_t ch = 0; ch < NCHAN; ch++) {
    adc.setChannel(ch + 1);
    delay(5);
    ok &= adc.calibrateAFE(0);
    resetAverage(ch);
  }
  adc.setChannel(curChan + 1);
  chanSettling = true;
  sendLog(ok ? "AFE calibration ok (both channels)" : "AFE calibration FAILED");
}

// There's no physical battery-disconnect switch on this board, so "off" is
// software: cut the shared TFT+STEMMA-QT power rail (kills the display and
// the NAU7802 both), then drop the ESP32-S2 itself into deep sleep. Power
// draw in deep sleep is in the microamp range, as close to "off" as we get.
// The D2 button is configured as an RTC wake source, so pressing it again
// triggers a full reset that re-runs setup() from scratch, just like a
// power-on boot.
void powerOff() {
  sendLog("powering off - press D2 to wake");
  delay(50);   // let the log/serial print flush before everything dies

  digitalWrite(TFT_BACKLITE, LOW);
  digitalWrite(TFT_I2C_POWER, LOW);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // ext0 wakeup is level-triggered: if D2 is still physically held down when
  // we arm it, the wake level (HIGH) is already satisfied and the chip wakes
  // again immediately, looking like an instant reboot instead of powering
  // off. Wait for release first.
  while (digitalRead(2)) delay(5);
  delay(20);

  rtc_gpio_pulldown_en(GPIO_NUM_2);
  rtc_gpio_pullup_dis(GPIO_NUM_2);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_2, 1);   // wake on D2 going HIGH
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------------------
// WebSocket command handling
// ---------------------------------------------------------------------------
void handleCommand(uint8_t num, const char *json) {
  JsonDocument d;
  if (deserializeJson(d, json)) { sendLog("bad json"); return; }
  const char *c = d["c"] | "";
  uint8_t ch = constrain((int)(d["ch"] | 0), 0, NCHAN - 1);

  if      (!strcmp(c, "tare"))     doTare(ch);
  else if (!strcmp(c, "calw"))     doCalibrateWeight(ch, d["v"] | 0.0f);
  else if (!strcmp(c, "scale"))    { countsPerUnit[ch] = d["v"] | 1.0f; }
  else if (!strcmp(c, "offset"))   { offsetCounts[ch]  = d["v"] | 0; }
  else if (!strcmp(c, "units"))    { strlcpy(unitLabel[ch], d["v"] | "g", sizeof(unitLabel[ch])); }
  else if (!strcmp(c, "avg"))      { avgWindow[ch] = constrain((int)(d["v"] | 1), 1, 64); resetAverage(ch); }
  else if (!strcmp(c, "stream"))   { streaming = d["v"] | true; }
  else if (!strcmp(c, "gain"))     { adc.setGainIdx(d["v"] | 7); reconfigure(); }
  else if (!strcmp(c, "sps"))      { adc.setSpsIdx(d["v"] | 3);  reconfigure(); }
  else if (!strcmp(c, "ldo"))      { adc.setLdoIdx(d["v"] | 5);  reconfigure(); }
  else if (!strcmp(c, "afecal"))   { reconfigure(); }
  else if (!strcmp(c, "reset"))    { adc.reset(); adcOk = adc.powerUp(); reconfigure(); }
  else if (!strcmp(c, "wreg")) {
    uint8_t a = d["a"] | 0, v = d["v"] | 0;
    adc.writeReg(a, v);
    char m[48]; snprintf(m, sizeof(m), "wrote 0x%02X = 0x%02X", a, v);
    sendLog(m);
    sendRegs();
  }
  else if (!strcmp(c, "regs"))     { sendRegs(num); return; }
  else if (!strcmp(c, "status"))   { sendStatus(num); return; }
  else { sendLog("unknown command"); return; }

  sendStatus();
}

void wsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[ws] client %u connected\n", num);
      sendStatus(num);
      sendRegs(num);
      break;
    case WStype_TEXT:
      payload[len] = 0;
      handleCommand(num, (const char *)payload);
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// raw TCP CSV stream
// ---------------------------------------------------------------------------
void rawAccept() {
  if (!rawServer.hasClient()) return;
  for (int i = 0; i < MAX_RAW_CLIENTS; i++) {
    if (!rawClients[i] || !rawClients[i].connected()) {
      if (rawClients[i]) rawClients[i].stop();
      rawClients[i] = rawServer.available();
      rawClients[i].printf("# ms,ch,raw,units\n");
      return;
    }
  }
  rawServer.available().stop();   // full
}

void rawSend(uint32_t ms, uint8_t ch, int32_t raw, float units) {
  for (int i = 0; i < MAX_RAW_CLIENTS; i++) {
    if (rawClients[i] && rawClients[i].connected())
      rawClients[i].printf("%lu,%u,%ld,%.4f\n", (unsigned long)ms, ch + 1, (long)raw, units);
  }
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // If we're coming back from powerOff()'s deep sleep, GPIO2 was handed to
  // the RTC controller for ext0 wake and won't behave as a normal digital
  // pin again until that claim is released.
  rtc_gpio_deinit(GPIO_NUM_2);
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  Serial.printf("wake cause: %d (%s)\n", (int)wakeCause,
                wakeCause == ESP_SLEEP_WAKEUP_EXT0 ? "D2 button" : "power-on/reset");

  // power rails for the TFT and the STEMMA QT port
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);
  delay(20);

  tft.init(135, 240);
  tft.setRotation(3);
  tft.setTextWrap(false);   // an oversized value should clip, not wrap onto
                            // another row and leave unerased leftovers there

  tft.fillScreen(COL_BG);

  // "ATM" logo lockup: small-BIG-small, centered at the top. (This is a
  // sizing effect with the built-in font, not TAMU's actual wordmark font -
  // embedding that would need a real font file, which isn't practical here.)
  {
    uint8_t sSmall = 3, sBig = 5;
    int wA = 6 * sSmall, wT = 6 * sBig, wM = 6 * sSmall;
    int hSmall = 8 * sSmall, hBig = 8 * sBig;
    int total = wA + wT + wM;
    int x0 = (240 - total) / 2;
    int yBig = 4, ySmall = yBig + (hBig - hSmall) / 2;

    tft.setTextColor(COL_TAMU, COL_BG);
    tft.setTextSize(sSmall); tft.setCursor(x0, ySmall);              tft.print("A");
    tft.setTextSize(sBig);   tft.setCursor(x0 + wA, yBig);           tft.print("T");
    tft.setTextSize(sSmall); tft.setCursor(x0 + wA + wT, ySmall);    tft.print("M");
  }

  tft.setTextSize(3);
  tft.setTextColor(ST77XX_WHITE, COL_BG);
  tft.setCursor((240 - 6 * 3 * 6) / 2, 58);
  tft.print("Hello!");

  tft.setTextSize(1);
  tft.setCursor((240 - 6 * 25) / 2, 92);
  tft.print("Wireless Contact Pressure");
  delay(1400);

  tftStatic();
  tft.setTextSize(1);
  tft.setTextColor(COL_VALUE, COL_BG);
  tft.setCursor(0, 40);
  tft.println("booting...");

  // buttons: D0 is active LOW, D1/D2 are active HIGH
  pinMode(0, INPUT_PULLUP);
  pinMode(1, INPUT_PULLDOWN);
  pinMode(2, INPUT_PULLDOWN);

  Wire.begin();
  Wire.setClock(400000);

  battOk = maxlipo.begin(&Wire);
  Serial.println(battOk ? "MAX17048 ok" : "MAX17048 NOT FOUND");

  adcOk = adc.begin(&Wire);
  if (adcOk) {
    adc.setLdoIdx(DEFAULT_LDO_IDX);
    adc.setGainIdx(DEFAULT_GAIN_IDX);
    adc.setSpsIdx(DEFAULT_SPS_IDX);
    for (uint8_t ch = 0; ch < NCHAN; ch++) {   // per-channel offset cal registers
      adc.setChannel(ch + 1);
      delay(5);
      adc.calibrateAFE(0);
      resetAverage(ch);
    }
    adc.setChannel(curChan + 1);
    chanSettling = true;
    Serial.printf("NAU7802 ok, rev %u\n", adc.revision());
  } else {
    Serial.println("NAU7802 NOT FOUND - check the QT cable");
  }

  // ---- WiFi: try the saved network (Preferences, seeded from config.h on
  // first boot); if that fails, open a setup hotspot instead of just running
  // with no network at all.
  prefs.begin("wifi", false);
  String savedSsid = prefs.getString("ssid", WIFI_SSID);
  String savedPass = prefs.getString("pass", WIFI_PASS);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);                 // much lower latency for streaming
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiMode = WIFI_MODE_CONNECTED;
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", HTTP_PORT);
  } else {
    wifiMode = WIFI_MODE_PROVISION;
    Serial.println("WiFi join failed - opening setup hotspot");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    dnsServer.start(53, "*", WiFi.softAPIP());   // answer every DNS lookup with
                                                  // our own IP so most OSes pop
                                                  // up a "sign in to network" page
    Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
    tftProvisionScreen();
  }

  // ---- servers
  http.on("/", []() {
    if (wifiMode == WIFI_MODE_PROVISION) http.send_P(200, "text/html", PORTAL_HTML);
    else                                 http.send_P(200, "text/html", INDEX_HTML);
  });
  http.on("/save", HTTP_POST, []() {
    String ssid = http.arg("ssid");
    if (!ssid.length()) { http.send(400, "text/plain", "SSID required"); return; }
    prefs.putString("ssid", ssid);
    prefs.putString("pass", http.arg("pass"));
    http.send(200, "text/html", "<h1>Saved. Restarting...</h1>");
    delay(1000);
    ESP.restart();
  });
  http.on("/api/status", []() {
    JsonDocument d;
    d["ip"]   = WiFi.localIP().toString();
    d["vbat"] = vbat;
    d["pct"]  = battPct;
    JsonArray raw = d["raw"].to<JsonArray>();
    JsonArray units = d["units"].to<JsonArray>();
    for (uint8_t i = 0; i < NCHAN; i++) { raw.add(lastRaw[i]); units.add(lastUnits[i]); }
    String out; serializeJson(d, out);
    http.send(200, "application/json", out);
  });
  http.onNotFound([]() {
    if (wifiMode == WIFI_MODE_PROVISION) {
      // Captive-portal catch-all: redirect any unknown path back to "/" so
      // the OS's connectivity check gets a redirect instead of a 404, which
      // is what makes most phones/laptops pop the sign-in page automatically.
      http.sendHeader("Location", "/", true);
      http.send(302, "text/plain", "");
    } else {
      http.send(404, "text/plain", "not found");
    }
  });
  http.begin();

  ws.begin();
  ws.onEvent(wsEvent);

  rawServer.begin();
  rawServer.setNoDelay(true);

  readBattery();
  tftStatic();
  rateWindowStart = millis();
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------
void loop() {
  uint32_t now = millis();

  // ---- 1. pull every fresh conversion out of the ADC, alternating channels.
  // Each CHS switch needs one discarded "settling" conversion before the
  // reading is trustworthy (NAU7802 analog front end, per-channel cal regs).
  if (adcOk && adc.available()) {
    int32_t raw = adc.readRaw();
    if (chanSettling) {
      chanSettling = false;
    } else {
      uint8_t ch = curChan;
      lastRaw[ch]   = pushAverage(ch, raw);
      lastUnits[ch] = toUnits(ch, lastRaw[ch]);
      sampleCount++;
      if (batchN[ch] < BATCH_MAX) batchRaw[ch][batchN[ch]++] = lastRaw[ch];
      rawSend(now, ch, lastRaw[ch], lastUnits[ch]);

      curChan = (curChan + 1) % NCHAN;
      adc.setChannel(curChan + 1);
      chanSettling = true;
    }
  }

  // ---- 2. network servicing
  ws.loop();
  http.handleClient();
  rawAccept();
  if (wifiMode == WIFI_MODE_PROVISION) dnsServer.processNextRequest();

  // ---- 3. flush a batch of samples to the GUI
  if (streaming && now - tLastBatch >= WS_BATCH_MS) {
    tLastBatch = now;
    bool any = batchN[0] || batchN[1];
    if (any && ws.connectedClients()) {
      JsonDocument d;
      d["t"]  = "d";
      d["ms"] = now;
      JsonArray ch = d["ch"].to<JsonArray>();
      for (uint8_t c = 0; c < NCHAN; c++) {
        JsonObject o = ch.add<JsonObject>();
        JsonArray a = o["v"].to<JsonArray>();
        for (uint8_t i = 0; i < batchN[c]; i++) a.add(batchRaw[c][i]);
        o["o"] = offsetCounts[c];
        o["k"] = countsPerUnit[c];
      }
      String out; serializeJson(d, out);
      ws.broadcastTXT(out);
    }
    batchN[0] = 0; batchN[1] = 0;
  }

  // ---- 4. status / battery
  if (now - tLastStatus >= STATUS_MS) {
    tLastStatus = now;
    readBattery();
    float dt = (now - rateWindowStart) / 1000.0f;
    if (dt > 0) actualSps = sampleCount / dt;
    sampleCount = 0;
    rateWindowStart = now;
    if (ws.connectedClients()) sendStatus();

    // try to recover a disconnected sensor
    if (!adcOk) {
      adcOk = adc.begin(&Wire);
      if (adcOk) {
        adc.setLdoIdx(DEFAULT_LDO_IDX);
        adc.setGainIdx(DEFAULT_GAIN_IDX);
        adc.setSpsIdx(DEFAULT_SPS_IDX);
        reconfigure();
      }
    }
  }

  // ---- 5. screen
  if (now - tLastTft >= TFT_MS) { tLastTft = now; tftUpdate(); }

  // ---- 6. buttons: D0 = tare, D1 = tap:switch channel / hold 1s:screen off,
  // D2 = power off/on (deep sleep). Debounced; while the screen is blanked,
  // any button press just wakes it back up instead of doing its normal job.
  {
    static const uint32_t HOLD_MS = 1000;
    static bool p0 = false, p1 = false, p2 = false;
    static uint32_t d1PressStart = 0;
    static bool d1LongFired = false;

    bool s0 = db0.update(!digitalRead(0));
    bool s1 = db1.update(digitalRead(1));
    bool s2 = db2.update(digitalRead(2));
    bool rising0 = s0 && !p0, rising1 = s1 && !p1;
    bool falling1 = !s1 && p1, rising2 = s2 && !p2;

    if (!screenOn) {
      if (rising0 || rising1 || rising2) {
        screenOn = true;
        digitalWrite(TFT_BACKLITE, HIGH);
        tftStatic();
      }
    } else {
      if (rising0) { doTare(0); doTare(1); }

      if (rising1) { d1PressStart = millis(); d1LongFired = false; }
      if (s1 && !d1LongFired && millis() - d1PressStart >= HOLD_MS) {
        d1LongFired = true;
        screenOn = false;
        digitalWrite(TFT_BACKLITE, LOW);
        tft.fillScreen(COL_BG);
      }
      if (falling1 && !d1LongFired) { dispChan = 1 - dispChan; }   // short tap

      if (rising2) powerOff();   // never returns; wakes on next D2 press
    }
    p0 = s0; p1 = s1; p2 = s2;
  }
}
