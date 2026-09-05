# Contact Pressure — Wireless Strain Gauge Sensor

A WiFi-connected load cell reader built on the Adafruit ESP32-S2 Reverse TFT
Feather and a NAU7802 24-bit ADC. It reads two independent load cell / strain
gauge channels at once, shows live readings on its built-in screen, and serves
a full browser dashboard over WiFi — no app or drivers needed.

<p align="center"><i>(On its screen and web page, the device calls itself
"Contact Pressure.")</i></p>

## Features

- **Dual-channel sampling** — reads both NAU7802 inputs continuously
  (round-robin), each with independent tare, scale, offset, units, and
  averaging.
- **Browser dashboard** at `http://<board-ip>/` — live charts (real elapsed
  time on the x-axis), per-channel calibration, NAU7802 register map with
  raw read/write access, and a WiFi-free WebSocket data stream.
- **Recording** — record both channels together, timestamped by your
  computer's clock, with battery voltage and the active settings saved in
  the exported CSV's header.
- **Raw TCP CSV stream** on port 3333 for scripts/lab automation
  (`tcp_client.py`), independent of the browser GUI.
- **On-device screen** — CH1/CH2 reading (switchable), battery gauge, WiFi
  status, and a button legend, all on the 240x135 TFT.
- **Three physical buttons**:
  - **D0** — tare both channels
  - **D1** — tap to switch which channel the screen shows; hold ~1s to blank
    the screen (any button press wakes it back up)
  - **D2** — power off (deep sleep) / on. Press once to sleep, press again to
    wake — this is the closest thing to a battery disconnect switch, since
    the board has no physical power switch.
- **WiFi setup without re-flashing** — if the board can't join your network,
  it opens its own hotspot (`ContactPressure-Setup`) with a setup page, so
  you can enter new WiFi credentials from a phone without a computer or USB
  cable.

## Hardware

- Adafruit **ESP32-S2 Reverse TFT Feather** (product 5345)
- **NAU7802** 24-bit ADC (e.g. Adafruit's breakout) on the STEMMA QT / Qwiic
  port, wired to your load cell(s)
- The board's built-in **MAX17048** fuel gauge reports battery %/voltage —
  no extra wiring needed
- Optional LiPo battery on the JST connector

## Getting started

**1. Install PlatformIO** (one-time):
```bash
python3 -m venv ~/.pio-venv
~/.pio-venv/bin/pip install -U platformio
export PATH="$HOME/.pio-venv/bin:$PATH"   # add to your shell profile too
```

**2. Set your WiFi** (optional — you can also do this later from the
device's own setup hotspot):
```bash
cp src/config.h.example src/config.h
# edit src/config.h and fill in WIFI_SSID / WIFI_PASS
```
`src/config.h` is gitignored on purpose — it's the one place your real WiFi
password lives, and it never gets committed.

**3. Build and flash:**
```bash
pio run                # first run downloads the toolchain, a few minutes
pio run -t upload      # flash it (needs write access to the USB serial port —
                        # on Linux, `sudo usermod -aG dialout $USER`, then log
                        # out/in, if you get a permissions error)
```

**4. First boot:**
- If it joins your WiFi, its screen shows its IP address — open
  `http://<that-ip>/` in a browser (or `http://Contact_Pressure.local/` if
  your network supports mDNS).
- If it can't join, it opens a hotspot called **`ContactPressure-Setup`**.
  Connect to it from your phone (most devices auto-prompt a sign-in page);
  if not, open `http://192.168.4.1/` manually and enter your real network's
  SSID and password there.

## Calibrating a channel

1. With nothing on the load cell, click **Tare (zero)** for that channel.
2. Place a known weight on it, enter that weight under **Calibration**, and
   click **Set scale**.
3. Done — the live reading is now in real units.

## Reading raw data from a script

```bash
python3 tcp_client.py 192.168.1.42                       # print live to terminal
python3 tcp_client.py 192.168.1.42 --out run1.csv --seconds 60
```
This is a plain-text CSV stream (`ms,ch,raw,units`) over TCP port 3333 —
no browser or WebSocket needed, works from any language with a socket.

## Project layout

```
platformio.ini          build config (board, libraries)
src/
  loadcell_feather.ino   firmware: sensor, display, WiFi, servers, buttons
  nau7802.h              minimal NAU7802 driver (full register access)
  webpage.h              the browser dashboard (HTML/CSS/JS, served from flash)
  portal.h               the WiFi-setup hotspot page
  config.h               your WiFi + board settings (gitignored)
  config.h.example       template for config.h
tools/serial_read.py      bounded serial capture (for debugging over USB)
tcp_client.py             standalone script client for the raw CSV stream
```

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Can't flash: permission denied on the serial port | Add yourself to the `dialout` group and log out/in |
| Upload finds the wrong port / fails to connect | Pass it explicitly: `pio run -t upload --upload-port /dev/ttyACM0` |
| One channel reads noisy/erratic garbage | That input is likely floating (no load cell wired to it) — at high PGA gain, an open differential input picks up a lot of noise. This is expected, not a fault. |
| Board never shows an IP, no hotspot either | Check `src/config.h` exists and has valid syntax; check the serial log (`python3 tools/serial_read.py --seconds 15`) for boot errors |
| Battery % reads 0 or nonsense | Confirm the MAX17048 is detected (check serial boot log) — this board doesn't use a resistor-divider ADC pin for battery sensing |

For firmware-level detail (protocols, pin mappings, known quirks), see
`CLAUDE.md`.
