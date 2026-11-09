#!/usr/bin/env python3
"""Stand-in for the Switch: talk to Frame Perfect from a computer.

Waits for the Flipper to appear as a Pro Controller (057E:2009) or HORIPAD
(0F0D:00C1), runs the Pro handshake like a console would, then reads input
reports and prints every button change with its time, plus report intervals.

    uv run --with hidapi python tools/host_test.py --seconds 15

The computer's own USB scheduling adds jitter (about 1 ms on macOS), so this
checks durations to within a few ms; the Flipper's log has the exact times.
"""
import argparse
import statistics
import sys
import time

import hid

PRO = (0x057E, 0x2009)
HORI = (0x0F0D, 0x00C1)


def find(timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for d in hid.enumerate():
            vp = (d["vendor_id"], d["product_id"])
            if vp in (PRO, HORI):
                return vp, d["path"]
        time.sleep(0.2)
    return None, None


def pro_buttons(r):
    # report id 0x30: bytes 3..5 buttons, 6..11 sticks
    return (r[3] | (r[4] << 8) | (r[5] << 16)) & ~0x8000, tuple(r[6:12])


def hori_buttons(r):
    return r[0] | (r[1] << 8) | (r[2] << 16), tuple(r[3:7])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=15)
    ap.add_argument("--wait", type=float, default=20)
    args = ap.parse_args()

    vp, path = find(args.wait)
    if not vp:
        print("no controller found")
        return 1
    kind = "pro" if vp == PRO else "hori"
    print(f"found {kind} {vp[0]:04x}:{vp[1]:04x}")
    dev = hid.device()
    dev.open_path(path)
    dev.set_nonblocking(False)

    if kind == "pro":
        # what a console sends: status, handshake, (baud), handshake, HID-only
        for cmd in ([0x80, 0x01], [0x80, 0x02], [0x80, 0x03], [0x80, 0x02], [0x80, 0x04]):
            dev.write(bytes(cmd) + bytes(62))
            time.sleep(0.05)
        # a few subcommands: device info, SPI read of stick calibration, report mode 0x30
        def sub(sc, data=b""):
            pkt = bytes([0x01, 0x00]) + bytes(8) + bytes([sc]) + data
            dev.write(pkt + bytes(64 - len(pkt)))
            time.sleep(0.03)
        sub(0x02)
        sub(0x10, bytes([0x3D, 0x60, 0, 0, 0x12]))
        sub(0x03, bytes([0x30]))

    t0 = time.monotonic()
    last_t = None
    intervals = []
    last_state = None
    edges = []
    counts = {}
    while time.monotonic() - t0 < args.seconds:
        r = dev.read(64, 200)
        if not r:
            continue
        now = time.monotonic()
        rid = r[0] if kind == "pro" else None
        counts[rid] = counts.get(rid, 0) + 1
        if kind == "pro" and rid != 0x30:
            continue
        if last_t is not None:
            intervals.append((now - last_t) * 1e6)
        last_t = now
        st = pro_buttons(r) if kind == "pro" else hori_buttons(r)
        if st != last_state:
            edges.append((now, st))
            last_state = st
    dev.close()

    print("report ids:", {(hex(k) if k is not None else "hori"): v for k, v in counts.items()})
    if intervals:
        print(
            f"input reports: {len(intervals) + 1}, interval mean {statistics.mean(intervals):.0f} us, "
            f"median {statistics.median(intervals):.0f}, min {min(intervals):.0f}, max {max(intervals):.0f}"
        )
    if edges:
        base = edges[0][0]
        print("button changes (ms from first, frames at 60 fps, held for):")
        for i, (t, st) in enumerate(edges):
            dt = (t - base) * 1000
            held = (edges[i + 1][0] - t) * 1000 if i + 1 < len(edges) else float("nan")
            print(f"  {dt:9.2f} ms  {dt / 16.6667:6.2f} f  buttons=0x{st[0]:06x}  held {held:7.2f} ms = {held / 16.6667:5.2f} f")
    return 0


if __name__ == "__main__":
    sys.exit(main())
