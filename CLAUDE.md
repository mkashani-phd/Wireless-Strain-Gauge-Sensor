# CLAUDE.md — agent notes for this project

This file is for a future Claude Code session picking this repo back up. It
covers what the human-facing README doesn't: toolchain gotchas, exact
protocols, pin-level quirks, and the things that went wrong once already so
you don't repeat them.

## Hardware

- **Board**: Adafruit ESP32-S2 Reverse TFT Feather (product 5345). Single
  core, 240 MHz, 4MB flash, 2MB PSRAM, native USB, no Bluetooth.
- **Sensor**: NAU7802 24-bit ADC on STEMMA QT/I2C (addr `0x2A`), two
  differential input channels.
- **Battery gauge**: MAX17048 fuel gauge on the *same* I2C bus (addr `0x36`)
  — **not** a resistor divider on an ADC pin. The original smoke-test-style
  code for this board family often assumes `analogReadMilliVolts(A13)` for
  battery voltage; that pin macro doesn't even exist on this board's variant
  (`pins_arduino.h` only defines A0-A5) and won't compile. Always use
  `Adafruit_MAX17048`.
- **`TFT_I2C_POWER`** gates power to *both* the display and the STEMMA QT
  rail. It must be driven `HIGH` before `Wire.begin()` or the I2C bus (and
  therefore the NAU7802 and MAX17048) will appear dead.
- **Buttons are raw GPIO0/1/2**, referred to in code and on-screen as D0/D1/D2
  (matches this board's silkscreen). D0 is active-LOW with `INPUT_PULLUP`
  (it's also the BOOT strap pin); D1/D2 are active-HIGH with
  `INPUT_PULLDOWN`. There is no D3 — don't invent a fourth button.
- There's no physical battery-disconnect switch on this board — D2's
  deep-sleep "power off" (below) is the software equivalent.

## Toolchain

- **PlatformIO**, not Arduino IDE, not `arduino-cli`.
- `platformio.ini` deliberately uses the **pioarduino** fork
  (`https://github.com/pioarduino/platform-espressif32/...`), not the
  official `platform = espressif32`. The official platform is frozen on
  Arduino core 2.x and won't build this project (core 3.x APIs are used).
  Don't "simplify" that line back — it's been hit once already.
- Board id: `adafruit_feather_esp32s2_reversetft`. If PlatformIO ever
  complains the board is unknown, run `pio boards | grep -i reverse` and use
  whatever id it prints rather than guessing.
- Build: `pio run`. Flash: `pio run -t upload`.
- **Upload port auto-detection is unreliable in practice** — it has picked
  `/dev/ttyS0`/`/dev/ttyS31` (bogus virtual ports) instead of the real
  `/dev/ttyACM0` more than once. Pass it explicitly:
  `pio run -t upload --upload-port /dev/ttyACM0`.
- **Serial port permissions**: the port is owned by `root:dialout`. If the
  invoking user isn't in `dialout`, either ask them to
  `sudo usermod -aG dialout $USER` and fully log out/in, or — if you need to
  flash *within the same shell session* without waiting for that — use
  `sg dialout -c "pio run -t upload --upload-port /dev/ttyACM0"`. Group
  membership changes don't take effect in an already-running shell/session,
  even after the user runs the `usermod` command; `sg` sidesteps that by
  re-reading `/etc/group` for a subshell.
- **Never run `pio device monitor`** — it never exits and hangs the session.
  Use `tools/serial_read.py --seconds N [--port /dev/ttyACM0]` for bounded
  reads instead.

## Things that will bite you (already did, once)

- **1200bps "touch" does NOT reset this board into the running sketch** —
  it drops the ESP32-S2 into its ROM USB-CDC download/bootloader mode
  instead (VID/PID changes from `239A:80ED` to Espressif's `303A:0002`).
  `tools/serial_read.py --reset` (which does a DTR/RTS toggle) is a no-op on
  this board's native USB and won't actually reset it either. To force a
  clean reset without reflashing, use esptool directly:
  `esptool --chip esp32s2 --port /dev/ttyACM0 --after hard-reset chip-id`.
  The normal `pio run -t upload` sequence handles this correctly on its own
  (it ends with "Hard resetting via RTS pin") — this only matters if you're
  trying to reset the board *without* reflashing.
- **Port re-enumeration takes a couple seconds after any reset/upload.**
  Don't try to reopen `/dev/ttyACM0` immediately; `sleep 2-6` first or the
  open will fail with "no such device" / "input output error".
- **Deep sleep + `ext0` wakeup wakes up instantly if the wake pin is still
  held at the trigger level when you arm it.** `esp_sleep_enable_ext0_wakeup`
  is level-triggered, not edge-triggered. If the user's finger is still on
  D2 when `esp_deep_sleep_start()` runs, the wake condition (`GPIO2 HIGH`) is
  already satisfied and the chip reboots immediately — this looked exactly
  like "every press just restarts it" when it happened. Fix: busy-wait for
  the button to be *released* (`while (digitalRead(2)) delay(5);`) before
  arming the wakeup, every time.
- **After an `ext0` wake, the wake GPIO is still claimed by the RTC IO mux**
  and won't behave correctly as a normal digital pin until you call
  `rtc_gpio_deinit(GPIO_NUM_2)` — do this early in `setup()`, unconditionally
  (harmless if the last boot wasn't from deep sleep).
- **`tft.setTextSize()` state leaks across draw calls.** A real bug this
  session: battery text inherited a leftover `setTextSize(4)` from the
  channel-value draw a few lines earlier, making it huge and wrap onto
  another row it never got cleared from. Always set the size explicitly
  right before each piece of text, don't assume it's still 1.
- **Adafruit_GFX wraps text onto the next line by default** if it hits the
  screen edge — and won't erase what it wrote there once the wrapped
  content shrinks or moves. `tft.setTextWrap(false)` is set once in
  `setup()` specifically so an oversized number clips instead of leaving
  unerasable garbage on a "phantom" row below.
- **This library has no built-in way to rotate a single piece of text**
  independent of the whole display's orientation. The "HARD REBOOT" vertical
  label (`drawVerticalLabel()` in the .ino) works by rendering into an
  off-screen `GFXcanvas1` buffer at normal orientation, then blitting it
  pixel-by-pixel with x/y transposed. This was chosen over temporarily
  calling `tft.setRotation()` mid-draw specifically because the rotation
  tables for this panel size are fiddly to get right blindly (no way to
  visually verify — see "Hardware-in-the-loop" below), and a mistake there
  risks corrupting the orientation of everything drawn after it.

## Firmware architecture (`src/loadcell_feather.ino`)

- **Dual-channel round robin**: the NAU7802 has one shared PGA/ADC behind a
  channel mux — gain/rate/LDO are chip-wide (one register, physically can't
  differ per channel), but offset/gain *calibration* registers are separate
  per channel (`OCAL1`/`GCAL1` vs `OCAL2`/`GCAL2`). The main loop alternates
  channels every conversion and **discards one "settling" conversion** after
  each switch (`chanSettling` flag) — the analog front end needs a full
  cycle to settle after `CHS` changes, and skipping this discard produces
  torn/wrong readings right after a channel switch. `reconfigure()` and the
  boot-time AFE cal both calibrate *both* channels for this reason.
- **Buttons** are debounced in `struct Debounce` (30ms stability window),
  polled every `loop()` iteration (not gated to a coarser interval — that
  was tried first and gives poor edge timing). D1 needs both rising and
  falling edges to distinguish a tap (switch displayed channel) from a
  ~1s hold (blank the screen), so it's handled with explicit
  press-timestamp tracking rather than the simple single-edge case D0/D2 use.
- **Screen blanking vs. power-off are two different things**: D1-hold sets
  `screenOn = false` (backlight off, `tftUpdate()` skipped, everything else —
  WiFi, sampling, servers — keeps running). D2 is `powerOff()`: cuts the
  shared TFT+STEMMA-QT rail, disables WiFi, and calls
  `esp_deep_sleep_start()` — a real reset on wake, not a resume. Don't
  conflate the two if asked to change one.
- **WiFi provisioning**: on boot, tries `Preferences` NVS (`prefs.begin("wifi", false)`,
  keys `"ssid"`/`"pass"`), seeded from `config.h`'s `WIFI_SSID`/`WIFI_PASS` the
  first time. If connection fails after 20s, switches to `WIFI_AP` mode,
  starts a `DNSServer` on port 53 answering every query with the AP's own IP
  (the standard captive-portal trick), and serves `portal.h`'s form at `/`.
  Submitting it (`POST /save`) writes to `Preferences` and calls
  `ESP.restart()`. `wifiMode` (`WIFI_MODE_CONNECTED` / `WIFI_MODE_PROVISION`)
  gates which page `/` and `onNotFound` serve, and whether `tftUpdate()` runs
  the normal UI or leaves `tftProvisionScreen()` (drawn once, static) alone.
- **TFT layout constants** (`LEGEND_W`, `RIGHT_COL_W`, `MID_RIGHT` in the
  .ino): a 34px legend column on the left (button functions), a 34px column
  on the right (physical reset-button reminder), and the live reading +
  status squeezed into the ~172px middle. If you change either side column's
  width, every x-position in `tftUpdate()`/`tftStatic()` that references
  `LEGEND_W`/`MID_RIGHT` needs re-checking for fit — this has broken
  (garbled/wrapped text) more than once from font-size or column-width
  changes without re-deriving the pixel budget.

## Wire protocols (if you're touching `webpage.h`, `portal.h`, or the .ino together)

**WebSocket** (port from `config.h`'s `WS_PORT`, default 81), JSON messages:
- `{"t":"s", ip, rssi, vbat, pct, adc, rev, gain, sps, ldo, sps_act, stream,
  clients, uptime, heap, ch:[{offset,scale,units,avg}, {...}]}` — status,
  `ch` is a 2-element array indexed by channel.
- `{"t":"d", ms, ch:[{v:[...raw samples...], o:offset, k:scale}, {...}]}` —
  batched data, flushed every `WS_BATCH_MS`. `v` can be empty for a channel
  in a given batch (round-robin means samples don't arrive evenly).
- `{"t":"r", v:[[addr,val], ...]}` — full register dump.
- `{"t":"log", m:"..."}` — human-readable log line, also echoed to Serial.
- Commands *to* the board: `{"c":"tare"|"calw"|"scale"|"offset"|"units"|"avg",
  "ch":0|1, "v":...}` (per-channel, `ch` defaults to 0 if omitted) and
  `{"c":"gain"|"sps"|"ldo"|"afecal"|"reset"|"wreg"|"regs"|"status"|"stream"}`
  (chip-wide, no `ch`).

**HTTP**: `GET /` (main GUI or setup portal, depending on `wifiMode`),
`GET /api/status` (`{ip, vbat, pct, raw:[ch0,ch1], units:[ch0,ch1]}`),
`POST /save` (`ssid`, `pass` form fields — provisioning only).

**Raw TCP** (port from `RAW_TCP_PORT`, default 3333): plain CSV,
`# ms,ch,raw,units` header then `ms,ch,raw,units` per line, `ch` is 1 or 2.

## Hardware-in-the-loop constraints — read this before assuming a fix worked

- You can build, flash, and read serial (bounded, via `tools/serial_read.py`)
  from this environment. You **cannot** see the TFT, press physical buttons,
  or view the rendered browser GUI.
- Every TFT layout/behavior change in this project's history has needed a
  build → flash → "please check the screen and tell me what you see" round
  trip. Don't claim a display change "looks right" — say what you changed
  and what to check for, and wait for the user's description before assuming
  success.
- Board connectivity checks (`curl http://<ip>/api/status`) confirm the
  firmware is alive and networked, but confirm nothing about what's actually
  rendered on the TFT or in the browser.
- If a board that was working suddenly won't flash, check `lsusb` for the
  Adafruit (`239A:80ED`) or bootloader (`303A:0002`) VID before assuming a
  code regression — it has genuinely dropped off USB entirely at least once
  in this project's history (loose cable / power blip), which looks like a
  tooling failure but isn't one.
