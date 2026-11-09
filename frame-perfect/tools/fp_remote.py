#!/usr/bin/env python3
"""Drive Frame Perfect's Remote mode from the Mac.

`serve` is a long-running process. It keeps:
  * the Bluetooth link to "FP <flipper name>" (15 ms HID link, peripheral latency off),
  * a running estimate of the Flipper's clock from pings every 200 ms,
  * the Switch 2's frame times from the TV (HDMI IRQ 134 via a private tracefs
    instance, mapped to Mac time with trace_marker round trips), or a fake
    59.973 Hz grid with --fake-grid,
  * a frame grid sent to the Flipper every 0.5 s ('F': period + one frame
    start, both on the Flipper's clock).
Everything is logged as JSON lines in logs/remote-<time>.jsonl.

The other subcommands talk to it over a Unix socket and print JSON:

  tools/ble-app/run.sh 'tools/fp_remote.py serve' &      # Bluetooth needs the app wrapper
  python3 tools/fp_remote.py status
  python3 tools/fp_remote.py calibrate --seconds 60      # Flipper on the Mac's USB
  python3 tools/fp_remote.py usb-on                      # Flipper becomes the controller
  python3 tools/fp_remote.py load oot_jumpslash.txt
  python3 tools/fp_remote.py param wait 3
  python3 tools/fp_remote.py go --offset-us 0 --delay-ms 400
  python3 tools/fp_remote.py stop | usb-off | quit
  python3 tools/fp_remote.py hold L+R 500 [--stick LX,LY,RX,RY] | claim
  python3 tools/fp_remote.py write sequences/x.txt [NAME] | files | delete NAME | log [BYTES]
  python3 tools/fp_remote.py update dist/frame_perfect.fap   # all over Bluetooth
  python3 tools/fp_remote.py quit-app
  python3 tools/fp_remote.py superjump --loops 16 [--claim] [--dry-run]

Clock model: Flipper time = Mac time + offset(t). Bluetooth pings give an
estimate that is off by a steady few ms (the two directions differ); that
bias is measured once over USB (`calibrate`) and holds while the Bluetooth
connection stays up, so keep `serve` and the Flipper app running.
"""
import argparse
import asyncio
import bisect
import json
import os
import random
import re
import select
import socket
import statistics
import struct
import subprocess
import sys
import termios
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fp_config as C  # noqa: E402  local settings (private/config.json)
ROOT = os.path.dirname(HERE)
SOCK = os.path.join(HERE, ".fp_remote.sock")
CALIB = os.path.join(HERE, ".fp_calib.json")  # last USB calibration, reloaded on start
SERIAL_PORT = C.SERIAL_PORT
APP = "/ext/apps/USB/frame_perfect.fap"
SERVICE = "8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000"
TX_CHAR = "19ed82ae-ed21-4c9d-4145-228e61fe0000"  # Flipper -> Mac
RX_CHAR = "19ed82ae-ed21-4c9d-4145-228e62fe0000"  # Mac -> Flipper
NAME = C.REMOTE_BLE_NAME  # None = any "FP ..." device
FAKE_PERIOD_NS = 16_674_400  # measured Switch 2 frame period (59.973 Hz)
TV_HOST = C.TV_HOST
TV_SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=7", "-o", "LogLevel=ERROR"] + (
    ["-o", "IdentitiesOnly=yes", "-i", C.TV_SSH_KEY] if C.TV_SSH_KEY else [])
TRACE_INST = "/sys/kernel/tracing/instances/fp"
BUTTONS = {"Y": 0x1, "X": 0x2, "B": 0x4, "A": 0x8, "R": 0x40, "ZR": 0x80, "MINUS": 0x100, "PLUS": 0x200,
           "RS": 0x400, "LS": 0x800, "HOME": 0x1000, "CAPTURE": 0x2000, "DOWN": 0x10000, "UP": 0x20000,
           "RIGHT": 0x40000, "LEFT": 0x80000, "L": 0x400000, "ZL": 0x800000}  # pad_usb.h BTN_*


def now_us():
    return time.perf_counter_ns() / 1000.0


def linfit(xs, ys):
    """Least-squares line; returns (f, slope)."""
    mx, my = statistics.mean(xs), statistics.mean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    sl = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else 0.0
    return (lambda t: my + sl * (t - mx)), sl


# --------------------------------------------------------------------------
# Logging
# --------------------------------------------------------------------------

class Log:
    def __init__(self):
        os.makedirs(os.path.join(ROOT, "logs"), exist_ok=True)
        self.path = os.path.join(ROOT, "logs", time.strftime("remote-%Y-%m-%d-%H%M%S.jsonl"))
        self.f = open(self.path, "a")

    def __call__(self, kind, **kw):
        rec = {"t": round(time.time(), 6), "mac_us": round(now_us(), 1), "kind": kind, **kw}
        self.f.write(json.dumps(rec) + "\n")
        self.f.flush()
        if kind not in ("ping_summary", "grid"):
            print(json.dumps(rec), flush=True)


# --------------------------------------------------------------------------
# Flipper clock over Bluetooth
# --------------------------------------------------------------------------

class Clock:
    """Flipper - Mac offset from Bluetooth pings, plus the USB-measured bias."""

    def __init__(self):
        self.samples = []  # (t_mid_mac, est_offset, rtt)
        self.bias_us = None  # true - estimate, from calibrate
        self.bias_info = None
        if os.path.exists(CALIB):
            # valid while the Bluetooth link stays up; it wanders ~0.4 ms per half hour
            c = json.load(open(CALIB))
            self.bias_us, self.bias_info = c["bias_us"], {**c["info"], "loaded_from_file": True}

    def add(self, t1, t_rx, t_tx, t4):
        est = ((t_rx - t1) + (t_tx - t4)) / 2
        rtt = (t4 - t1) - (t_tx - t_rx)
        self.samples.append(((t1 + t4) / 2, est, rtt))
        cut = now_us() - 120e6
        while self.samples and self.samples[0][0] < cut:
            self.samples.pop(0)

    def raw_fit(self, window_s=20.0, at=None):
        """Line through the fastest 30% of pings in the window. None if too few."""
        at = at or now_us()
        win = [s for s in self.samples if at - window_s * 1e6 <= s[0] <= at]
        if len(win) < 15:
            return None
        win.sort(key=lambda s: s[2])
        best = win[: max(8, len(win) * 3 // 10)]
        f, sl = linfit([s[0] for s in best], [s[1] for s in best])
        return f, sl

    def offset(self, at=None):
        """Best estimate of Flipper - Mac at Mac time `at` (us), or None."""
        fit = self.raw_fit(at=at)
        if not fit:
            return None
        return fit[0](at or now_us()) + (self.bias_us or 0.0)

    def rate(self):
        fit = self.raw_fit(window_s=60.0)
        return 1.0 + (fit[1] if fit else 0.0)  # d(flipper)/d(mac)


# --------------------------------------------------------------------------
# TV frame source
# --------------------------------------------------------------------------

class Tv:
    """HDMI frame interrupts from the TV, mapped to Mac time.

    Uses its own tracefs instance, so the TV's global trace settings (clock,
    events, block_dump_msg) are untouched. The instance is removed on stop."""

    LINE = re.compile(r"\s(\d+\.\d+): (irq_handler_entry: irq=134|tracing_mark_write: M(\d+))")

    def __init__(self, log):
        self.log = log
        self.frames = []  # TV seconds (float, mono_raw)
        self.marks = {}  # n -> (t_send, t_recv) mac us
        self.sync = []  # (mac_mid_us, tv_us - mac_mid_us, rtt)
        self.stream = None
        self.shell = None
        self.n = 0
        self.lock = threading.Lock()
        self.error = None

    def start(self):
        setup = (
            f"T=/sys/kernel/tracing; I={TRACE_INST}; [ -d $I ] || mkdir $I || exit 3; "
            "echo 0 > $I/tracing_on; echo mono_raw > $I/trace_clock; echo > $I/trace; "
            "echo 'irq==134' > $I/events/irq/irq_handler_entry/filter; "
            "echo 1 > $I/events/irq/irq_handler_entry/enable; echo 1 > $I/tracing_on; "
            "exec cat $I/trace_pipe")
        self.stream = subprocess.Popen(TV_SSH + [TV_HOST, setup], stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, bufsize=1)
        self.shell = subprocess.Popen(TV_SSH + [TV_HOST, "sh"], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, text=True, bufsize=1)
        threading.Thread(target=self._read_stream, daemon=True).start()
        threading.Thread(target=self._sync_loop, daemon=True).start()
        self.log("tv_start", instance=TRACE_INST)

    def stop(self):
        for p in (self.stream, self.shell):
            if p and p.poll() is None:
                p.terminate()
        cleanup = (f"I={TRACE_INST}; [ -d $I ] && {{ echo 0 > $I/tracing_on; "
                   "echo 0 > $I/events/irq/irq_handler_entry/enable; rmdir $I; }}; echo cleaned")
        r = subprocess.run(TV_SSH + [TV_HOST, cleanup], capture_output=True, text=True, timeout=20)
        self.log("tv_stop", result=r.stdout.strip(), err=r.stderr.strip())

    def _read_stream(self):
        for line in self.stream.stdout:
            m = self.LINE.search(line)
            if not m:
                continue
            ts = float(m.group(1))
            with self.lock:
                if m.group(3):
                    n = int(m.group(3))
                    mk = self.marks.pop(n, None)
                    if mk and mk[1]:
                        mid = (mk[0] + mk[1]) / 2
                        self.sync.append((mid, ts * 1e6 - mid, mk[1] - mk[0]))
                        if len(self.sync) > 600:
                            self.sync.pop(0)
                else:
                    self.frames.append(ts)
                    if len(self.frames) > 1200:
                        del self.frames[:200]
        self.error = "trace stream ended: " + (self.stream.stderr.read() or "")[:200]

    def _sync_loop(self):
        while self.shell.poll() is None:
            self.n += 1
            n = self.n
            t1 = now_us()
            with self.lock:
                self.marks[n] = (t1, None)
            try:
                self.shell.stdin.write(f"echo M{n} > {TRACE_INST}/trace_marker; echo D{n}\n")
                self.shell.stdin.flush()
                line = self.shell.stdout.readline()
            except (BrokenPipeError, OSError):
                break
            t4 = now_us()
            if line.strip() == f"D{n}":
                with self.lock:
                    if n in self.marks:
                        self.marks[n] = (t1, t4)
            time.sleep(random.uniform(0.15, 0.3))

    def tv_to_mac(self):
        """(f(tv_us) -> mac_us, slope d(tv)/d(mac), quality info) or None."""
        with self.lock:
            s = list(self.sync[-300:])
        if len(s) < 10:
            return None
        s.sort(key=lambda x: x[2])
        best = s[: max(6, len(s) // 4)]
        f, sl = linfit([x[0] for x in best], [x[1] for x in best])
        # tv = mac + off(mac); invert approximately (slope ~1e-5)
        def tv2mac(tv_us):
            m = tv_us - f(tv_us)
            return tv_us - f(m)
        self.mac2tv = lambda mac_us: mac_us + f(mac_us)
        return tv2mac, 1.0 + sl, {"marks": len(s), "best_rtt_ms": round(best[0][2] / 1000, 2),
                                   "median_rtt_ms": round(statistics.median(x[2] for x in s) / 1000, 2)}

    def grid(self):
        """(last frame tv_us, period tv_ns) from the last ~5 s, or None."""
        with self.lock:
            fr = list(self.frames[-330:])
        if len(fr) < 30:
            return None
        iv = [(b - a) * 1e9 for a, b in zip(fr, fr[1:])]
        period = statistics.median(iv[-300:])
        return fr[-1] * 1e6, period, len(fr)


# --------------------------------------------------------------------------
# USB reference clock (fp_clock on the Flipper's CLI)
# --------------------------------------------------------------------------

def usb_clock_samples(seconds):
    fd = os.open(SERIAL_PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(fd, termios.TCSANOW, a)

    def read_until(tok, timeout):
        out = b""
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    out += os.read(fd, 4096)
                except BlockingIOError:
                    pass
                if tok in out:
                    break
        return out

    os.write(fd, b"\r\n")
    read_until(b">:", 2)
    os.write(fd, b"fp_clock\r\n")
    if b"ready" not in read_until(b"ready", 4):
        os.close(fd)
        raise RuntimeError("fp_clock did not start (is Frame Perfect in remote mode, USB on the Mac?)")
    out = []
    end = time.time() + seconds
    while time.time() < end:
        buf = b""
        t1 = now_us()
        os.write(fd, b"x")
        while b"\n" not in buf:
            r, _, _ = select.select([fd], [], [], 0.5)
            if not r:
                break
            buf += os.read(fd, 256)
        t4 = now_us()
        try:
            _, sec, us = buf.split(b"\n")[0].strip().split()
            out.append((t1, int(sec) * 1_000_000 + int(us), t4))
        except ValueError:
            pass
        time.sleep(random.uniform(0.005, 0.02))
    os.write(fd, b"q")
    read_until(b">:", 2)
    os.close(fd)
    return out


# --------------------------------------------------------------------------
# Server
# --------------------------------------------------------------------------

class Server:
    def __init__(self, args):
        self.args = args
        self.log = Log()
        self.clock = Clock()
        self.tv = None
        self.client = None
        self.rx_c = None
        self.connected = False
        self.replies = asyncio.Queue()
        self.waiters = {}
        self.pings_sent = self.pings_got = 0
        self.last_status = None
        self.grid_sent = None
        self.fake_anchor = None
        self.stop_event = asyncio.Event()
        self.updating = False

    # ---- Bluetooth ----
    async def connect(self):
        from bleak import BleakClient, BleakScanner
        from bleak.backends.device import BLEDevice
        from CoreBluetooth import CBUUID
        target = None
        for attempt in range(6):
            sc = BleakScanner()
            mgr = sc._backend._manager
            await mgr.wait_until_ready()
            for p in mgr.central_manager.retrieveConnectedPeripheralsWithServices_([CBUUID.UUIDWithString_(SERVICE)]):
                if C.is_remote_name(str(p.name())):
                    target = BLEDevice(str(p.identifier().UUIDString()), str(p.name()), (p, mgr))
            if not target:
                devs = await BleakScanner.discover(timeout=5.0, return_adv=True)
                for addr, (dev, adv) in devs.items():
                    if C.is_remote_name(dev.name or adv.local_name):
                        target = dev
            if target:
                break
            self.log("bt_wait", attempt=attempt)
        if not target:
            raise RuntimeError(f"{NAME or 'FP <flipper>'} not found: is Frame Perfect running in remote mode?")
        self.client = BleakClient(target, timeout=20.0, disconnected_callback=self._on_disc)
        await self.client.connect()
        tx_c = None
        for svc in self.client.services:
            for ch in svc.characteristics:
                if ch.uuid == TX_CHAR and tx_c is None:
                    tx_c = ch
                if ch.uuid == RX_CHAR and self.rx_c is None:
                    self.rx_c = ch
        await self.client.start_notify(tx_c, self._on_notify)
        self.connected = True
        self.log("bt_connected", address=target.address, mtu=self.client.mtu_size)

    def _on_disc(self, _c):
        self.connected = False
        self.log("bt_disconnected")

    def _on_notify(self, _h, data):
        t4 = now_us()
        data = bytes(data)
        k = data[:1]
        if k == b"R" and len(data) >= 21:
            seq, t_rx, t_tx = struct.unpack_from("<IQQ", data, 1)
            t1 = self.waiters.pop(seq, None)
            if t1 is not None:
                self.pings_got += 1
                self.clock.add(t1, t_rx, t_tx, t4)
            return
        if k == b"K":
            return
        self.replies.put_nowait((t4, data))
        self.log("rx", msg=self._decode(data))

    @staticmethod
    def _decode(d):
        k = chr(d[0])
        try:
            if k == "!":
                return {"text": d[1:].decode(errors="replace")}
            if k == "l":
                return {"type": "loaded", "ok": d[1], "n_params": d[2], "cycle_us": struct.unpack_from("<I", d, 3)[0]}
            if k == "p":
                v, lo, hi = struct.unpack_from("<hhh", d, 2)
                return {"type": "param", "idx": d[1], "value": v, "min": lo, "max": hi, "name": d[8:].decode(errors="replace")}
            if k == "g":
                gid, st, start = struct.unpack_from("<HBQ", d, 1)
                return {"type": "go", "id": gid, "status": st, "start_us": start}
            if k == "d":
                gid, cyc, edges = struct.unpack_from("<HII", d, 1)
                return {"type": "done", "id": gid, "cycles": cyc, "edges": edges}
            if k == "u":
                return {"type": "usb", "ok": d[1], "kind": "hori" if d[2] else "pro", "interval": d[3]}
            if k == "q":
                pp, lp, cyc = struct.unpack_from("<IQI", d, 3)
                return {"type": "status", "usb": ["off", "waiting", "asleep", "handshake", "streaming"][d[1]],
                        "playing": d[2], "poll_us": pp, "last_poll_us": lp, "cycles": cyc,
                        "grid": d[19], "seq": d[20]}
            if k == "w":
                return {"type": "written", "ok": d[1], "bytes": struct.unpack_from("<I", d, 2)[0]}
            if k == "y":
                if d[1] == 0xFF:
                    return {"type": "files_end", "end": True, "count": d[2]}
                return {"type": "file", "i": d[1], "name": d[2:].decode(errors="replace")}
            if k == "o":
                return {"type": "log_chunk", "bytes": len(d) - 1}
            if k == "e":
                if d[1] == 0xFF:
                    return {"type": "edges", "count": d[2], "start_us": int.from_bytes(d[3:10], "little")}
                t, b = struct.unpack_from("<II", d, 2)
                return {"type": "edge", "i": d[1], "t_us": t, "buttons": b}
        except (struct.error, IndexError):
            pass
        return {"raw": d.hex()}

    async def send(self, data):
        if not self.connected:
            raise RuntimeError("Bluetooth not connected")
        await self.client.write_gatt_char(self.rx_c, data, response=False)

    async def ping_loop(self):
        seq = 0
        last_sum = time.time()
        while not self.stop_event.is_set():
            if not self.connected and not self.updating:
                try:
                    self.rx_c = None
                    await self.connect()
                except Exception as e:  # noqa: BLE001
                    self.log("reconnect_failed", error=str(e))
                    await asyncio.sleep(3)
                    continue
            if self.connected:
                seq += 1
                self.waiters[seq] = now_us()
                try:
                    await self.send(b"S" + struct.pack("<I", seq))
                    self.pings_sent += 1
                except Exception as e:  # noqa: BLE001
                    self.log("ping_error", error=str(e))
                for old in [k for k in self.waiters if k < seq - 50]:
                    self.waiters.pop(old)
            if time.time() - last_sum > 10:
                last_sum = time.time()
                self.log("ping_summary", sent=self.pings_sent, got=self.pings_got,
                         offset_us=self.clock.offset(), rate_ppm=(self.clock.rate() - 1) * 1e6)
            await asyncio.sleep(random.uniform(0.15, 0.25))

    # ---- frame grid ----
    def flipper_time(self, mac_us):
        off = self.clock.offset(mac_us)
        return None if off is None else mac_us + off

    def current_grid(self):
        """(period_ns, anchor_flipper_us, info) on the Flipper's clock, or None."""
        if self.args.fake_grid:
            nowf = self.flipper_time(now_us())
            if nowf is None:
                return None
            if self.fake_anchor is None:
                self.fake_anchor = nowf
            return FAKE_PERIOD_NS, self.fake_anchor, {"source": "fake"}
        if not self.tv:
            return None
        g, m = self.tv.grid(), self.tv.tv_to_mac()
        if not g or not m:
            return None
        last_tv_us, period_tv_ns, nfr = g
        tv2mac, tv_rate, q = m
        mac_anchor = tv2mac(last_tv_us)
        f_anchor = self.flipper_time(mac_anchor)
        if f_anchor is None:
            return None
        # period on the Flipper clock: tv ns -> mac ns -> flipper ns
        period_f = period_tv_ns / tv_rate * self.clock.rate()
        return int(round(period_f)), f_anchor, {"source": "tv", "frames": nfr, "period_tv_ns": round(period_tv_ns),
                                                 "age_ms": round((now_us() - mac_anchor) / 1000, 1), **q}

    async def grid_loop(self):
        while not self.stop_event.is_set():
            g = self.current_grid()
            if g and self.connected:
                period_ns, anchor, info = g
                try:
                    await self.send(b"F" + struct.pack("<IQ", period_ns, int(round(anchor))))
                    self.grid_sent = {"period_ns": period_ns, "anchor_us": int(round(anchor)), **info}
                    self.log("grid", **self.grid_sent)
                except Exception as e:  # noqa: BLE001
                    self.log("grid_error", error=str(e))
            await asyncio.sleep(0.5)

    # ---- request/reply helpers ----
    async def expect(self, kinds, timeout=3.0, count=1):
        out = []
        end = time.time() + timeout
        while len(out) < count and time.time() < end:
            try:
                t, d = await asyncio.wait_for(self.replies.get(), timeout=max(0.01, end - time.time()))
            except asyncio.TimeoutError:
                break
            dec = self._decode(d)
            if chr(d[0]) in kinds:
                out.append(dec)
            elif chr(d[0]) == "!":
                out.append(dec)
        return out

    def drain(self):
        while not self.replies.empty():
            self.replies.get_nowait()

    # ---- commands ----
    async def cmd(self, req):
        c = req.get("cmd")
        self.log("cmd", req=req)
        if c == "status":
            self.drain()
            q = None
            if self.connected:
                await self.send(b"Q")
                r = await self.expect("q")
                q = r[0] if r else None
            tvq = None
            if self.tv:
                m = self.tv.tv_to_mac()
                g = self.tv.grid()
                tvq = {"frames": g[2] if g else 0, "period_tv_ns": round(g[1]) if g else None,
                       "sync": m[2] if m else None, "error": self.tv.error}
            return {"bt": self.connected, "pings": [self.pings_sent, self.pings_got],
                    "offset_us": self.clock.offset(), "bias_us": self.clock.bias_us,
                    "bias_info": self.clock.bias_info, "rate_ppm": round((self.clock.rate() - 1) * 1e6, 2),
                    "grid": self.grid_sent, "flipper": q, "tv": tvq, "log": self.log.path}
        if c == "calibrate":
            secs = float(req.get("seconds", 60))
            samples = await asyncio.get_running_loop().run_in_executor(None, usb_clock_samples, secs)
            # true offset per USB sample; keep the fastest round trips per second
            buckets = {}
            for t1, tf, t4 in samples:
                buckets.setdefault(int(t1 // 1e6), []).append((t4 - t1, (t1 + t4) / 2, tf - (t1 + t4) / 2))
            pts = []
            for b in buckets.values():
                b.sort()
                pts += b[: max(1, len(b) // 5)]
            pts = [p for p in pts if p[0] < 1500]  # a slow round trip may pair a reply with the wrong request
            ftrue, slope = linfit([p[1] for p in pts], [p[2] for p in pts])
            for _ in range(2):  # drop outliers and refit
                r = [abs(p[2] - ftrue(p[1])) for p in pts]
                lim = max(60.0, 4 * statistics.median(r))
                pts = [p for p, e in zip(pts, r) if e <= lim]
                ftrue, slope = linfit([p[1] for p in pts], [p[2] for p in pts])
            res = statistics.pstdev([p[2] - ftrue(p[1]) for p in pts])
            # compare with the Bluetooth estimate (no bias) at the same moments
            diffs = []
            for p in pts[:: max(1, len(pts) // 60)]:
                fit = self.clock.raw_fit(at=p[1])
                if fit:
                    diffs.append(ftrue(p[1]) - fit[0](p[1]))
            if len(diffs) < 5:
                return {"error": "not enough Bluetooth pings during calibration", "usb_samples": len(samples)}
            self.clock.bias_us = statistics.median(diffs)
            self.clock.bias_info = {"at": time.strftime("%H:%M:%S"), "usb_samples": len(samples),
                                    "usb_fit_sd_us": round(res, 1), "usb_drift_ppm": round(slope * 1e6, 2),
                                    "bias_spread_us": round(statistics.pstdev(diffs), 1), "points": len(diffs)}
            self.log("calibrated", bias_us=self.clock.bias_us, **self.clock.bias_info)
            json.dump({"bias_us": self.clock.bias_us, "info": self.clock.bias_info}, open(CALIB, "w"))
            return {"bias_us": round(self.clock.bias_us, 1), **self.clock.bias_info}
        if c == "check":
            # how far the Bluetooth estimate (with bias) is from USB truth right now
            secs = float(req.get("seconds", 10))
            samples = await asyncio.get_running_loop().run_in_executor(None, usb_clock_samples, secs)
            errs = []
            for t1, tf, t4 in sorted(samples, key=lambda s: s[2] - s[0])[: max(10, len(samples) // 5)]:
                if t4 - t1 > 1500:
                    continue
                off = self.clock.offset((t1 + t4) / 2)
                if off is not None:
                    errs.append(tf - (t1 + t4) / 2 - off)
            r = {"samples": len(samples), "error_us_median": round(statistics.median(errs), 1) if errs else None,
                 "error_us_spread": round(statistics.pstdev(errs), 1) if len(errs) > 1 else None}
            self.log("check", **r)
            return r
        if c == "usb_on":
            self.drain()
            kind = {"pro": 0, "hori": 1}.get(req.get("kind"), 0xFF)
            await self.send(b"U" + bytes([kind, int(req.get("interval", 8))]))
            return {"reply": await self.expect("u")}
        if c == "usb_off":
            await self.send(b"C")
            return {"reply": await self.expect("!")}
        if c == "load":
            self.drain()
            await self.send(b"L" + req["name"].encode())
            r = await self.expect("l")
            if r and r[0].get("ok"):
                r += await self.expect("p", count=r[0]["n_params"])
            return {"reply": r}
        if c == "param":
            self.drain()
            await self.send(b"P" + struct.pack("<Bh", int(req["idx"]), int(req["value"])))
            return {"reply": await self.expect("p")}
        if c == "hold":
            # buttons: names from pad_usb.h (A B X Y L R ZL ZR PLUS MINUS HOME UP DOWN LEFT RIGHT ...)
            mask = 0
            for b in req["buttons"].upper().replace("+", " ").split():
                mask |= BUTTONS.get(b, 0) if b == "NONE" else BUTTONS[b]
            self.drain()
            stick = [int(v) for v in req.get("stick", "0,0,0,0").split(",")]
            await self.send(b"H" + struct.pack("<IHbbbb", mask, int(req.get("ms", 200)), *stick))
            return {"mask": hex(mask), "reply": await self.expect("!", count=2, timeout=int(req.get("ms", 200)) / 1000 + 3)}
        if c == "claim":
            # take over controller slot 1. If another controller is active, the
            # first L+R only opens the "Press L+R" screen; the second picks us.
            await self.cmd({"cmd": "hold", "buttons": "L+R", "ms": 500})
            await asyncio.sleep(1.0)
            await self.cmd({"cmd": "hold", "buttons": "L+R", "ms": 500})
            await asyncio.sleep(0.8)
            return await self.cmd({"cmd": "hold", "buttons": "A", "ms": 150})
        if c == "write":
            name = req.get("name") or os.path.basename(req["local"])
            data = open(req["local"], "rb").read()
            self.drain()
            await self.send(b"W\x01" + name.encode())
            for off in range(0, len(data), 90):
                await self.send(b"W\x00" + data[off:off + 90])
            await self.send(b"W\x02")
            r = await self.expect("w", timeout=5)
            ok = bool(r and r[0].get("ok") and r[0].get("bytes") == len(data))
            return {"name": name, "bytes": len(data), "ok": ok, "reply": r}
        if c == "files":
            self.drain()
            await self.send(b"Y")
            out = []
            end = time.time() + 5
            while time.time() < end:
                r = await self.expect("y", timeout=end - time.time())
                if not r:
                    break
                if r[0].get("end"):
                    return {"files": out}
                out.append(r[0].get("name"))
            return {"files": out, "incomplete": True}
        if c == "delete":
            self.drain()
            await self.send(b"D" + req["name"].encode())
            return {"reply": await self.expect("!")}
        if c == "log":
            self.drain()
            await self.send(b"O" + struct.pack("<H", int(req.get("bytes", 600))))
            text = b""
            end = time.time() + 8
            while time.time() < end:
                try:
                    _t, d = await asyncio.wait_for(self.replies.get(), end - time.time())
                except asyncio.TimeoutError:
                    break
                if d[:1] == b"o":
                    if len(d) == 1:
                        break
                    text += d[1:]
            return {"log": text.decode(errors="replace")}
        if c == "quit_app":
            await self.send(b"Z")
            return {"ok": True}
        if c == "update":
            return await self.update(req)
        if c == "stop":
            await self.send(b"X")
            return {"reply": await self.expect("!")}
        if c == "edges":
            return {"reply": await self.get_edges()}
        if c == "go":
            return await self.go(req)
        if c == "quit":
            self.stop_event.set()
            return {"ok": True}
        return {"error": f"unknown command {c}"}

    async def update(self, req):
        """Quit Frame Perfect, upload a new build over the stock Bluetooth RPC,
        check its MD5, start it in remote mode, and reconnect."""
        import ble_rpc
        from bleak import BleakClient
        local = req["local"]
        remote = req.get("remote", APP)
        self.updating = True
        out = {}
        try:
            if self.connected:
                await self.send(b"Z")
                await asyncio.sleep(1.0)
                try:
                    await self.client.disconnect()
                except Exception:  # noqa: BLE001
                    pass
            self.connected = False
            dev = None
            for _ in range(5):
                dev = await ble_rpc.find(C.STOCK_BLE_NAME)
                if dev:
                    break
                await asyncio.sleep(2)
            if not dev:
                return {"error": "the Flipper's stock Bluetooth not found after quitting the app"}
            async with BleakClient(dev, timeout=20.0) as c:
                rpc = ble_rpc.Rpc(c)
                await rpc.start()
                t0 = time.time()
                out["upload_ok"] = await rpc.upload(local, remote)
                out["upload_s"] = round(time.time() - t0, 1)
                if out["upload_ok"]:
                    out["start_ok"] = await rpc.app_start(remote, "remote")
                await asyncio.sleep(0.5)
            self.log("update", **out)
        finally:
            self.updating = False  # the ping loop reconnects to the remote profile
        return out

    async def get_edges(self):
        self.drain()
        await self.send(b"E")
        head = await self.expect("e")
        if not head or head[0].get("type") != "edges":
            return head
        return head + await self.expect("e", count=head[0]["count"], timeout=5)

    async def go(self, req):
        self.drain()
        off = self.clock.offset()
        if off is None:
            return {"error": "no clock estimate yet"}
        if self.clock.bias_us is None and not req.get("uncalibrated"):
            return {"error": "not calibrated: run `calibrate` with the Flipper on the Mac's USB first"}
        gid = random.randint(1, 65000)
        after_f = now_us() + off + float(req.get("delay_ms", 400)) * 1000
        safe = int(req.get("safe_us", 6000))  # frame mode by default; 0 = plain time offset
        await self.send(b"G" + struct.pack("<HQiH", gid, int(after_f), int(req.get("offset_us", 0)), safe))
        g = await self.expect("g")
        out = {"go": g, "grid": self.grid_sent}
        if not g or g[0].get("status") != 0:
            return out
        start_f = g[0]["start_us"]
        out["start_mac_us"] = start_f - self.clock.offset()
        if self.tv and self.tv.tv_to_mac():
            out["start_tv_ns"] = int(self.tv.mac2tv(out["start_mac_us"]) * 1000)  # mono_raw, like tv-capture burst
        wait_s = float(req.get("timeout", 30))
        done = await self.expect("d", timeout=wait_s)
        out["done"] = done
        out["edges"] = await self.get_edges()
        self.log("go_result", **out)
        return out

    # ---- socket ----
    async def handle(self, reader, writer):
        try:
            line = await reader.readline()
            req = json.loads(line)
            try:
                res = await self.cmd(req)
            except Exception as e:  # noqa: BLE001
                res = {"error": f"{type(e).__name__}: {e}"}
            writer.write((json.dumps(res) + "\n").encode())
            await writer.drain()
        finally:
            writer.close()

    async def run(self):
        if os.path.exists(SOCK):
            os.unlink(SOCK)
        srv = await asyncio.start_unix_server(self.handle, path=SOCK)
        self.log("serve_start", args=vars(self.args), log=self.log.path)
        if not self.args.no_tv and not self.args.fake_grid:
            if not TV_HOST:
                raise SystemExit("No TV configured (tv_host in private/config.json): use --fake-grid or --no-tv")
            self.tv = Tv(self.log)
            self.tv.start()
        await self.connect()
        tasks = [asyncio.create_task(self.ping_loop()), asyncio.create_task(self.grid_loop())]
        await self.stop_event.wait()
        for t in tasks:
            t.cancel()
        srv.close()
        if self.tv:
            self.tv.stop()
        try:
            await self.client.disconnect()
        except Exception:  # noqa: BLE001
            pass
        os.unlink(SOCK)
        self.log("serve_stop")


def client(req, timeout=120):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    return json.loads(buf)


def superjump(a):
    """The working recipe (sequences/oot_superjump.txt, confirmed 2026-11-09) with N loops.
    Needs: Link on the ground, Deku Stick on ZR and drawn, Fairy Slingshot right
    of it in the R wheel, no X jump since loading from the title screen."""
    import re as _re
    src = open(a.template).read()
    if not _re.search(r"^loop \d+$", src, _re.M):
        print("template has no 'loop N' line")
        return 1
    text = _re.sub(r"^loop \d+$", f"loop {a.loops}", src, flags=_re.M)
    name = f"superjump_x{a.loops}.txt"
    local = os.path.join(ROOT, "sequences", name)
    open(local, "w").write(text)
    w = client({"cmd": "write", "local": local, "name": name})
    if not w.get("ok"):
        print("write failed:", w)
        return 1
    ld = client({"cmd": "load", "name": name})
    rep = ld.get("reply") or []
    if not rep or not rep[0].get("ok"):
        print("load failed:", ld)
        return 1
    secs = 0.45 + 1.1 * a.loops
    print(f"loaded {name}: {a.loops} loops, about {secs:.0f} s")
    if a.dry_run:
        return 0
    if a.claim:
        print("claiming controller slot 1 ...", client({"cmd": "claim"}).get("reply"))
        time.sleep(1.0)
    r = client({"cmd": "go", "delay_ms": 300, "timeout": secs + 15}, timeout=secs + 60)
    g = (r.get("go") or [{}])[0]
    d = (r.get("done") or [{}])[0]
    print(f"go status {g.get('status')} (0 = ok), cycles {d.get('cycles')}, input changes {d.get('edges')}")
    ok = g.get("status") == 0 and d.get("cycles") == a.loops
    if a.capture and ok:
        # The sequence's loop runs to its end, so the Capture press is a separate hold
        # once Link has landed. Only the last ~30 s are kept: ~24 loops fit with the fall.
        if a.loops > 24:
            print(f"note: {a.loops} loops take ~{secs:.0f} s; the clip keeps only the last ~30 s")
        time.sleep(a.capture_wait)
        c = client({"cmd": "hold", "buttons": "CAPTURE", "ms": a.capture_hold_ms})
        print(f"held Capture {a.capture_hold_ms} ms after a {a.capture_wait:.0f} s wait:", c.get("reply"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sp = sub.add_parser("serve")
    sp.add_argument("--fake-grid", action="store_true", help="59.973 Hz grid from the Flipper clock, no TV")
    sp.add_argument("--no-tv", action="store_true")
    sub.add_parser("status")
    p = sub.add_parser("calibrate")
    p.add_argument("--seconds", type=float, default=60)
    p = sub.add_parser("check")
    p.add_argument("--seconds", type=float, default=10)
    p = sub.add_parser("usb-on")
    p.add_argument("--kind", choices=["pro", "hori"])
    p.add_argument("--interval", type=int, default=8)
    sub.add_parser("usb-off")
    p = sub.add_parser("load")
    p.add_argument("name")
    p = sub.add_parser("param")
    p.add_argument("idx", type=int)
    p.add_argument("value", type=int)
    p = sub.add_parser("go")
    p.add_argument("--offset-us", type=int, default=0)
    p.add_argument("--delay-ms", type=float, default=400)
    p.add_argument("--timeout", type=float, default=30)
    p.add_argument("--uncalibrated", action="store_true")
    p.add_argument("--safe-us", type=int, default=6000, help="frame mode: changes reach the console this far into their frame (0 = off)")
    p = sub.add_parser("hold")
    p.add_argument("buttons", help="e.g. 'L+R' or 'A'; 'NONE' for sticks only")
    p.add_argument("ms", type=int, nargs="?", default=200, help="0 = until the next hold")
    p.add_argument("--stick", help="LX,LY,RX,RY each -1/0/1, e.g. 0,1,0,0 = left stick up")
    sub.add_parser("claim")
    p = sub.add_parser("write")
    p.add_argument("local")
    p.add_argument("name", nargs="?")
    sub.add_parser("files")
    p = sub.add_parser("delete")
    p.add_argument("name")
    p = sub.add_parser("log")
    p.add_argument("bytes", type=int, nargs="?", default=600)
    p = sub.add_parser("update")
    p.add_argument("local")
    sub.add_parser("quit-app")
    p = sub.add_parser("superjump", help="write, load and run the working super jump with N loops")
    p.add_argument("--loops", type=int, default=8)
    p.add_argument("--claim", action="store_true", help="take controller slot 1 first (L+R twice, A)")
    p.add_argument("--dry-run", action="store_true", help="write and load only")
    p.add_argument("--capture", action="store_true",
                   help="after the jump, wait --capture-wait s for Link to land, then hold Capture (Switch saves the last ~30 s as a video)")
    p.add_argument("--capture-wait", type=float, default=5.0)
    p.add_argument("--capture-hold-ms", type=int, default=1500)
    p.add_argument("--template", default=os.path.join(ROOT, "sequences", "oot_superjump.txt"))
    sub.add_parser("stop")
    sub.add_parser("edges")
    sub.add_parser("quit")
    a = ap.parse_args()
    if a.cmd == "serve":
        asyncio.run(Server(a).run())
        return 0
    if a.cmd == "superjump":
        return superjump(a)
    req = {k: v for k, v in vars(a).items() if v is not None}
    req["cmd"] = a.cmd.replace("-", "_")
    if "local" in req:
        req["local"] = os.path.abspath(req["local"])  # the server runs from another folder
    res = client(req, timeout=300 if a.cmd == "update" else 120)
    if a.cmd == "log" and "log" in res:
        print(res["log"])
    else:
        print(json.dumps(res, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
