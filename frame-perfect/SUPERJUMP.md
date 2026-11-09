# Ocarina of Time (Switch 2) super jump: exact inputs

The jumpslash-cancel "infinite jump", done by Frame Perfect. Every number here was found by trial and error against 60 fps TV captures (2026-11-09) and confirmed with 3, 8, 16, 20 and 32 loops. 1 frame = 16.67 ms. "Running" frames are frames in which the game actually advances, as opposed to frames where the item wheel has it paused.

## Keys (child Link, this remake)

| Key | Does |
| --- | --- |
| ZL | lock-on / target (alone it raises the shield) |
| ZL + stick forward + A | jump slash, only with an item drawn |
| ZR | swing the equipped item (the Deku Stick) |
| R (held) | item wheel: the game pauses while it's open; the left stick moves the selection; releasing R equips the highlighted item |
| X | jump. **Never press it**: a regular jump caps the height until you reload from the title screen |

**Setup:**
- Deku Stick on ZR and drawn (one ZR swing).
- Fairy Slingshot directly right of the Deku Stick in the R wheel.
- Link standing on open ground.
- Switch VRR Output off.

## What the game does (measured)

| Event | Delay |
| --- | --- |
| Console reads the controller | every 8.000 ms; each input change is placed so it lands 6–14 ms into its frame |
| R pressed → wheel visible | ~6 frames (100 ms); fully open ~10 frames (167 ms) |
| R released → game runs again | **9–12 frames (150–200 ms)**: purple "equipped" flash, then a fade. Varies by about a frame |
| R pressed → game paused | ~6 frames (100 ms) |
| Slingshot selected, game running → stick leaves Link's hand | **3rd running frame** (~50 ms) |
| ZR while the wheel is still closing | **swallowed**. It must reach the game on a running frame |
| A (jump slash) → Link leaves the ground | ~8–12 frames; lands ~26 frames after A |

## The inputs

**Opening (once):** 26 frames, 433 ms.

| Frames | ms | Held |
| --- | --- | --- |
| 0–2 | 0–50 | nothing |
| 3–4 | 50–83 | ZL + forward + **A**: jump slash |
| 5–10 | 83–183 | ZL + forward: let Link leave the ground (pausing earlier freezes him on it) |
| 11–25 | 183–433 | ZL + **R**: wheel open, paused |

**Each loop:** 66 frames, 1100 ms. Frames counted from the loop start.

| Frames | ms | Held | Purpose |
| --- | --- | --- | --- |
| 0–4 | 0–83 | ZL + R + stick right | highlight the Slingshot |
| 5–6 | 83–117 | ZL + R | stick back to centre |
| 7–15 | 117–267 | ZL | **R released for 9 frames**: the game runs ~3–6 frames and the stick leaves Link's hand |
| 16–30 | 267–517 | ZL + R | paused again |
| 31–35 | 517–600 | ZL + R + stick left | highlight the Deku Stick |
| 36–37 | 600–633 | ZL + R | centre |
| 38–44 | 633–750 | ZL | R released; wait out the wheel's closing fade |
| 45, 47, 49 | 750, 783, 817 | ZL + **ZR** (1 frame each, 1 frame apart) | triple tap: whichever lands on the first running frame starts the mid-air jump attack |
| 50 | 833–850 | ZL | 1 frame of rise |
| 51–65 | 850–1100 | ZL + R | pause before the apex |

Per loop the game runs only about 8–10 frames in total, which keeps Link rising.

## What didn't work

| Try | Result |
| --- | --- |
| ZL + A with no item drawn, or ZL + B | no jump slash (walks, or raises the shield) |
| Pausing 2 frames after A | Link frozen before leaving the ground, no height |
| Swap-off release of 4–6 frames | 1–3 running frames: the stick stays in hand |
| Swap-off release of 7 frames | worked once, failed once (the 9–12 frame resume delay varies) |
| ZR 1 frame after releasing R | swallowed while the wheel was closing |
| Double ZR at +8/+10, 3 frames of rise | ~13 running frames per loop: Link sank lower each cycle and landed |

## The sequence file (`sequences/oot_superjump.txt`)

```text
fps 60
param after_a   6 1 12   # frames from A to opening the wheel (Link airborne)
param menu_wait 15 8 30  # wheel fully open
param select    5 3 10   # stick flick to change the highlighted item
param settle    2 1 6    # stick back to centre, wheel still open
param unequip   9 1 14   # R released for the swap-off (7 was on the edge)
param zr_gap    7 1 14   # frames after the release before the first ZR tap
param ascent    1 1 14   # frames after the last ZR tap before re-pausing
3 -
2 ZL LY+ A
$after_a ZL LY+
$menu_wait ZL R
loop 8
$select ZL R LX+
$settle ZL R
$unequip ZL
$menu_wait ZL R
$select ZL R LX-
$settle ZL R
$zr_gap ZL
1 ZL ZR
1 ZL
1 ZL ZR
1 ZL
1 ZL ZR
$ascent ZL
$menu_wait ZL R
```

Each line holds that exact controller state for the given number of frames (`$name` = a parameter); anything not listed is released. Run it with N loops, frame-aligned:

```bash
python3 tools/fp_remote.py superjump --loops 32 --claim      # add --capture to save a clip
```
