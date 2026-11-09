#!/usr/bin/env python3
"""Send one command to the Flipper CLI and print the reply.
   python tools/flipper_cli.py 'loader open /ext/apps/USB/frame_perfect.fap auto ...' [--no-wait]"""
import os, select, sys, termios, time
port = __import__("fp_config").SERIAL_PORT
cmd = sys.argv[1]
wait = "--no-wait" not in sys.argv
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd); a[3] &= ~(termios.ECHO | termios.ICANON); termios.tcsetattr(fd, termios.TCSANOW, a)
def rd(t):
    out = b""; end = time.time() + t
    while time.time() < end:
        try:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r: out += os.read(fd, 4096)
        except OSError:
            break
    return out
os.write(fd, b"\r\n"); rd(0.8)
os.write(fd, cmd.encode() + b"\r\n")
out = rd(2.0 if wait else 0.5)
try: os.close(fd)
except OSError: pass
print(out.decode(errors="replace"))
