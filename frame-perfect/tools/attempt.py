#!/usr/bin/env python3
"""One supervised attempt: TV burst + scheduled run of the loaded sequence,
then a contact sheet of the frames from the start, with the Flipper's input
changes marked on the frames where the console read them.

    python3 tools/attempt.py OUTDIR [--frames 60] [--burst 5] [--every 1]
"""
import argparse
import bisect
import csv
import json
import os
import re
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
ap.add_argument("--frames", type=int, default=60, help="frames to show after the start")
ap.add_argument("--burst", type=int, default=5)
ap.add_argument("--every", type=int, default=1)
ap.add_argument("--crop", default="0.30,0.22,0.70,0.98", help="area around Link as fractions x0,y0,x1,y1 ('' = whole frame)")
ap.add_argument("--size", default="960x540", help="capture size")
ap.add_argument("--reuse", action="store_true", help="redo the sheet from an existing OUTDIR")
ap.add_argument("--start", type=int, default=0, help="first frame to show")
ap.add_argument("--tile", default="240x256")
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
if a.reuse:
    r = json.load(open(os.path.join(a.out, "run.json")))
else:
  burst = subprocess.Popen([os.path.join(TVCAP, "tv-capture"), "burst", a.size, str(a.burst), os.path.join(a.out, "burst")],
                         stdout=open(os.path.join(a.out, "burst.log"), "w"), stderr=subprocess.STDOUT)
  time.sleep(2.5)
  r = F.client({"cmd": "go", "delay_ms": 300}, timeout=60)
  json.dump(r, open(os.path.join(a.out, "run.json"), "w"))
  burst.wait()

sys.path.insert(0, TVCAP)
from PIL import Image, ImageDraw  # noqa: E402
import burst_decode as B  # noqa: E402

bd = os.path.join(a.out, "burst")
h134 = []
for line in open(os.path.join(bd, "irq.txt")):
    m = re.search(r"\s(\d+)\.(\d+): irq_handler_entry: irq=134", line)
    if m:
        h134.append(int(m[1]) * 10**9 + int(m[2].ljust(9, "0")[:9]))
rows = list(csv.DictReader(open(os.path.join(bd, "index.csv"))))
hd = [int(x["hdmi_t_ns"]) for x in rows]
recs = list(B.records(os.path.join(bd, "burst.bin")))
T = r.get("start_tv_ns")
edges = [e for e in (r.get("edges") or []) if e.get("type") == "edge"]
names = {v: k for k, v in F.BUTTONS.items()}


def btn(b):
    return "+".join(n for v, n in sorted(names.items()) if b & v) or "-"


marks = {}
print(f"go {r.get('go')}")
for e in edges:
    t = T + e["t_us"] * 1000
    k = bisect.bisect_right(h134, t) - 1
    fstart = bisect.bisect_right(h134, T) - 1
    marks.setdefault(k, []).append(btn(e["buttons"] & 0xFFFFFF))
    print(f"  console read {btn(e['buttons'] & 0xFFFFFF):>12} in frame +{k - fstart:3d}, {(t - h134[k]) / 1e6:5.2f} ms into it")
f0 = bisect.bisect_right(h134, T) - 1
tiles = []
crop = [float(v) for v in a.crop.split(",")] if a.crop else None
TW, TH = (int(v) for v in a.tile.split("x"))
for n in range(a.start, a.start + a.frames, a.every):
    k = f0 + n
    if k >= len(h134) or h134[k] not in hd:
        continue
    i = hd.index(h134[k])
    img = Image.fromarray(B.to_rgb(*B.nv12_from_request(recs[i][1])))
    if crop:
        W, H = img.size
        img = img.crop((int(crop[0] * W), int(crop[1] * H), int(crop[2] * W), int(crop[3] * H)))
    img = img.resize((TW, TH))
    d = ImageDraw.Draw(img)
    d.text((3, 2), f"+{n}", fill=(255, 255, 0))
    if k in marks:
        d.text((3, 14), " ".join(marks[k]), fill=(0, 255, 255))
    tiles.append(img)
cols = max(1, min(10, 2560 // TW))
sheet = Image.new("RGB", (cols * TW, ((len(tiles) + cols - 1) // cols) * TH))
for j, t in enumerate(tiles):
    sheet.paste(t, ((j % cols) * TW, (j // cols) * TH))
sheet.save(os.path.join(a.out, "sheet.png"))
print(os.path.join(a.out, "sheet.png"))
