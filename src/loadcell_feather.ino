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

#include "config.h"
#include "nau7802.h"
#include "webpage.h"

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

uint32_t tLastBatch = 0, tLastStatus = 0, tLastTft = 0, tLastBtn = 0;

// --- battery
float    vbat = 0;
int      battPct = 0;

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

String pad(const String &s, int n) {
  String r = s;
  while ((int)r.length() < n) r += ' ';
  return r;
}

void tftStatic() {
  tft.fillScreen(COL_BG);
  tft.drawFastHLine(0, 20, 240, COL_LABEL);
  tft.drawFastHLine(0, 92, 240, COL_LABEL);
  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(0, 26);  tft.print("CH1");
  tft.setCursor(0, 58);  tft.print("CH2");
  tft.setCursor(0, 100); tft.print("BAT");
}

void tftUpdate() {
  char buf[40];

  // --- top bar: IP + RSSI
  tft.setTextSize(1);
  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.setCursor(0, 4);
  if (WiFi.status() == WL_CONNECTED) tft.print(pad(WiFi.localIP().toString(), 17));
  else                               tft.print(pad("connecting...", 17));
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(160, 4);
  snprintf(buf, sizeof(buf), "%4ddBm", (int)WiFi.RSSI());
  tft.print(buf);

  // --- one reading line per channel
  for (uint8_t ch = 0; ch < NCHAN; ch++) {
    tft.setTextSize(2);
    tft.setTextColor(adcOk ? COL_VALUE : COL_BAD, COL_BG);
    tft.setCursor(36, 24 + ch * 32);
    if (adcOk) {
      float v = lastUnits[ch];
      if (fabsf(v) < 1000)        snprintf(buf, sizeof(buf), "%9.2f", v);
      else if (fabsf(v) < 100000) snprintf(buf, sizeof(buf), "%9.0f", v);
      else                        snprintf(buf, sizeof(buf), "%9.1e", v);
    } else {
      snprintf(buf, sizeof(buf), "   NO ADC");
    }
    tft.print(buf);

    tft.setTextSize(1);
    tft.setTextColor(COL_LABEL, COL_BG);
    tft.setCursor(36, 42 + ch * 32);
    snprintf(buf, sizeof(buf), "%-4s raw %+9ld", unitLabel[ch], (long)lastRaw[ch]);
    tft.print(pad(String(buf), 26));
  }

  // --- battery bar
  int w = (int)(100.0f * battPct / 100.0f);
  uint16_t bc = battPct > 40 ? COL_OK : (battPct > 15 ? COL_WARN : COL_BAD);
  tft.drawRect(30, 98, 102, 12, COL_LABEL);
  tft.fillRect(31, 99, w, 10, bc);
  tft.fillRect(31 + w, 99, 100 - w, 10, COL_BG);
  tft.setTextColor(COL_VALUE, COL_BG);
  tft.setCursor(140, 100);
  snprintf(buf, sizeof(buf), "%3d%% %4.2fV", battPct, vbat);
  tft.print(buf);

  // --- bottom status
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.setCursor(0, 122);
  snprintf(buf, sizeof(buf), "x%-3u %3uSPS(%4.1f) cl:%u",
           NAU_GAIN_TABLE[adc.getGainIdx()],
           NAU_SPS_TABLE[adc.getSpsIdx()],
           actualSps,
           ws.connectedClients());
  tft.print(pad(String(buf), 30));
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

  // power rails for the TFT and the STEMMA QT port
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);
  delay(20);

  tft.init(135, 240);
  tft.setRotation(3);
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

  // ---- WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);                 // much lower latency for streaming
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", HTTP_PORT);
  }

  // ---- servers
  http.on("/", []() { http.send_P(200, "text/html", INDEX_HTML); });
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
  http.onNotFound([]() { http.send(404, "text/plain", "not found"); });
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

  // ---- 6. buttons (D0 = tare, D1 = backlight, D2 = pause stream)
  if (now - tLastBtn >= 120) {
    tLastBtn = now;
    static bool p0 = false, p1 = false, p2 = false, bl = true;
    bool b0 = !digitalRead(0), b1 = digitalRead(1), b2 = digitalRead(2);
    if (b0 && !p0) { doTare(0); doTare(1); }
    if (b1 && !p1) { bl = !bl; digitalWrite(TFT_BACKLITE, bl); }
    if (b2 && !p2) { streaming = !streaming; sendStatus(); }
    p0 = b0; p1 = b1; p2 = b2;
  }
}
