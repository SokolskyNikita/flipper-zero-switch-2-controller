# Frame Perfect (Flipper Zero app)

A wired Switch / Switch 2 controller for frame-exact input sequences. Derived from [Controller Ducky](https://github.com/eric-m-holub/controller-ducky) (Apache-2.0, commit `d63fbbe`, pinned in `../vendor/controller-ducky/`): the Pro Controller handshake, subcommand replies, fake SPI flash and report layout come from there. Changes are noted at the top of each source file.

On the Flipper: Apps → USB → **Frame Perfect** (`/ext/apps/USB/frame_perfect.fap`). Data in `/ext/apps_data/frame_perfect/`: `settings.txt`, `log.txt`, `sequences/`.

## The two tests

**Automatic test** (about 3 minutes)

1. Tells you what to set on the Switch 2 and waits for OK.
2. Tries 8 settings: Pro Controller at 8/4/2/1 ms and HORIPAD at 8/5/4/1 ms. For each, it measures for 5 s how often the console really reads it.
3. Ranks them (fastest reads; ties go to steady polling, then the Pro Controller, then the longer requested interval).
4. Connects with the best one and moves the Home menu cursor right, right, left, left. **One question:** did the cursor move? On "no" it tries the next best, up to 3.
5. Saves the confirmed setting to `settings.txt` and shows the results. OK goes to Manual test.

If nothing connects, it says what to check and offers a retry. If no setting moves the cursor, nothing is saved.

**Manual test**

- Starts from the saved recommendation. With none, it uses the defaults: Pro Controller, 8 ms (what a real Pro Controller does). The screen says which.
- Change Mode and Poll interval, Connect, or reset to the recommendation.
- Once connected:
  - **Game check (2 questions).** Tells you to open the game's pause menu with your own controller, moves its cursor, then asks whether it moved and whether your own controller still works.
  - **Live control.** Four layouts on the Flipper's keys. Hold OK for the next layout, hold Back to leave.
  - **Sequences.** Run sequence files. Up/Down picks a parameter, Left/Right changes it, OK starts or stops.
  - **Disconnect.**

## Log

`log.txt` gets one timestamped line for each of these:

- app start, with the saved setting
- every screen shown and its text, and every answer (YES / NO / OK / Back)
- each connect and disconnect, and USB events: configure, deconfigure, suspend, resume
- when streaming started and how long the handshake took
- the first 120 raw packets from the console, in hex (Pro rumble-only packets are counted, not logged), and a summary of handshake commands and subcommands
- poll statistics: every second during runs and sweeps, every 10 s while connected (rate, mean/min/max interval, 0.5 ms histogram)
- each sweep step's result, every input-check result, and the recommendation
- live button changes
- each sequence run: start (parameters, cycle length), end (cycles done, stopped or finished), and the exact time of every state change (first 64)

## Sequence files

```
fps 60
param wait 15 8 30     # name, value, min, max
2 ZL LY+ A             # hold ZL + stick forward + A for 2 frames
$wait ZL L             # duration from a parameter
40ms -                 # milliseconds; "-" = nothing pressed
loop 20                # the steps below repeat 20 times (0 = until stopped)
1 ZR
4 -
```

Inputs: `A B X Y L R ZL ZR PLUS MINUS HOME CAPTURE LS RS UP DOWN LEFT RIGHT`, sticks `LX+ LX- LY+ LY- RX+ RX- RY+ RY-` (full tilt). Each line is the complete state; anything not listed is released. `#` starts a comment.

- `sequences/oot_superjump.txt`: Ocarina of Time (Switch 2) super jump. See [SUPERJUMP.md](SUPERJUMP.md).
- `sequences/lmenu_test.txt`: timing probe (L held 30 frames opens the item menu), used by `tools/check_timing.py` and `tools/phase_sweep.py`.
- `sequences/tap_test.txt`: timing self-test.

## Remote mode (driven from a Mac over Bluetooth)

`loader open /ext/apps/USB/frame_perfect.fap remote` (or `tools/ble_rpc.py start ... remote`) starts Remote mode. The Flipper runs its own Bluetooth profile, "FP <name>", with an HID service so macOS gives it a 15 ms connection. `tools/fp_remote.py` on the Mac controls:

- **Frame grid:** period plus one frame start, on the Flipper's clock.
- **Sequences:** load, set parameters, write and list files.
- **Scheduled start:** `go` at a time T, in frame mode. Every input change reaches the console 6–14 ms into its frame.
- **Holds:** buttons and sticks.
- **USB identity:** on or off.
- **Status and logs:** status, edge log, log tail, and quit, so builds can be updated over Bluetooth.

The protocol is documented at the top of the Remote mode section in `frame_perfect.c`; the workflow is in [SUPERJUMP-RUNBOOK.md](SUPERJUMP-RUNBOOK.md).

## How the timing works

Each time the console reads a report (USB IN transfer complete, in the interrupt), the next report is built immediately. For scheduled runs it is built for the moment of the next read, one poll period ahead, so a change reaches the console at the first read at or after its scheduled time. Sequences are timed against the DWT cycle counter in µs; frames and ms are summed separately so rounding never accumulates. Frame mode maps each read to a frame of the grid the Mac sends: a read at least 6 ms into frame N carries frame N's state.

## Build and install

Developed on Momentum `mntm-dev` 93ac669b (API 87.0). The loader refuses apps built for a newer API, and Momentum's published SDKs were 87.1, so `tools/make_sdk.sh` builds a matching SDK (the `mntm-012` SDK with `api_symbols.csv` from the target firmware commit). The Bluetooth peripheral-latency switch in `fp_ble.c` locates an unexported firmware function by its code pattern; on other builds it fails safely and the link stays slower.

```bash
uv tool install ufbt
```

```bash
tools/make_sdk.sh 93ac669b /tmp/fp-sdk
```

```bash
ufbt update --local=/tmp/fp-sdk/sdk-matched.zip --hw-target=f7
```

Build and copy (Flipper on the Mac in normal USB mode):

```bash
ufbt
```

```bash
uv run --with colorlog --with pyserial python ~/.ufbt/current/scripts/storage.py -p /dev/cu.usbmodemflip_<YourFlipper>1 send dist/frame_perfect.fap /ext/apps/USB/frame_perfect.fap
```

Sequences go to `/ext/apps_data/frame_perfect/sequences/` the same way; `receive` fetches `log.txt`.

## Testing with the Mac as the host

`tools/flipper_cli.py` sends one CLI command; `tools/host_test.py` plays the console (Pro handshake, then timestamps each button change). Headless arguments:

| Arguments | Does |
| --- | --- |
| `auto mode=pro\|hori int=N seq=FILE runs=N quit=SECONDS` | connect, run a sequence, quit |
| `autotest quit` | sweep only, save the best setting unconfirmed, quit |
| `autotest answer=y\|n quit` | the full guided automatic test, answering its questions automatically (test only) |

```bash
python3 tools/flipper_cli.py 'loader open /ext/apps/USB/frame_perfect.fap auto mode=pro int=1 seq=tap_test.txt runs=2 quit=20' --no-wait
```

```bash
uv run --with hidapi python tools/host_test.py --seconds 12
```

When the app exits, the Flipper's CLI comes back and the log can be fetched. Delete `settings.txt` after Mac tests: a Mac recommendation means nothing for the Switch.

With a Mac as host: Pro and HORI both enumerate and the Pro handshake completes; macOS polls at the requested 8, 4, 2 and 1 ms (5 becomes 4); at 1 ms, holds of 1, 2, 3, 5 and 15 frames measured within ±0.04 frames. The Switch 2 ignores the requested interval and polls every 8 ms.
