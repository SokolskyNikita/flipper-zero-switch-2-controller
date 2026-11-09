#!/usr/bin/env python3
"""Stream the Flipper's system log over the USB CLI for N seconds, keeping
lines that match a filter (default: BLE connection/GAP lines).

    python3 tools/flipper_log.py --seconds 40 [--level info] [--grep REGEX] > out.txt
"""
import argparse
import os
import re
import select
import sys
import termios
import time

ap = argparse.ArgumentParser()
ap.add_argument("--seconds", type=float, default=40)
ap.add_argument("--level", default="info")
ap.add_argument("--grep", default=r"(?i)gap|interval|phy|ble|bt|connect")
ap.add_argument("--port", default=__import__("fp_config").SERIAL_PORT)
a = ap.parse_args()

fd = os.open(a.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
t = termios.tcgetattr(fd)
t[3] &= ~(termios.ECHO | termios.ICANON)
termios.tcsetattr(fd, termios.TCSANOW, t)
os.write(fd, b"\r\n")
time.sleep(0.5)
os.write(fd, f"log {a.level}\r\n".encode())
pat = re.compile(a.grep)
buf = b""
end = time.time() + a.seconds
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try:
            buf += os.read(fd, 4096)
        except (BlockingIOError, OSError):
            continue
        *lines, buf = buf.split(b"\n")
        for ln in lines:
            s = ln.decode(errors="replace").strip()
            if s and pat.search(s):
                print(f"{time.strftime('%H:%M:%S')} {s}", flush=True)
os.write(fd, b"\x03")  # Ctrl-C ends `log`
time.sleep(0.3)
os.close(fd)
