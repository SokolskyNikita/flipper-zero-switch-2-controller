# Frame Perfect: a frame-exact Switch 2 controller on a Flipper Zero

Frame Perfect turns a Flipper Zero into a wired Nintendo Switch / Switch 2 controller that plays input sequences with frame-exact timing, scheduled from a Mac over Bluetooth. First example: the **Ocarina of Time (Switch 2) super jump** (jumpslash cancel), tested with up to 32 consecutive cycles.

## How it works

```
LG TV (HDMI frame interrupts) ──ssh──► Mac ──Bluetooth (15 ms)──► Flipper Zero ──USB──► Switch 2 dock
                                       │                         plays the sequence from
                                       └ fp_remote.py: clock      its own µs clock, aligned
                                         sync + frame grid        to the console's 8 ms reads
```

- **Flipper app** (`frame-perfect/`): a USB Pro Controller (or HORIPAD) that plays sequence files from its own µs clock, so Bluetooth jitter never reaches the console. In **frame mode** each input change reaches the console 6–14 ms into its intended frame.
- **Mac tools** (`frame-perfect/tools/`):
  - keep a 15 ms Bluetooth link to the Flipper and estimate its clock;
  - read the console's frame timing from the TV and send the Flipper a frame grid every 0.5 s;
  - `superjump --loops N` writes, loads and runs a sequence in one command.
- **Checking attempts:** short 60 fps bursts captured on the TV, matched frame by frame to the inputs the console read.

## What was measured

- **Console polling:** the Switch 2 reads a wired controller every 8.000 ms, whatever interval the controller asks for.
- **VRR must be off** (System Settings → TV Output → VRR Output). With VRR on, gameplay frames stretch by up to 7 ms at unpredictable times. With VRR off, every frame is 16,667.0 µs (sd 1.5 µs over 5,400 frames), and a frame grid refreshed every 0.5 s predicts every frame start to within 14 µs.
- **Input to screen:** pressing L opens the item menu 3–4 frames after the frame in which the console read it. The game picks up input about 2–5 ms after each HDMI frame start, with a few ms of jitter, so presses early in a frame are a toss-up between two frames. Presses reaching the console 6–14 ms into a frame reacted on the same frame 6 of 6 times in frame mode and 10 of 12 across the timing sweeps (the other two were a frame late, console-side). Frame mode aims every change there.
- **Game-specific timings** (item wheel, swap-off, ZR): see [SUPERJUMP.md](SUPERJUMP.md).
- **Bluetooth on macOS:** an ordinary BLE accessory gets a 30 ms connection. An accessory with an HID service gets 15 ms, but with peripheral latency 22, which lets the Flipper sleep for up to 330 ms. Frame Perfect adds an HID service, and on the firmware build it was developed on it also turns peripheral latency off: Mac→Flipper median 11 ms, p99 31 ms. How macOS chooses these: [docs/macos-ble-latency.md](docs/macos-ble-latency.md).

## Requirements

- Flipper Zero. Developed on Momentum firmware (`mntm-dev` 93ac669b, API 87.0); see [frame-perfect/README.md](frame-perfect/README.md) for building against your firmware's API.
- Nintendo Switch 2 in its dock, with VRR Output off.
- A Mac (the Bluetooth tools use CoreBluetooth), with `uv` and Python 3.
- For frame-grid alignment and frame checks: a **rooted LG webOS TV**, reached over SSH. This setup is unusual; see below.

### Without a rooted TV

`fp_remote.py serve --fake-grid` sends a fixed 60 Hz grid, which is correct in rate with VRR off but has an unknown phase. Sequences then run on time alone, so individual inputs land a frame early or late more often and attempts need retries. Finding the phase without a TV, for example from a phone video of a known input, is an open item.

## Quick start (super jump)

The exact keys, timings and sequence are in [SUPERJUMP.md](SUPERJUMP.md).

1. Build and copy the app ([frame-perfect/README.md](frame-perfect/README.md)), then build the Bluetooth helper: `frame-perfect/tools/ble-app/build.sh`.
2. Put your settings in `private/config.json` (see `frame-perfect/tools/fp_config.py`).
3. Follow [SUPERJUMP-RUNBOOK.md](SUPERJUMP-RUNBOOK.md): start Remote mode, run `serve`, calibrate the clock over USB once, move the Flipper to the dock, then:

```bash
python3 frame-perfect/tools/fp_remote.py superjump --loops 32 --claim
```

## Repository layout

| Path | What |
| --- | --- |
| `SUPERJUMP.md`, `SUPERJUMP-RUNBOOK.md` | the super jump: exact inputs, and how to run it |
| `frame-perfect/` | the Flipper app (C) and sequences |
| `frame-perfect/tools/` | Mac tools: `fp_remote.py` (server + commands), `ble_rpc.py` (upload builds over Bluetooth), `attempt.py`, `phase_sweep.py`, `check_timing.py`, `host_test.py` |
| `docs/` | how macOS picks Bluetooth LE connection intervals (reverse-engineered from `bluetoothd`) |
| `private/` | git-ignored: local settings, lab notes, raw logs |

## Credits

- [Controller Ducky](https://github.com/eric-m-holub/controller-ducky) by eric-m-holub (Apache-2.0): the Pro Controller handshake, subcommand replies, fake SPI flash and report layout this app started from.
- The super jump (jumpslash cancel) was found by the Ocarina of Time speedrunning community: discovery credited to Atapi; tutorials by gymnast86, aejo and Jay Dunna ([INFINITE JUMP GLITCH EASY GUIDE](https://www.youtube.com/watch?v=cacROQ9d2xY)), whose recipe this sequence follows.
- Flipper Zero firmware: [Momentum](https://github.com/Next-Flip/Momentum-Firmware) and [flipperdevices](https://github.com/flipperdevices/flipperzero-firmware).
- TV frame capture builds on [PicCap / hyperion-webos](https://github.com/webosbrew/hyperion-webos).

## Disclaimer

Not affiliated with or endorsed by Nintendo. "Nintendo Switch", "Pro Controller" and "The Legend of Zelda" are Nintendo trademarks. The app identifies itself with the Pro Controller's USB IDs so the console accepts it, as other community controller projects do. Use it offline, in single-player games.

## Licence

Apache-2.0 ([LICENSE](LICENSE)); see [NOTICE](NOTICE).
