#!/usr/bin/env python3
"""Summarise bluetoothd's per-second LE link counters between two times.

bluetoothd logs a line like this for each LE link every second:
  Le [0x4d]: time 4, coex 0, rssi -61, tx [S= 41:F= 14], rx [S= 55:F= 0], 1M {...}, 2M {...}
tx = packets the Mac sent (S acknowledged, F not), rx = packets it received.

    tools/ble_linkstats.py "2026-11-09 05:07:40" "2026-11-09 05:09:52"
"""
import re
import subprocess
import sys

start, end = sys.argv[1], sys.argv[2]
out = subprocess.run(
    ["/usr/bin/log", "show", "--start", start, "--end", end, "--info", "--debug",
     "--predicate", 'process == "bluetoothd" AND eventMessage CONTAINS "Le [0x"'],
    capture_output=True, text=True).stdout
pat = re.compile(r"Le \[(0x\w+)\]: time +(\d+), coex +(\d+), rssi (-?\d+), tx \[S= *(\d+):F= *(\d+)\], rx \[S= *(\d+):F= *(\d+)\], 1M \{rx (\d+), tx (\d+)\}, 2M \{rx (\d+), tx (\d+)\}")
links = {}
for line in out.splitlines():
    m = pat.search(line)
    if not m:
        continue
    a = links.setdefault(m[1], [0] * 9)
    for i, v in enumerate([1, int(m[5]), int(m[6]), int(m[7]), int(m[8]), int(m[4]), int(m[3]),
                           int(m[9]) + int(m[10]), int(m[11]) + int(m[12])]):
        a[i] += v
for h, (n, ts, tf, rs, rf, rssi, coex, p1, p2) in links.items():
    print(f"link {h}: {n} s, Mac tx {(ts + tf) / n:.1f}/s failed {100 * tf / max(1, ts + tf):.1f}%, "
          f"Mac rx {(rs + rf) / n:.1f}/s failed {100 * rf / max(1, rs + rf):.1f}%, rssi {rssi / n:.1f}, coex {coex}, "
          f"packets 1M {p1} / 2M {p2}")
