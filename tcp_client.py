#!/usr/bin/env python3
"""
Minimal client for the board's raw TCP stream (port 3333).

The browser GUI is the main interface; this is here for scripted logging,
lab automation, or piping samples into numpy/pandas.

    python3 tcp_client.py 192.168.1.42
    python3 tcp_client.py 192.168.1.42 --out run1.csv --seconds 60

Only the standard library is used, so it runs anywhere Python 3 runs.
"""

import argparse
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", help="board IP address, e.g. 192.168.1.42")
    ap.add_argument("--port", type=int, default=3333)
    ap.add_argument("--out", help="write CSV to this file")
    ap.add_argument("--seconds", type=float, default=0,
                    help="stop after N seconds (0 = run until Ctrl-C)")
    a = ap.parse_args()

    sink = open(a.out, "w", buffering=1) if a.out else None
    stop = time.time() + a.seconds if a.seconds else None

    with socket.create_connection((a.host, a.port), timeout=10) as s:
        s.settimeout(1.0)
        buf = b""
        n = 0
        print(f"connected to {a.host}:{a.port}  (Ctrl-C to stop)", file=sys.stderr)
        try:
            while True:
                if stop and time.time() > stop:
                    break
                try:
                    chunk = s.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").strip()
                    if not text:
                        continue
                    if sink:
                        sink.write(text + "\n")
                    if text.startswith("#"):
                        print(text, file=sys.stderr)
                        continue
                    n += 1
                    ms, raw, units = text.split(",")
                    # replace this with whatever processing you need
                    print(f"\r{n:8d}  raw={int(raw):+9d}  {float(units):+12.4f}",
                          end="", file=sys.stderr)
        except KeyboardInterrupt:
            pass
        finally:
            print(f"\n{n} samples", file=sys.stderr)
            if sink:
                sink.close()


if __name__ == "__main__":
    main()
