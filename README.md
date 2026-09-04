# ESP32-S2 Reverse TFT Feather — bootstrap and smoke test

**This file is written for Claude Code.** Drop it into an empty folder, open
that folder, and say *"read the README and get the board running."* Everything
needed is below: toolchain install, project scaffolding, a test program, build,
flash, and verification. Work through the phases in order.

---

## Hardware

- Adafruit **ESP32-S2 Reverse TFT Feather** (product 5345), connected by USB-C
- 240x135 ST7789 display on the back, three buttons (D0, D1, D2)
- STEMMA QT / Qwiic I²C port — an **NAU7802** load-cell ADC may be plugged in
- Optional LiPo on the JST connector

Single-core ESP32-S2 at 240 MHz, 4 MB flash, 2 MB PSRAM, native USB, **no
Bluetooth**.

---

## Phase 0 — Check the environment

```bash
uname -s                                    # Darwin | Linux | (MINGW on Windows)
python3 --version                           # need 3.10+
ls /dev/tty.usbmodem* /dev/ttyACM* 2>/dev/null   # is the board plugged in?
```

On Linux, flashing needs write access to the port. If the user is not in
`dialout`, tell them to run `sudo usermod -aG dialout $USER` and log out and
back in. **Do not run `sudo` yourself.**

If no port appears at all, stop and ask the human to plug the board in. Do not
proceed to Phase 3 without a port.

---

## Phase 1 — Install PlatformIO

Check first, install only if missing:

```bash
pio --version || {
  python3 -m venv ~/.pio-venv
  ~/.pio-venv/bin/pip install -U platformio
  echo 'export PATH="$HOME/.pio-venv/bin:$PATH"' >> ~/.bashrc
}
export PATH="$HOME/.pio-venv/bin:$PATH"
pio --version
```

Use a virtualenv rather than `pip install --user` or a system install — several
distributions ship an externally-managed Python that refuses the latter. If the
user has `pipx`, `pipx install platformio` is equally fine.

Tell the human to add `~/.pio-venv/bin` to their PATH permanently (or to use
`~/.pio-venv/bin/pio`) so the command survives a new shell.

---

## Phase 2 — Scaffold the project

Create exactly this layout in the current folder:

```
platformio.ini
src/main.cpp
tools/serial_read.py
```

### `platformio.ini`

```ini
[env:feather_s2_reversetft]
; The OFFICIAL platformio espressif32 platform is frozen on Arduino core 2.x
; and will not build this. pioarduino is the community fork tracking core 3.x.
; Do not "simplify" this line back to platform = espressif32.
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board     = adafruit_feather_esp32s2_reversetft
framework = arduino

build_flags =
  -DARDUINO_USB_CDC_ON_BOOT=1     ; Serial goes over native USB
  -DBOARD_HAS_PSRAM
  -DCORE_DEBUG_LEVEL=1

lib_deps =
  adafruit/Adafruit GFX Library
  adafruit/Adafruit ST7735 and ST7789 Library
  adafruit/Adafruit BusIO

monitor_speed   = 115200
monitor_filters = esp32_exception_decoder, time
upload_speed    = 921600
```

If the build reports an unknown board, run `pio boards | grep -i reverse` and
substitute the ID it prints. Do not guess a different one.

### `src/main.cpp` — the smoke test

This exercises every subsystem at once: display, the QT power rail, I²C,
battery ADC, buttons, and serial. If this runs, the hardware is good.

```cpp
// Smoke test for the Adafruit ESP32-S2 Reverse TFT Feather.
// Prints an I2C scan and battery voltage to serial and to the TFT.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

uint32_t lastScan = 0;
int      devCount = 0;
char     devList[64] = "scanning...";

void scanI2C() {
  devCount = 0;
  devList[0] = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      devCount++;
      char b[8];
      snprintf(b, sizeof(b), "%s0x%02X", devCount > 1 ? " " : "", a);
      strlcat(devList, b, sizeof(devList));
      Serial.printf("I2C device at 0x%02X%s\n", a,
                    a == 0x2A ? "  <- NAU7802" : "");
    }
  }
  if (!devCount) strlcpy(devList, "none found", sizeof(devList));
}

float batteryVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(A13);
  return (mv / 8.0f) * 2.0f / 1000.0f;   // on-board 100k/100k divider
}

void setup() {
  Serial.begin(115200);

  // TFT_I2C_POWER gates BOTH the display and the STEMMA QT rail.
  // Without this the I2C scan finds nothing. Must come before Wire.begin().
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);
  delay(20);

  tft.init(135, 240);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);

  pinMode(0, INPUT_PULLUP);     // D0 is active LOW
  pinMode(1, INPUT_PULLDOWN);   // D1 and D2 are active HIGH
  pinMode(2, INPUT_PULLDOWN);

  analogReadResolution(12);
  Wire.begin();
  Wire.setClock(400000);

  Serial.println("\n=== Feather ESP32-S2 Reverse TFT smoke test ===");
  Serial.printf("chip %s, %u MHz, flash %u MB, PSRAM %u KB\n",
                ESP.getChipModel(), ESP.getCpuFreqMHz(),
                ESP.getFlashChipSize() / (1024 * 1024),
                ESP.getPsramSize() / 1024);
  scanI2C();
}

void loop() {
  if (millis() - lastScan > 3000) { lastScan = millis(); scanI2C(); }

  float v = batteryVolts();
  bool b0 = !digitalRead(0), b1 = digitalRead(1), b2 = digitalRead(2);

  tft.setTextSize(2);
  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setCursor(0, 6);
  tft.print("Feather S2 OK ");

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(0, 34);  tft.printf("uptime  %6lus   ", millis() / 1000);
  tft.setCursor(0, 50);  tft.printf("battery %5.2f V   ", v);
  tft.setCursor(0, 66);  tft.printf("I2C     %d dev     ", devCount);
  tft.setCursor(0, 82);  tft.printf("        %-28s", devList);
  tft.setCursor(0, 104); tft.printf("buttons D0:%d D1:%d D2:%d",
                                    b0 ? 1 : 0, b1 ? 1 : 0, b2 ? 1 : 0);

  Serial.printf("up=%lus batt=%.2fV i2c=%d [%s] btn=%d%d%d\n",
                millis() / 1000, v, devCount, devList, b0, b1, b2);
  delay(500);
}
```

### `tools/serial_read.py`

`pio device monitor` **never exits** and will hang the session. Always read
serial through this instead.

```python
#!/usr/bin/env python3
"""Capture USB serial for N seconds, then exit. Usage:
     python3 tools/serial_read.py --seconds 15 [--port /dev/ttyACM0] [--reset]
"""
import argparse, sys, time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pip install pyserial")

def guess_port():
    ports = list(list_ports.comports())
    for p in ports:                       # Espressif native-USB vendor ID
        if p.vid == 0x303A: return p.device
    for p in ports:
        if "ACM" in p.device or "usbmodem" in p.device: return p.device
    return ports[0].device if ports else None

a = argparse.ArgumentParser()
a.add_argument("--port"); a.add_argument("--baud", type=int, default=115200)
a.add_argument("--seconds", type=float, default=10)
a.add_argument("--reset", action="store_true")
a = a.parse_args()

port = a.port or guess_port()
if not port: sys.exit("no serial port found; is the board plugged in?")
print(f"# {port} @ {a.baud} for {a.seconds:g}s", file=sys.stderr)

with serial.Serial(port, a.baud, timeout=0.2) as s:
    if a.reset:
        s.setDTR(False); s.setRTS(True); time.sleep(0.1)
        s.setRTS(False); time.sleep(0.2); s.reset_input_buffer()
    end = time.time() + a.seconds
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace")); sys.stdout.flush()
```

Install pyserial if it is missing: `~/.pio-venv/bin/pip install pyserial`, and
then call the script with that interpreter.

---

## Phase 3 — Build, flash, verify

```bash
pio run                      # first run downloads the toolchain, several minutes
pio device list              # confirm the port
pio run -t upload
python3 tools/serial_read.py --seconds 12
```

**Success looks like** a chip identification line, then repeating status lines.
If an NAU7802 is plugged into the QT port, the scan reports `0x2A`.

Report the captured serial output to the human and ask them to confirm the
screen shows `Feather S2 OK` with a live-updating uptime, and that pressing the
buttons flips the `D0/D1/D2` digits.

---

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `A fatal error occurred: Could not open /dev/ttyACM0` | user not in `dialout`, or another program holds the port. Close any monitor. |
| Upload starts then fails to connect | Ask the human to hold **BOOT (D0)**, tap **RESET**, release BOOT. A new port appears; upload to that, then tap RESET. **You cannot do this yourself.** |
| Port vanishes after upload | Normal. Native USB re-enumerates. Wait ~2 s before reading serial. |
| I²C scan finds 0 devices | `TFT_I2C_POWER` not driven HIGH, or the QT cable is not seated. Check the pin is set before `Wire.begin()`. |
| Screen stays black | `TFT_BACKLITE` not HIGH, or `tft.init(135, 240)` has the arguments swapped. |
| Battery reads 0 V or nonsense | Wrong ADC pin for this board revision. Some Feathers instead carry a **MAX17048 fuel gauge at I²C 0x36** — if the scan shows `0x36`, use that rather than the divider. |
| `Unknown board ID` | `pio boards \| grep -i reverse` and use the printed ID. |
| Compile errors about core 3.x APIs | The `platform =` line was changed to plain `espressif32`. Restore the pioarduino URL. |

---

## Rules

- **Use the board's pin macros** — `TFT_CS`, `TFT_DC`, `TFT_RST`, `TFT_BACKLITE`,
  `TFT_I2C_POWER`, `A13` — never raw GPIO numbers. The macros come from the
  board variant and stay correct across core updates.
- **`TFT_I2C_POWER` must be HIGH before `Wire.begin()`.** This is the single
  most common reason a QT sensor appears dead on this board.
- Single core, no Bluetooth. Nothing in `loop()` may block for long.
- Never run `sudo`. If a step needs it, tell the human what to run.
- Never run an unbounded serial monitor.
- Do not commit WiFi credentials. Ask before writing real ones into a file.

## What you cannot verify

You can compile, flash, and read serial. You **cannot** see the TFT, press the
board's buttons, hold BOOT/RESET, check the load-cell wiring, or reach anything
the board serves over WiFi. When a change touches those, state what the human
should look for instead of declaring it working.

---

## Next step

Once the smoke test passes, the hardware and toolchain are proven. The real
project reads a load cell through the NAU7802 at 0x2A, shows the value plus IP
address and battery percentage on the TFT, and serves a browser GUI over WiFi
with a WebSocket data stream and direct NAU7802 register editing. Ask the human
for that source, or build it up from here.
