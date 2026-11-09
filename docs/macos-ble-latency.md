# How macOS picks a Bluetooth LE connection interval

How a Mac sets the connection interval, peripheral latency and supervision timeout for a Bluetooth LE accessory, reverse-engineered from `bluetoothd`.

- **Binary:** `/usr/sbin/bluetoothd`, arm64e slice, macOS 27.0.1 (26A434)
- **Mac:** MacBook Pro M4 Pro, Broadcom BCM4388C2 controller
- **Date:** 2026-11-09

**Scope.** Third-party LE accessories with the Mac as central. Apple accessories and legacy paths appear only where they change the outcome for third-party devices. Later macOS versions may behave differently.

"Observed" marks behaviour seen in this Mac's logs. Everything else comes from the code.

## Contents

1. [Basics](#1-basics)
2. [Overview](#2-overview)
3. [At connect time](#3-at-connect-time)
4. [Connection profiles](#4-connection-profiles)
5. [App requests](#5-app-requests)
6. [Accessory requests](#6-accessory-requests)
7. [HID devices](#7-hid-devices)
8. [Status flags that are not inputs](#8-status-flags-that-are-not-inputs)
9. [What a third-party accessory gets](#9-what-a-third-party-accessory-gets)
10. [Reading the log](#10-reading-the-log)
11. [Workarounds and other platforms](#11-workarounds-and-other-platforms)
12. [Open questions](#12-open-questions)

## 1. Basics

- **The Mac decides.** The central sets the parameters. The accessory can only request others, and the Mac may accept, change or refuse them. The central also picks the PHY: a Flipper Zero's `HCI_LE_Set_PHY(1M)` was accepted by its own controller, but the link stayed on 2M (observed).
- **Units.** Intervals are counts of **1.25 ms** (6 = 7.5 ms, 9 = 11.25 ms, 12 = 15 ms, 24 = 30 ms). Supervision timeouts are counts of 10 ms. Some `bluetoothd` log lines print raw counts, others milliseconds.
- **A range, not a number.** `bluetoothd` sends the controller a standard HCI LE Connection Update (min/max interval, peripheral latency, timeout, CE length). The controller firmware picks the interval inside the range. Observed: this Mac's Broadcom controller always picks the **maximum**, so 10–30 ms becomes 30 ms. Only min = max gives an exact interval.
- **Peripheral latency** is how many connection events the accessory may skip when it has nothing to send. It never delays data from the accessory, which can send at any event. It can delay data from the Mac by up to (latency + 1) × interval, but only if the accessory actually skips events. A peripheral can choose not to skip, ignoring the granted latency locally (on the STM32WB55, `ACI_HAL_SET_PERIPHERAL_LATENCY`); macOS keeps reporting the granted value, e.g. 22 (observed).
- **No public control.** CoreBluetooth gives a central app no supported way to set any of this.

## 2. Overview

```
connect ─► initial range from the chip generation (3)      10–30 ms here → 30 ms
   │
   ├─ an app sets a latency level (5)                       best level "low" = 10–30 ms
   ├─ the accessory requests parameters (6)                 no config: min ≥ 15 ms, max ≥ min + 15 ms, or exactly 15/15
   │
   └─ the accessory exposes an HID service (7)
         one mode for all HIDs (7.2) ─► per-device interval (7.3) ─► LEHID profile (4)
         15 / 11.25 / 7.5 ms, recomputed when HIDs, audio or Game Mode change
```

## 3. At connect time

When `bluetoothd` creates an LE connection, it picks the starting range:

| Case | Range |
| --- | --- |
| A device in the "requires low latency" set is waiting to connect (5) | fixed 15 ms, or the controller default on one special pairing path |
| A device of internal type 18 is waiting to connect, and a controller capability check fails | fixed 28.75 ms |
| Chip generation 1–20 | 7.5–15 ms |
| Any other chip generation | **10–30 ms** |

This Mac's controller is `BCM_4388C2`, number 24 in `bluetoothd`'s chip table. Observed: "overriding macOS connection interval to 10ms min and 30ms max", then 30 ms.

## 4. Connection profiles

After connect, every change is applied as a **named profile** through `setConnectionLatency`, logged as `Setting connection latency to "<name>" for device …`. The table has 28 entries. Those a third-party device can reach:

| Profile | Interval | Peripheral latency | Timeout | CE length | Subrating | Used for |
| --- | --- | --- | --- | --- | --- | --- |
| `low` | 10–30 ms | 0 | 2000 ms | – | no | app level 0 (5) |
| `medium` | 100–120 ms | 1 | 2000 ms | – | no | app level 1 |
| `high` | 290–320 ms | 1 | 2000 ms | – | no | app level 2 |
| `very-high` | 500–600 ms | 1 | 3600 ms | – | no | app level "very high" |
| `LEHID-15ms` | 15 ms | 22 | 2000 ms | 4 | yes | HID at 15 ms (7.3), observed |
| `LEHID-11.25ms` | 11.25 ms | 30 | 2000 ms | 4 | yes | HID at 11.25 ms |
| `LEHID-7.5msCE4` | 7.5 ms | 44 | 2000 ms | 4 | yes | HID at 7.5 ms |
| `LEHID-7.5msCE2` | 7.5 ms | 44 | 2000 ms | 2 | yes | HID at 7.5 ms, Logitech or game controller |

- **HID profiles set min = max**, so an HID gets exactly the planned interval.
- **Worst case from Mac to device** with these latencies: 345 ms at 15 ms, 349 ms at 11.25 ms, 338 ms at 7.5 ms, and only if the device skips events.
- **Subrating** (Bluetooth 5.3, profiles marked "yes"):
  - Each time `setConnectionLatency` applies such a profile to an HID on a platform that supports subrating ("Enable Subrating set to 1"), a request is queued. It's sent after the locally started parameter update completes, or at once if the parameters are already at target ("Proceeding with subrating mode"). So it's re-requested every time an LEHID profile is applied.
  - Values: subrate factor 1–6, max latency 1, continuation 1. An idle link can then run at up to 6 × the base interval, returning to the base interval when data flows.
  - `bluetoothd` doesn't check the peer's features first. If the peer can't subrate, the request fails ("Remote device does not support connection subrating", then "Failed to enter Connection Subrating Mode") and nothing changes. A Flipper Zero (STM32WB55) reports `SupportsSubrating: 0` and fails this way harmlessly (observed).
  - Check: the "Remote Features" line at connect shows `SupportsSubrating: 0/1`; "Connection parameters … Subrate Factor: N" shows the factor in use.
- **Other entries:** `midi`, `midi v2` (12); `super-low`, `low-fixed`, `mid-fixed`, `very-low`, `power-optimize`; Apple-only `magnet*`, `pencil-*`, `continuity*`, `homekit`, `leAudio*`.
- **`LEHID-5ms`** exists, but its 5 ms values are overwritten by the generic 10–30 ms defaults, so it's effectively disabled.

## 5. App requests

An app can call the hidden `-[CBCentralManager setDesiredConnectionLatency:forPeripheral:]`. `bluetoothd` accepts it without an entitlement check and applies `low`, `medium` or `high` (4). **`low`, 10–30 ms, is the best an app can get**, and on this Mac it ends at 30 ms (observed).

- **Several apps.** The combined level is the one with the smallest minimum interval, ignoring "opportunistic" requests. Logged as `combinedLatencyForDevice returning …`.
- **Per-device config.** Applying a profile also stores a per-device config in the LE connection manager, which accessory requests (6) are checked against.
- **When an app's session ends** ("Unregistering session …"), the combined level is recomputed from the remaining apps and re-applied. With no app left the result is `low`, so the device keeps a `low` config for the rest of the connection. Restorable sessions are kept.
- **When the device disconnects**, the LE disconnect handler ("LE ConnManager disconnection complete") erases the per-device config. The app's request stays in its session but **isn't re-applied on reconnect**: the connection-complete handler calls `setConnectionLatency` only for iCloud-paired and Find My devices. The level returns only when something recomputes it (the app or another app asks, a session ends or a session's connection options change). So each connection starts without a config; HIDs get their LEHID profile once the HID mode runs (7.3).
- **Level values can't be changed on retail macOS.** `LeConnectionLatency{Low,Medium,High}{Min,Max}Interval` / `…SupervisionTimeout` keys in a nested `LE` dictionary in `com.apple.MobileBluetooth.debug` would override them, but the loader runs only when `os_variant_has_internal_diagnostics("com.apple.bluetooth")` is true. Observed: the "Override latency" line never appears.
- **"Requires low latency" is out of reach.** This separate per-device set gives a fixed 15 ms at connect (3). The connect option `RequiresLowLatency` reaches `bluetoothd` from an ordinary app, but the flag isn't set (observed); where it's dropped is unknown.

## 6. Accessory requests

When the accessory sends a connection parameter request (L2CAP or link layer), `bluetoothd` checks it in this order.

**1. Refused outright**
- any interval outside 7.5 ms–4 s
- min greater than max
- peripheral latency over 500
- a timeout outside 100 ms–32 s, or over 18 s when the accessory is the peripheral

**2. Adjusted, if the device has no per-device config**
- peripheral latency of 31 or more is refused
- a min below **15 ms** is raised to 15 ms ("… smaller than 15 ms. Scaling up")
- if max − min is under 15 ms, max becomes min + 15 ms ("Intervals should be 15 ms apart")
- exception: a request of exactly **15/15 ms** passes unchanged

**3. Refused if the link would be fragile**
- max over 2 s
- max × (latency + 1) over 6 s
- a timeout shorter than 3 × max × (latency + 1)

**4. With a per-device config** (from an app level (5) or an HID profile (7.3))
- **Low-latency pointers** (7.4) in HID mode `DEFAULTS` or `15_MS_ALL` get exactly what they asked for: "allow LE Connection(Mouse/Pointing) Parameter Override by Peer … Using peer's configuration". The post-update check (7.6) then pushes them back unless the result is 7.5 ms.
- **Everyone else:**
  - a min below the config's minimum is refused
  - if the min is at or below the config's preferred maximum, max is set to that maximum. Observed: "Using 24 as the max interval" turned a 15/15 ms request into 15–30 ms (so 30 ms) under the `low` level.
  - for HID configs (which carry the same peer flag the pointer rule checks), a min above the preferred maximum is accepted only if a multiple of that maximum fits inside the requested range

**5. The accepted response** carries:
- min and max as adjusted above
- **the peer's own peripheral latency and timeout, unchanged**
- the CE length from the per-device config, or at least 6 slots when the "preserving minCE length to 3.75ms" host workaround applies

The new latency lasts until the profile is applied again. The HID-mode pass (7.3) re-applies the profile when the device's interval or CE length differs from the plan; it doesn't compare latency. So a CE-length mismatch (the 6-slot workaround against the profile's 4) brings latency 22 back on the next mode recompute (7.2).

**Also**
- `DisableConnectionParameterUpdates` in the `LE` debug settings refuses every request. It's read through the same debug-settings object as the level overrides (5), so it's probably internal-only too.
- A BLE-MIDI device's request is refused while low-latency HID devices are connected.
- The result is logged as `Accepting/Rejecting following parameters: min=… max=…`.

**Consequences (code only, not tested live):**
- A generic accessory gets **exactly 15 ms** by requesting 15/15 ms, provided no app set a latency level for it during the current connection. With a `low` config it ends at 30 ms (step 4, observed).
- An HID can slow itself down in steps of its planned interval (15 → 30 → 45 ms) but can't go below the plan.
- An HID can lower its peripheral latency, down to 0, by requesting its planned interval with that latency (e.g. 15/15 ms, latency 0 instead of `LEHID-15ms`'s 22).

## 7. HID devices

The only path where third-party accessories reliably get below 30 ms.

### 7.1 Entry: the HID tag, appearance and PnP ID

**The `_HID_DEVICE_` tag.** Each device record holds a set of string tags. `_HID_DEVICE_` is added when GATT discovery finds a **primary service** (declaration 0x2800) with UUID **0x1812**. The same step starts mouse identification (8) and registers the device with the LE connection manager. Every later HID step checks the tag; a device without it stays on the generic path (3–6). Other services alongside 0x1812 don't matter: a Flipper Zero with 0x1812 plus a custom serial service got `LEHID-15ms` (observed).

The log line **"Device … does not have \_HID\_SERVICE\_"** means this check failed. It comes from the functions that set or read the Compatible/Incompatible flag (8) and from the "Updating LE HID Device Types" step.

**How a device ends up without the tag.** `bluetoothd` never removes `_HID_DEVICE_` from a live record: its only setter is always called in "add" mode. A device lacks it when:
- **Its record was replaced or recreated.** A new record stored for a UUID that already has one copies only a few fields (IDS address and a couple of flags), so it starts with **no tags**. Records are also erased when the device cache is full (only records that aren't connected, connecting, LE-paired, classic-paired, an unexpired Find My device or a fast-LE-connection device are evicted: "Device … evicted"), or when a device is deleted or unpaired. The new record gets the tag only after GATT discovery finds the HID service again.
- **A privileged client removed it.** The entitlement-checked XPC calls `_xpcCBDiscoveryDeviceUntag:` and `_xpcCBDiscoveryDeviceRemoveAllTags:`, and CoreBluetooth sessions with a tagging capability flag, can remove any tag. Which entitlement grants that session flag is unknown.
- **GATT discovery hasn't finished** on a new connection.

**Appearance** (device record offset 416) is set only from:
- the **Appearance field in the advertising data** (AD type 0x19), in the advertisement handler
- saved device records (`CBNVRAMDevice`)
- `APPEARANCE_*` tags
- the game-controller relabel (7.5) and two hardcoded audio cases

No other code writes that offset, so **the GAP Appearance characteristic isn't used**. An accessory that leaves appearance out of its advertisement is logged as `appearanceValue(0x0)` (observed).

**The PnP ID** characteristic (vendor ID source, VID, PID, version) supplies VID and PID. When it's missing, the log shows `isLowLatencyBLEHID: device is NULL`.

### 7.2 One mode for all connected HIDs

`adjustHIDSniffInterval` looks at every connected Bluetooth HID, classic and LE, and picks one mode for all of them. Modes are logged with a `MULTI_HID_CONFIG_` prefix (e.g. `MULTI_HID_CONFIG_DEFAULTS`). Each run logs its inputs:

```
numAppleHID, numOldAppleHID, numIncompatibleHID, numTwoSniffApple, numSCODevice, numOfLEHID
is15msDefault, is15msSupportedByController, isMove3PPLEMStoLegacyMode, isGamingConsoleMode, isAoSWith15msHIDs
numAudioDevices, isStereoSCO, isStreaming, numGameControllers, numOfLEADevices
numberOfAppleMSOrTP, numberOfAppleKB, numberOfNon15msMitiForMSTP
```

**Inputs:**
| Input | Meaning |
| --- | --- |
| `is15msSupportedByController` | chip generation 20–1999, 3000–3999 or 5000–5999. |
| `is15msDefault` | feature flag `ELLAudioWith15msHIDs` or `Use15msSniffIntervalForModernAppleHIDs`, or preference `moveAllAppleHIDsTo15` in `com.apple.bluetooth`. |
| `isMove3PPLEMStoLegacyMode` | the `move3PPLEMSToLegacyMode` preference. |
| `isGamingConsoleMode` | macOS Game Mode, from the `com.apple.system.console_mode_changed` notification. |
| `numIncompatibleHID` | old Apple HIDs, plus **classic (BR/EDR) HIDs that aren't modern Apple HIDs or game controllers, once they enter sniff mode at an interval other than 15 ms**. The sniff callback logs "classic device … is not using 15ms sniff interval" and sets the flag, cleared on disconnect. Classic game controllers are put on 15 ms sniff and don't count. So a typical third-party classic keyboard or mouse moves every LE HID to 11.25 ms. |

**The choice:**
1. Start with `11_25_MS_ALL` if anything is incompatible, otherwise `DEFAULTS`.
2. **15 ms default on:** anything incompatible → `11_25_MS_ALL`, done; otherwise → `15_MS_ALL`, then step 4.
3. **15 ms default off, no Apple HID connected:** if an LE HID is connected, 15 ms is supported, nothing is incompatible and a stereo SCO call or an LE Audio device is active → `15_MS_ALL`; otherwise keep step 1's mode. If 15 ms isn't supported, done; otherwise step 4.
4. **Game Mode:** Game Mode on, 15 ms supported, nothing incompatible and **exactly one** game controller connected → `DEFAULTS` becomes `DEFAULTS_LL`, `15_MS_ALL` becomes `15_MS_ALL_LL`.

With Apple HIDs connected, further branches (not detailed) can force `11_25_MS_ALL` or `15_MS_ALL` depending on the Apple mouse/trackpad/keyboard mix and 15 ms support. `11_25_MS_ALL_LL` is in the name table but never chosen.

**Recomputed when:** an HID connects or disconnects; an audio service connects or disconnects; Game Mode changes; `move3PPLEMSToLegacyMode` changes; an HID's interval leaves its plan (7.6).

### 7.3 Each device's interval and profile

`switchLEHIDsConnectionInterval` goes through each connected LE HID:

| Mode | Game controller (7.5) | Low-latency pointer (7.4) | Any other LE HID |
| --- | --- | --- | --- |
| `DEFAULTS`, `15_MS_ALL` | 7.5 ms if it also passes 7.4, else 15 ms | **7.5 ms** | **15 ms** |
| `DEFAULTS_LL`, `15_MS_ALL_LL` (Game Mode) | **7.5 ms** | 15 ms | 15 ms |
| `11_25_MS_ALL` | 11.25 ms | 11.25 ms | 11.25 ms |

**Interval to profile:** 15 ms → `LEHID-15ms`; 11.25 ms → `LEHID-11.25ms`; 7.5 ms → `LEHID-7.5msCE2` for Logitech (PnP VID 0x046D) and game controllers, otherwise `LEHID-7.5msCE4`.

The profile values (4) are what reaches the controller. The latency, timeout and CE length in the `switching to X ms latency(N) … CELength(N)` log line are computed but not sent. Observed: `setConnectionLatency LEHID-15ms` produced 15/15 ms, latency 22, CE 4, timeout 2000.

Every LE HID link is also marked high-priority in the controller (`enableLEHIDHighPriority`, logged as "… is setting to low latency").

### 7.4 Who gets 7.5 ms: `isLowLatencyBLEHID`

Checked in order; the first match decides:

1. No `_HID_DEVICE_` tag → no.
2. `move3PPLEMSToLegacyMode` off → no.
3. Any LE Audio device connected → no.
4. An Apple device, while feature flag `UseLowLatencyToAppleMID` is off → no.
5. Appearance **0x3C2 (mouse)** or **0x3C9 (touchpad)** → **yes**.
6. PnP VID 0x046D (Logitech) with PID 0xBB00 or 0xBB01 → yes.
7. Anything else → no, including keyboard 0x3C1, gamepad 0x3C4, appearance 0 and devices without a PnP ID.

### 7.5 What counts as a game controller

The `IsBLEGameController` tag comes only from a built-in PnP VID/PID list:
- Microsoft (VID 0x045E) Xbox PIDs 0x0B13, 0x0B20, 0x0B21, 0x0B22
- any device with VID 0x358A
- FeiZhi APEX (0x1915/0x0040) and Flydigi VADER2P (0x07D7/0x0040), which report a mouse or touchpad appearance and are relabelled gamepad (0x3C4)

A gamepad appearance alone doesn't make a device a game controller.

### 7.6 After the switch

When an HID's interval changes (`macLeDeviceConnectionParametersUpdated`), macOS either keeps the new value or recomputes the mode, which pushes the device back to its plan:

| Device | Mode | Kept | Pushed back |
| --- | --- | --- | --- |
| Game controller | any | its planned interval | anything else |
| Low-latency pointer | `DEFAULTS`, `15_MS_ALL` | 7.5 ms | anything else |
| Low-latency pointer | `11_25_MS_ALL` | below 15 ms | 15 ms or more |
| Other HID | `DEFAULTS`, `11_25_MS_ALL` | any interval that got past section 6 | nothing |
| Other HID | `15_MS_ALL`, `…_LL` | multiples of 15 ms | anything else |

In `DEFAULTS` mode macOS also records the device's interval as its preference ("cache device's preference CI is … ms"), cleared when `move3PPLEMSToLegacyMode` changes.

## 8. Status flags that are not inputs

These look like judgments in the log, but none feeds back into the interval choice.

- **"Compatible LE HID" / "Incompatible LE HID":** a result of 7.3. 15 ms is "Compatible"; 7.5 or 11.25 ms is "Incompatible". Stored on the device record (offset 642, "Setting LeDevice … to Compatible/Incompatible HID"), copied into the `CBDevice` other processes see, with a change notification. On a record without `_HID_DEVICE_` it reads as "compatible" (7.1).
- **`HIDDevicePoorBehavior`:** set when an LE HID's connection update completes below 15 ms, cleared at 15 ms or more; gated by a platform check. Probably surfaces as `HIDBadBehavior` in `CBDevice` log flags (fits a mouse at 7.5 ms; mapping not traced).
- **`HIDDeviceKnownGoodBehavior` / `KnownPoorBehavior` / `UnknownBehavior`:** LE mice only. macOS hashes the device's GATT layout and compares it, by device name, against built-in lists.
  - Poor: CORSAIR HARPOON, Arc Touch BT Mouse.
  - Good: MX Anywhere 2, MX Master 3, Surface Mouse, BluetoothMouse3600, Razer Atheris, Designer Mouse, Keyboard K600 TV.
  - Always good: Modern Mobile Mouse, Surface Arc Mouse.
  - With the `RuleBasedHIDClassification` flag, unmatched mice get Unknown.
- **HID lag detector** ("HID lag issue is detected"): writes a note into the HCI trace and adjusts a statistics threshold. It doesn't change the interval or disconnect anything.
- **"Dynamic Latency Trigger" and "Low Latency Game" lines** (keyboard in use, VoiceOver, Game Mode, GarageBand, spatial audio, screen state): the audio stack's adaptive jitter buffer, not LE intervals.

## 9. What a third-party accessory gets

**This Mac's inputs** (from the code, `/System/Library/FeatureFlags/Domain/BluetoothFeatures.plist`, `/Library/Preferences/com.apple.bluetooth` and the logs):

| Input | Value |
| --- | --- |
| Chip generation | 24 (`BCM_4388C2`): connect range 10–30 ms, 15 ms HID supported |
| `move3PPLEMSToLegacyMode` | 1 |
| `ELLAudioWith15msHIDs`, `Use15msSniffIntervalForModernAppleHIDs`, `UseLowLatencyToAppleMID` | not in the flags file (off) |
| `moveAllAppleHIDsTo15` | not set |
| HID mode with one third-party LE HID | `MULTI_HID_CONFIG_DEFAULTS` (observed) |

**Results:**

| Accessory | Interval | Peripheral latency | Source |
| --- | --- | --- | --- |
| Generic GATT | 30 ms | 0 | observed |
| Generic GATT, app sets level `low` | 30 ms | 0 | observed |
| Generic GATT requesting exactly 15/15 ms, no app level this connection | 15 ms | as requested | code |
| HID, keyboard appearance or none (e.g. Flipper Zero, appearance 0x0) | 15 ms | 22 | observed |
| HID, with a third-party classic HID in non-15 ms sniff | 11.25 ms | 30 | code |
| HID, **mouse or touchpad appearance in the advertisement** | **7.5 ms** | 44 | code |
| HID, recognised game controller, during Game Mode | 7.5 ms | 44 | code |
| Your own USB Bluetooth dongle and stack | your choice | your choice | see 11 |

A 7.5 ms pointer needs a primary HID service (0x1812) and appearance 0x3C2 or 0x3C9 **in the advertising data** (7.1, 7.4). It drops back to 15 or 11.25 ms when an LE Audio device connects, an old Apple HID connects or a third-party classic HID enters non-15 ms sniff, or Game Mode is on with exactly one game controller.

## 10. Reading the log

```bash
log stream --predicate 'process == "bluetoothd" AND (eventMessage CONTAINS "adjustHIDSniffInterval" OR eventMessage CONTAINS "switchLEHIDs" OR eventMessage CONTAINS "isLowLatencyBLEHID" OR eventMessage CONTAINS "following parameters" OR eventMessage CONTAINS "overriding" OR eventMessage CONTAINS "connection latency" OR eventMessage CONTAINS "ParametersUpdated")'
```

| Line | Meaning |
| --- | --- |
| `overriding macOS connection interval to …` | initial range (3) |
| `Setting connection latency to "<profile>" …` | profile applied (4) |
| `combinedLatencyForDevice returning …` | app level (5) |
| `Accepting/Rejecting following parameters: min=… max=…` | accessory request result (6) |
| `adjustHIDSniffInterval: MULTI_HID_CONFIG_…` | HID mode (7.2) |
| `isLowLatencyBLEHID: … appearanceValue(0x…)` / `… VID / PID` / `needs low latency` | 7.5 ms check (7.4) |
| `switchLEHIDsConnectionInterval…: LE HID "…" switching to X ms …` | planned HID interval (7.3) |
| `LE Connection Parameters Updated … interval:N peripheral latency:N` | parameters in use |
| `Remote Features: … SupportsSubrating: N` | whether the peer can subrate (4) |
| `Sent LE Connection Subrating Request with status: N` / `Remote device does not support connection subrating` / `… Subrate Factor: N` | subrating attempt and result (4) |

## 11. Workarounds and other platforms

No generic LE data link gets below ~30 ms with a Mac as central. The usual workarounds:

- **Look like an HID device.** ZMK keyboards get 15 ms ([zmkfirmware/zmk#2661](https://github.com/zmkfirmware/zmk/issues/2661), [Reddit ZMK thread](https://www.reddit.com/r/ErgoMechKeyboards/comments/1j0rxu7/zmk_totem_bluetooth_is_immediately_disconnected/)), with latency 22 on macOS 27, matching `LEHID-15ms`. Logitech mice get 7.5 ms with latency 44 ([Apple forum thread 845857](https://developer.apple.com/forums/thread/845857)).
- **Bypass the Mac's controller.** The built-in chip can't be driven directly ([google/bumble#660](https://github.com/google/bumble/issues/660)), so people use a USB dongle (e.g. nRF52840) with their own stack such as Bumble. Wireless-MIDI products (CME WIDI, Roland WM-1) reach about 3 ms only between two of their own adapters ([CME](https://www.cme-pro.com/the-truth-about-bluetooth-midi/), [Zynthian thread](https://discourse.zynthian.org/t/3-wireless-midi-adapters-offer-3ms-0-38-ms-latency/5699)).
- **Timestamp everything** and live with the interval, as BLE-MIDI does. A [2016 sniffer test](https://gearspace.com/threads/ble-midi-bluetooth-midi-latency-measurement.1129003/) saw macOS give a MIDI device 11.25 ms.

Other platforms let a central ask directly: Android `requestConnectionPriority`, Windows 11 `RequestPreferredConnectionParameters`, Linux debugfs `conn_min_interval` / `conn_max_interval`. macOS and iOS offer nothing public.

## 12. Open questions

- **BLE MIDI.** Own profiles (`midi`, `midi v2`) and MIDI-specific interval settings. Unknown: a MIDI controller's default interval, and what selects `midi v2`.
- **Game controllers beyond the built-in list.** The feature flag `UseLowLatencyGameControllers` (present in this Mac's flags file), and whether the GameController framework can tag more devices `IsBLEGameController`.
- **Classic (BR/EDR) devices.** DualSense, Switch Pro, older Xbox pads, classic keyboards, mice and audio use sniff intervals and audio links. Only their effect on LE HIDs (7.2) is covered.
- **Untested predictions:**
  - an HID with a mouse appearance in its advertisement: 7.5 ms, latency 44
  - an exact 15/15 ms request with no app level set during that connection: 15 ms
  - an HID requesting 15/15 ms with peripheral latency 0: latency 0 until the profile is re-applied (6)
- **Sleep, lid closed and dark wake.** Whether intervals or profiles change.
- **Other Macs.** Mapping chip generation numbers to Mac models, to tell which start at 7.5–15 ms instead of 10–30 ms.
