# Super jump runbook (Ocarina of Time, Switch 2)

How to run the Flipper-driven super jump and recover when something breaks. The inputs and why they work: [SUPERJUMP.md](SUPERJUMP.md).

## The pieces

- **Flipper:** on the Switch dock's USB, running Frame Perfect in **Remote mode**. It's the Switch's Pro Controller, and it takes commands over Bluetooth as **"FP <flipper name>"** (15 ms link, peripheral latency off).
- **Mac:** `tools/fp_remote.py serve` holds the Bluetooth link. It pings the Flipper's clock, reads the HDMI frame grid from the TV and sends it to the Flipper every 0.5 s. Other `fp_remote.py` commands talk to it. Its log is `logs/remote-*.jsonl`.
- **TV:** read-only. `serve` keeps a private tracefs instance `fp` (IRQ 134) on it while running. A capture tool on the TV (`tv_capture_dir` in `private/config.json`; PicCap's `hyperion-webos` plus our `burst` receiver) records frames for checking attempts.
- **Switch:** System Settings → TV Output → **VRR Output Off**. With VRR on, frames stretch unpredictably.

## Before each jump

1. **Hand control to the Flipper:** `python3 tools/fp_remote.py claim` (L+R twice, then A). Your own controller stays connected. Take it back with L+R, then A, on your controller.
2. **Game state:**
   - Link standing on the ground, facing where the jump should go.
   - No NPC in front: ZL targeting can turn Link toward one.
   - **Deku Stick on ZR and drawn.** If it's put away, `python3 tools/fp_remote.py hold ZR 120` swings it out.
   - In the R wheel, the Fairy Slingshot is directly right of the Deku Stick.
   - No X jump since loading from the title screen (a regular jump caps the height until you reload).
3. **Optional timing preflight,** about 20 s; opens the item menu 4 times:
   `uv run --with numpy --with pillow python tools/check_timing.py`
   It should print "TIMING OK". If it doesn't, see "Timing drifts" below.

## Run it

```bash
cd frame-perfect
python3 tools/fp_remote.py superjump --loops 16          # ~18 s; --claim to take control first
```

**Recording for a video:** add `--capture`. After the jump, it waits 5 s for Link to land (`--capture-wait`), then holds Capture for 1.5 s, and the Switch saves the last ~30 s as a clip. Use about 24 loops (~27 s) so the clip includes the start; 32 loops is ~36 s. Getting the clip off the Switch: Album → the video → Send to Smartphone (QR codes), or System Settings → Data Management → Manage Screenshots and Videos → Copy to Computer via USB (MTP, e.g. OpenMTP on the Mac).

```bash
python3 tools/fp_remote.py superjump --loops 24 --capture
```

Each loop is about 1.1 s. The command builds `sequences/superjump_xN.txt` from `sequences/oot_superjump.txt`, writes it to the Flipper, loads it and runs it in frame mode, then prints the cycles and input changes sent. `--dry-run` only writes and loads.

Turning Link between jumps: `python3 tools/fp_remote.py hold NONE 450 --stick=-1,0,0,0` (left; `1,0,0,0` right, `0,-1,0,0` back toward the camera, ~0.7 s), then `hold ZL 300` to recentre the camera.

## Check an attempt on video

```bash
uv run --with numpy --with pillow python tools/attempt.py OUTDIR --burst 6               # 960x540, ~3 s of attempt
uv run --with numpy --with pillow python tools/attempt.py OUTDIR --size 640x360 --burst 12  # longer runs
```

It prints which frame each input reached the console on, and `OUTDIR/sheet.png` shows Link cropped. Re-crop with `--reuse --start N --frames N --every N`. Link's **shadow** on the ground is the easiest height gauge. A failed swap-off shows as the stick still in Link's hand in the frames after the Slingshot wheel closes.

## When something breaks

| Symptom | Fix |
| --- | --- |
| `fp_remote.py` says "No such file" / connection refused | `serve` isn't running: `tools/ble-app/run.sh 'tools/fp_remote.py serve'` (run in the background; it reloads the clock correction from `tools/.fp_calib.json`) |
| `status` shows `bt: false` for a while | `serve` reconnects every 3 s by itself. If the Flipper app quit or crashed, start it over Bluetooth: `tools/ble-app/run.sh 'tools/ble_rpc.py start /ext/apps/USB/frame_perfect.fap remote'`, then `usb-on` and `claim` |
| "Press L+R" screen appears | Someone pressed a button on the other controller. `claim` again |
| Link didn't climb | First suspect the swap-off: record an attempt and check the stick left his hand. Then the start position: Link facing a wall, or ZL locked onto something |
| Timing drifts (preflight phases outside 6–14 ms; frame mode is the default, `go --safe-us 6000`) | The Bluetooth clock correction wanders ~0.4 ms per half hour; a new Bluetooth connection may shift it more. Best fix: put the Flipper on the Mac's USB and run `python3 tools/fp_remote.py calibrate --seconds 45` (then `usb-on` and back to the dock). Quick fix: `go --safe-us 7000` (or 5000) to shift the window |
| New Flipper build | `python3 tools/fp_remote.py update dist/frame_perfect.fap`, all over Bluetooth (~95 s at 0.7 KB/s), then `usb-on`, `claim` |
| Mac went to sleep | Bluetooth drops. `serve` reconnects, but run the preflight before trusting the timing |

## Limits and notes

- The Flipper's edge log keeps the first 64 input changes, so long runs only report totals (`cycles`, `edges` in `done`).
- `tv-capture burst` keeps frames in the TV's RAM: at most 300 MB or 15 s (14 s at 640×360, ~6 s at 960×540). It starts capturing ~1.3 s after launch.
- Updates over the stock Bluetooth link run at 0.7 KB/s.
- Keep the Mac awake during a session (`caffeinate -is`); a lid-closed MacBook still sleeps.
