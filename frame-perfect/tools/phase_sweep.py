#!/usr/bin/env python3
"""Find when the game reads input, relative to the HDMI frame grid.

Runs a TV burst and, inside it, the loaded L-menu sequence several times with
different start offsets. For each run: when the console read the L press
(Flipper edge log), and the first HDMI frame where the picture jumps (the
menu opening). Prints the delay per offset; a step of one frame between two
offsets marks the moment the game samples input.

    python3 tools/phase_sweep.py OUTDIR --offsets 0,2,4,6,8,10,12,14,16 [--burst 14]
Needs `fp_remote.py serve` running and lmenu_test.txt loaded.
"""
import argparse
import csv
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fp_remote as F  # noqa: E402

import fp_config as C  # noqa: E402
TVCAP = C.need_tvcap()

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--offsets", default="0,2,4,6,8,10,12,14,16")
ap.add_argument("--burst", type=int, default=14)
ap.add_argument("--gap", type=float, default=0.25, help="seconds between runs")
a = ap.parse_args()
offs = [float(x) for x in a.offsets.split(",")]
os.makedirs(a.out, exist_ok=True)

burst = subprocess.Popen([os.path.join(TVCAP, "tv-capture"), "burst", "640x360", str(a.burst), os.path.join(a.out, "burst")],
                         stdout=open(os.path.join(a.out, "burst.log"), "w"), stderr=subprocess.STDOUT)
time.sleep(3.0)
runs = []
for off in offs:
    r = F.client({"cmd": "go", "offset_us": int(off * 1000), "delay_ms": 300})
    runs.append({"offset_ms": off, **r})
    time.sleep(a.gap)
burst.wait()
json.dump(runs, open(os.path.join(a.out, "runs.json"), "w"))

# analyse
sys.path.insert(0, TVCAP)
import numpy as np  # noqa: E402
import burst_decode as B  # noqa: E402

bdir = os.path.join(a.out, "burst")
rows = list(csv.DictReader(open(os.path.join(bdir, "index.csv"))))
hd = [int(x["hdmi_t_ns"]) for x in rows]
recs = list(B.records(os.path.join(bdir, "burst.bin")))
means = []
for t, msg in recs:
    y, uv, w, h, sy, suv = B.nv12_from_request(msg)
    means.append(float(np.frombuffer(y, np.uint8)[: sy * h].reshape(h, sy)[:, :w].mean()))
period = 16_666_667
print(f"{'offset':>6} {'L read by console':>18} {'phase in frame':>15} {'menu frame':>11} {'delay':>9} {'frames':>7}")
res = []
for r in runs:
    if not r.get("start_tv_ns") or not r.get("edges"):
        print(f"{r['offset_ms']:6.1f} no data: {r.get('go') or r.get('error')}")
        continue
    edges = [e for e in r["edges"] if e.get("type") == "edge"]
    press = next((e["t_us"] for e in edges if e["buttons"] & 0x400000), None)
    t_press = r["start_tv_ns"] + press * 1000  # console read of L, TV clock (rates differ by ~1e-5)
    k = max(i for i in range(len(hd)) if hd[i] <= t_press) if hd[0] <= t_press else None
    if k is None:
        print(f"{r['offset_ms']:6.1f} outside the burst")
        continue
    phase = (t_press - hd[k]) / 1e6
    j = next((i for i in range(k + 1, min(len(means), k + 30)) if abs(means[i] - means[i - 1]) > 2.5), None)
    if j is None:
        print(f"{r['offset_ms']:6.1f} no menu jump found")
        continue
    delay = (hd[j] - t_press) / 1e6
    res.append((r["offset_ms"], phase, j - k, delay))
    print(f"{r['offset_ms']:6.1f} {press / 1000:15.1f} ms {phase:12.2f} ms {j:11d} {delay:7.1f} ms {j - k:7d}")
json.dump(res, open(os.path.join(a.out, "result.json"), "w"))
