#!/usr/bin/env python3
"""Preflight: are scheduled inputs still landing where we think?

Loads lmenu_test.txt (L held 30 frames), runs it 4 times in frame mode inside
one TV burst (tools/phase_sweep.py) and checks every press reached the console
6-14 ms into its frame and opened the menu 4 frames later. Opens the item
menu 4 times: the Flipper must be controller 1, Link standing in a safe spot.

    uv run --with numpy --with pillow python tools/check_timing.py [OUTDIR]
Exit 0 = OK. If phases sit outside the window, the Bluetooth clock bias has
drifted: recalibrate over USB (fp_remote.py calibrate), or nudge --safe-us.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fp_remote as F  # noqa: E402

out = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="fp-check-")
ld = F.client({"cmd": "load", "name": "lmenu_test.txt"})
if not (ld.get("reply") or [{}])[0].get("ok"):
    F.client({"cmd": "write", "local": os.path.join(HERE, "..", "sequences", "lmenu_test.txt"), "name": "lmenu_test.txt"})
    F.client({"cmd": "load", "name": "lmenu_test.txt"})
r = subprocess.run([sys.executable, os.path.join(HERE, "phase_sweep.py"), out, "--offsets", "0,0,0,0", "--burst", "10"],
                   capture_output=True, text=True)
print(r.stdout[-1200:])
res = json.load(open(os.path.join(out, "result.json"))) if os.path.exists(os.path.join(out, "result.json")) else []
ok = len(res) >= 3
for off, phase, _idx, delay in res:
    frames = round((phase + delay) / 16.667)
    good = 5.0 <= phase <= 15.0 and frames == 4
    ok &= good
    print(f"  phase {phase:5.2f} ms, menu {frames} frames later {'OK' if good else 'CHECK'}")
print("TIMING OK" if ok else "TIMING CHECK FAILED (see above)")
sys.exit(0 if ok else 1)
