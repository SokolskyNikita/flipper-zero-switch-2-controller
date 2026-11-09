#!/usr/bin/env python3
"""Flipper RPC over Bluetooth: upload a file, check its MD5, start an app.

Works with the Flipper's stock Bluetooth profile (no app running that replaces
it). Uses the same protobuf RPC as qFlipper, framed as varint-length-prefixed
PB.Main messages on the serial service, and waits on the service's
flow-control characteristic so the Flipper's receive buffer never overflows.

    tools/ble-app/run.sh 'tools/ble_rpc.py upload dist/frame_perfect.fap /ext/apps/USB/frame_perfect.fap'
    tools/ble-app/run.sh 'tools/ble_rpc.py start /ext/apps/USB/frame_perfect.fap remote'
    tools/ble-app/run.sh 'tools/ble_rpc.py upload LOCAL REMOTE --start "/ext/apps/USB/frame_perfect.fap remote"'
"""
import argparse
import asyncio
import hashlib
import sys
import time

from bleak import BleakClient, BleakScanner

TX_CHAR = "19ed82ae-ed21-4c9d-4145-228e61fe0000"  # Flipper -> Mac (notify)
RX_CHAR = "19ed82ae-ed21-4c9d-4145-228e62fe0000"  # Mac -> Flipper (write)
FLOW_CHAR = "19ed82ae-ed21-4c9d-4145-228e63fe0000"  # free RX buffer, u32 big-endian
CHUNK = 350


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def f_int(field, v):
    return varint(field << 3) + varint(v)


def f_bytes(field, b):
    if isinstance(b, str):
        b = b.encode()
    return varint((field << 3) | 2) + varint(len(b)) + b


def main_msg(cmd_id, field, content, has_next=False):
    m = f_int(1, cmd_id) + (f_int(3, 1) if has_next else b"") + f_bytes(field, content)
    return varint(len(m)) + m


def read_varint(buf, i):
    val = shift = 0
    while True:
        b = buf[i]
        i += 1
        val |= (b & 0x7F) << shift
        if not b & 0x80:
            return val, i
        shift += 7


def fields(msg):
    """{field: value} for one protobuf message (last wins, enough here)."""
    out, i = {}, 0
    while i < len(msg):
        key, i = read_varint(msg, i)
        f, wt = key >> 3, key & 7
        if wt == 0:
            v, i = read_varint(msg, i)
        elif wt == 2:
            ln, i = read_varint(msg, i)
            v = bytes(msg[i:i + ln])
            i += ln
        else:
            break
        out[f] = v
    return out


class Rpc:
    def __init__(self, client):
        self.c = client
        self.buf = bytearray()
        self.replies = asyncio.Queue()
        self.free = 0
        self.free_evt = asyncio.Event()
        self.cmd = 0

    def on_tx(self, _h, data):
        self.buf += data
        while self.buf:
            try:
                ln, i = read_varint(self.buf, 0)
            except IndexError:
                return
            if len(self.buf) < i + ln:
                return
            self.replies.put_nowait(fields(self.buf[i:i + ln]))
            del self.buf[: i + ln]

    def on_flow(self, _h, data):
        self.free = int.from_bytes(bytes(data[:4]), "big")
        self.free_evt.set()

    async def start(self):
        await self.c.start_notify(TX_CHAR, self.on_tx)
        await self.c.start_notify(FLOW_CHAR, self.on_flow)
        self.free = int.from_bytes(bytes(await self.c.read_gatt_char(FLOW_CHAR))[:4], "big")

    async def send(self, frame):
        while self.free < len(frame):
            self.free_evt.clear()
            try:
                await asyncio.wait_for(self.free_evt.wait(), 2.0)
            except asyncio.TimeoutError:
                self.free = int.from_bytes(bytes(await self.c.read_gatt_char(FLOW_CHAR))[:4], "big")
        self.free -= len(frame)
        await self.c.write_gatt_char(RX_CHAR, frame, response=False)

    async def reply(self, cmd_id, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            r = await asyncio.wait_for(self.replies.get(), max(0.1, end - time.time()))
            if r.get(1, 0) == cmd_id:
                return r
        raise TimeoutError(f"no reply to command {cmd_id}")

    async def upload(self, local, remote):
        data = open(local, "rb").read()
        self.cmd += 1
        cid = self.cmd
        t0 = time.time()
        last_print = t0
        for off in range(0, max(1, len(data)), CHUNK):
            chunk = data[off:off + CHUNK]
            more = off + CHUNK < len(data)
            req = f_bytes(1, remote) + f_bytes(2, f_bytes(4, chunk))
            await self.send(main_msg(cid, 11, req, has_next=more))
            if time.time() - last_print > 2 or not more:
                done = off + len(chunk)
                el = time.time() - t0
                print(f"  {done}/{len(data)} bytes, {done / 1024 / max(el, 1e-3):.1f} KB/s", flush=True)
                last_print = time.time()
        r = await self.reply(cid, 20)
        el = time.time() - t0
        status = r.get(2, 0)
        print(f"upload {local} -> {remote}: status {status}, {len(data)} bytes in {el:.1f} s "
              f"({len(data) / 1024 / el:.1f} KB/s)", flush=True)
        if status:
            return False
        # MD5 check: Md5sumRequest (14) {path} -> Md5sumResponse (15) {md5sum}
        self.cmd += 1
        await self.send(main_msg(self.cmd, 14, f_bytes(1, remote)))
        r = await self.reply(self.cmd)
        md5 = fields(r.get(15, b"")).get(1, b"").decode()
        want = hashlib.md5(data).hexdigest()
        print(f"md5 on Flipper {md5}, local {want}: {'OK' if md5 == want else 'MISMATCH'}", flush=True)
        return md5 == want

    async def app_start(self, name, args):
        self.cmd += 1
        await self.send(main_msg(self.cmd, 16, f_bytes(1, name) + f_bytes(2, args)))
        r = await self.reply(self.cmd)
        print(f"start {name} {args!r}: status {r.get(2, 0)}", flush=True)
        return r.get(2, 0) == 0


async def find(name=None):
    """The Flipper's stock Bluetooth profile: by name, or (name None) by the
    16-bit service 0x308x that Flippers advertise."""
    for _ in range(3):
        devs = await BleakScanner.discover(timeout=6.0, return_adv=True)
        for addr, (dev, adv) in devs.items():
            n = dev.name or adv.local_name or ""
            if name and n == name:
                return dev
            if not name and any(u.lower().startswith("0000308") and u.lower().endswith("-0000-1000-8000-00805f9b34fb")
                                for u in adv.service_uuids):
                return dev
    return None


async def run(a):
    dev = await find(a.name)
    if not dev:
        print(f"{a.name or 'Flipper'!r} not found: is the Flipper's own Bluetooth on (no app replacing it)?")
        return 1
    async with BleakClient(dev, timeout=20.0) as client:
        print(f"connected to {dev.name!r}, mtu {client.mtu_size}", flush=True)
        rpc = Rpc(client)
        await rpc.start()
        print(f"flow control: {rpc.free} bytes free", flush=True)
        ok = True
        if a.cmd == "upload":
            ok = await rpc.upload(a.local, a.remote)
            if ok and a.start:
                name, _, args = a.start.partition(" ")
                ok = await rpc.app_start(name, args)
        elif a.cmd == "start":
            ok = await rpc.app_start(a.name_app, " ".join(a.args))
        await asyncio.sleep(0.5)
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    import fp_config as C
    ap.add_argument("--name", default=C.STOCK_BLE_NAME, help="the Flipper's name (default: config, else any Flipper)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    u = sub.add_parser("upload")
    u.add_argument("local")
    u.add_argument("remote")
    u.add_argument("--start", help="then start this app: 'PATH ARGS'")
    s = sub.add_parser("start")
    s.add_argument("name_app")
    s.add_argument("args", nargs="*")
    sys.exit(asyncio.run(run(ap.parse_args())))
