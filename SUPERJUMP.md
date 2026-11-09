# Ocarina of Time (Switch 2) super jump: exact inputs

The jumpslash-cancel "infinite jump", done by Frame Perfect. Every number here was found by trial and error against 60 fps TV captures (2026-11-09) and confirmed with 3, 8, 16, 20 and 32 loops. "Running" frames are frames in which the game actually advances, as opposed to frames where the item wheel has it paused.

**Video:** [Super Jump in the Ocarina of Time Remake](https://www.youtube.com/watch?v=hTiaGGzQzuU). Child Link reaches the Shadow Temple entrance from the Kakariko Graveyard (v1.1.1, Switch 2). The first loop is broken down frame by frame, and the end shows a normal jump breaking the trick.

## Keys (child Link, this remake)

| Key | Does |
| --- | --- |
| ZL | lock-on / target (alone it raises the shield) |
| ZL + stick forward + A | jump slash, only with an item drawn |
| ZR | swing the equipped item (the Deku Stick) |
| ZL + ZR, mid-air, right after the stick is re-equipped | a new mid-air jump attack with its own upward push; each loop of the super jump chains one of these |
| R (held) | item wheel: the game pauses while it's open; the left stick moves the selection; releasing R equips the highlighted item |
| X | jump. **Never press it**: a regular jump caps the height until you reload from the title screen (tested: two X jumps right before the sequence, and the 8 loops gained no height) |

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

ZL (lock-on) is held from frame 3 of the opening to the end of the last loop, so the tables list only what is held besides it.

**Opening (once):** 26 frames, 433 ms (1 frame = 16.67 ms).

| Frames | Held: ZL and... | For... | Notes |
| --- | --- | --- | --- |
| 0–2 | nothing (ZL not held yet) | 3 frames | |
| 3–4 | left stick forward + **A** | 2 frames | A (not ZR) makes the jump slash, and only while the Deku Stick is drawn |
| 5–10 | left stick forward | 6 frames | wait until Link has left the ground: pausing sooner freezes him before the jump has any upward speed |
| 11–25 | **R** | 15 frames | holding R opens the item wheel, which pauses the game: this is how Link is frozen mid-air |

**Each loop:** 66 frames, 1100 ms. Frames counted from the loop start.

| Frames | Held: ZL and... | For... | Notes |
| --- | --- | --- | --- |
| 0–4 | R + left stick right | 5 frames | moves the wheel's highlight one slot right, from the Deku Stick to the Fairy Slingshot. Any other item would do: it only has to make Link put the stick away |
| 5–6 | R | 2 frames | left stick back to centre, so the flick moves exactly one slot |
| 7–15 | nothing else | **9 frames** | releasing R doesn't unpause at once: the game stays paused another 9–12 frames (equip flash, then fade), so 9 frames buys only ~3–6 running frames. The stick leaves Link's hand on the 3rd; 7 frames worked once and failed once |
| 16–30 | R | 15 frames | |
| 31–35 | R + left stick left | 5 frames | moves the highlight one slot left, back to the Deku Stick, so releasing R re-equips it. Re-equipping it mid-air is what makes another jump attack possible |
| 36–37 | R | 2 frames | |
| 38–44 | nothing else | 7 frames | no ZR yet: the game is still paused during the fade, and a ZR pressed then is ignored |
| 45–49 | **ZR** on frames 45, 47 and 49 | 5 frames | why three taps: ZR only counts as a fresh press (released → pressed) on a running frame, and the first running frame drifts by a frame or so. Three taps two frames apart make sure one lands on it |
| 50 | nothing else | 1 frame | |
| 51–65 | R | 15 frames | pause again right away: once Link passes the apex and starts falling, no jump attack lifts him again. The game should run only ~8–10 frames per loop |

## What didn't work

| Try | Result |
| --- | --- |
| ZL + A with no item drawn, or ZL + B | no jump slash (walks, or raises the shield) |
| Pausing 2 frames after A | Link frozen before leaving the ground, no height |
| Swap-off release of 4–6 frames | 1–3 running frames: the stick stays in hand |
| Swap-off release of 7 frames | worked once, failed once (the 9–12 frame resume delay varies) |
| ZR 1 frame after releasing R | swallowed while the wheel was closing |
| Double ZR at +8/+10, 3 frames of rise | ~13 running frames per loop: Link sank lower each cycle and landed |

## The sequence file ([`frame-perfect/sequences/oot_superjump.txt`](frame-perfect/sequences/oot_superjump.txt))

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

Each line holds that exact controller state for the given number of frames (`$name` = a parameter); anything not listed is released. Run it with N loops, frame-aligned, from `frame-perfect/`:

```bash
python3 tools/fp_remote.py superjump --loops 32 --claim      # add --capture to save a clip
```
