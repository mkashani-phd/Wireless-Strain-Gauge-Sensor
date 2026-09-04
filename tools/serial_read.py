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
