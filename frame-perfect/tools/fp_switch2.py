#!/usr/bin/env python3
"""fp: the Frame Perfect (Switch 2 controller) session, down to the OoT super jump.

Narrow on purpose: generic Flipper things (USB/CLI, starting apps, the
Bluetooth wrapper, keep-awake) live in flipper.py. This wraps fp_remote.py
(still the source of truth) so a session is a few short commands:

  fp up                 start the app in remote mode if needed, start `serve`, wait for Bluetooth,
                        keep the Mac awake while serve runs
  fp sync [SECONDS]     USB clock calibration (Flipper on the Mac's USB, default 45 s);
                        saves it, appends to the history and prints the drift since the last one
  fp prep               up + sync: everything to do while the Flipper is on the Mac
  fp ready [--no-check] after moving the Flipper to the dock: usb-on, claim, timing preflight
  fp check              timing preflight only (opens the item menu 4 times)
  fp jump [LOOPS] [--capture] [--no-claim] [--no-setup] [--hold-a | --forward]
                        the super jump (default 16 loops; 24 with --capture); first runs `fp setup`, which takes controller 1;
                        --hold-a: A also held through the triple-ZR burst (failed 2026-11-09);
                        --forward: left stick full forward through the burst (failed 2026-11-09)
  fp setup [--no-claim] take controller 1 (L+R x2, A x2), then sequences/oot_setup.txt: B x3 (cancel), R wheel + left (Deku Stick on ZR), ZR, ZL; ~9 s
  fp turn left|right|back [MS]            turn Link, then ZL to recentre the camera
  fp draw               swing the Deku Stick out (ZR 120 ms)
  fp claim              take controller 1 (L+R twice, A twice, slower than fp_remote's claim)
  fp st                 one-screen status
  fp drift              calibration history + the TV/Switch frame-grid fit of the running serve
  fp log                tail the running serve's log
  fp down [--app]       stop serve (and with --app, quit the Flipper app)

Anything else is passed to fp_remote.py, e.g. `fp hold A 150`, `fp go --safe-us 7000`.
"""
import datetime as dt
import json
import os
import statistics as st
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, HERE)
import flipper  # noqa: E402  generic Flipper helpers
import fp_config as C  # noqa: E402
import fp_remote as F  # noqa: E402

ROOT = os.path.dirname(HERE)
PRIVATE = os.path.join(C.REPO, "private")
HISTORY = (os.path.join(PRIVATE, "calib-history.jsonl") if os.path.isdir(PRIVATE)
           else os.path.join(HERE, ".fp_calib_history.jsonl"))
FRAME_US = 1e6 / 60


say, die, on_mac_usb = flipper.say, flipper.die, flipper.on_usb


def remote(*args, check=True):
    """Run fp_remote.py with these arguments in the foreground."""
    r = subprocess.run([sys.executable, os.path.join(HERE, "fp_remote.py"), *args], cwd=ROOT)
    if check and r.returncode:
        sys.exit(r.returncode)
    return r.returncode


def ask(req, timeout=120):
    """One request to serve; None if serve isn't answering."""
    try:
        return F.client(req, timeout=timeout)
    except (FileNotFoundError, ConnectionRefusedError):
        return None


def serve_pids():
    r = subprocess.run(["pgrep", "-f", "fp_remote.py serve"], capture_output=True, text=True)
    return [int(p) for p in r.stdout.split()]


def start_app():
    say("starting Frame Perfect in remote mode ...")
    ok, how, out = flipper.start(F.APP, "remote")
    if not ok:
        die(f"could not start the app over {how}:\n{out}")
    say(f"  started over {how}")
    time.sleep(2)


def launch_serve():
    flipper.ble("tools/fp_remote.py serve", background=True)


def wait_bt(seconds):
    """Wait for serve to answer with bt true. Returns status, 'exited' or None (timeout)."""
    end = time.time() + seconds
    seen_pid = False
    while time.time() < end:
        s = ask({"cmd": "status"}, timeout=10)
        if s and s.get("bt"):
            return s
        if serve_pids():
            seen_pid = True
        elif seen_pid or time.time() > end - seconds + 8:
            return "exited"
        time.sleep(1)
    return None


def keep_awake():
    pids = serve_pids()
    if pids:
        flipper.keep_awake(pids[0])


def up():
    s = ask({"cmd": "status"}, timeout=10)
    if s and s.get("bt"):
        keep_awake()
        say("serve already up, Bluetooth connected")
        return s
    if s:  # serve running, link down: give its 3 s reconnect a chance, then restart the app
        say("serve is up but Bluetooth is down; waiting for its reconnect ...")
        s = wait_bt(10)
        if isinstance(s, dict):
            keep_awake()
            return s
        start_app()
        s = wait_bt(20)
        if not isinstance(s, dict):
            die("Bluetooth still down; is the Flipper on and near the Mac?")
        keep_awake()
        return s
    if on_mac_usb():  # on USB we can see whether the app runs, and skip a serve that can't connect
        try:
            if "Frame Perfect" not in flipper.cli("loader info"):
                start_app()
        except RuntimeError:
            pass
    for attempt in (1, 2):
        say("starting serve ...")
        launch_serve()
        s = wait_bt(25)
        if isinstance(s, dict):
            keep_awake()
            say("serve up, Bluetooth connected")
            return s
        log = flipper.last_ble_output()
        for p in serve_pids():  # timed out: don't leave a half-up server behind
            os.kill(p, 15)
        if attempt == 1 and ("not found" in log or s is None):
            start_app()
            continue
        die("serve did not come up; last lines of its output:\n" + "\n".join(log.strip().splitlines()[-8:]))


def history():
    if not os.path.exists(HISTORY):
        return []
    return [json.loads(l) for l in open(HISTORY) if l.strip()]


def drift_line(prev, cur):
    t0 = dt.datetime.fromisoformat(prev["when"])
    t1 = dt.datetime.fromisoformat(cur["when"])
    hours = (t1 - t0).total_seconds() / 3600
    d = (cur["bias_us"] - prev["bias_us"]) / 1000
    rate = d / (hours * 2) if hours > 0 else 0
    return (f"bias {prev['bias_us'] / 1000:+.2f} -> {cur['bias_us'] / 1000:+.2f} ms ({d:+.2f} ms over {hours:.1f} h, "
            f"{rate:+.2f} ms per half hour); crystal {prev.get('usb_drift_ppm', 0):.2f} -> "
            f"{cur.get('usb_drift_ppm', 0):.2f} ppm; spread {cur.get('bias_spread_us', 0) / 1000:.2f} ms"
            + ("; NEW BLUETOOTH LINK in between, so not pure drift" if prev.get("link") != cur.get("link") else ""))


def sync(seconds=45):
    if not on_mac_usb():
        die(f"{C.SERIAL_PORT} not found: put the Flipper on the Mac's USB first")
    up()
    say(f"calibrating over USB for {seconds:.0f} s ...")
    r = ask({"cmd": "calibrate", "seconds": seconds}, timeout=seconds + 120)
    if not r or "bias_us" not in r:
        die(f"calibration failed: {r}")
    st_ = ask({"cmd": "status"}, timeout=15) or {}
    # the serve log names the Bluetooth link: a new link can shift the bias by itself
    cur = {"when": dt.datetime.now().isoformat(timespec="seconds"), **r, "link": os.path.basename(st_.get("log", ""))}
    hist = history()
    with open(HISTORY, "a") as f:
        f.write(json.dumps(cur) + "\n")
    say(f"calibrated: bias {r['bias_us'] / 1000:+.2f} ms, USB fit sd {r.get('usb_fit_sd_us')} us")
    if hist:
        say("drift since last: " + drift_line(hist[-1], cur))
    if r.get("bias_spread_us", 0) > 600:
        say("note: wide bias spread; run `fp check` before trusting the timing")


def grid_fit(path, skip=0.2):
    """Fit the TV frame grid logged by serve: period on the Mac clock and phase residual."""
    g = []
    for line in open(path):
        if '"kind": "grid"' in line:
            x = json.loads(line)
            if x.get("period_ns") and "anchor_us" in x:
                g.append(x)
    g = g[int(len(g) * skip):]
    doubled = sum(1 for x in g if x["period_ns"] > 1.5 * FRAME_US * 1000)
    g = [x for x in g if x["period_ns"] <= 1.5 * FRAME_US * 1000]
    if len(g) < 10:
        return {"doubled": doubled, "minutes": 0} if doubled else None
    period = st.median(x["period_ns"] for x in g) / 1000
    base = g[0]["anchor_us"]
    xs = [x["anchor_us"] - base for x in g]
    n = [round(v / period) for v in xs]
    mn, mx = st.mean(n), st.mean(xs)
    den = sum((a - mn) ** 2 for a in n)
    pf = sum((a - mn) * (b - mx) for a, b in zip(n, xs)) / den if den else period
    res = [b - (mx + (a - mn) * pf) for a, b in zip(n, xs)]
    return {"doubled": doubled, "minutes": (g[-1]["mac_us"] - g[0]["mac_us"]) / 6e7, "period_ns": pf * 1000,
            "ppm": (pf / FRAME_US - 1) * 1e6, "sd_us": st.pstdev(res), "max_us": max(map(abs, res)),
            "slips": sum(1 for v in res if abs(v) > FRAME_US / 2)}


def tv_readers():
    """How many processes read serve's trace_pipe on the TV (None if the TV can't be reached)."""
    if not F.TV_HOST:
        return None
    pipe = f"cat {F.TRACE_INST}/trace_pipe "
    sh = (f'n=0; for p in /proc/[0-9]*; do [ "$(tr \'\\0\' \' \' < $p/cmdline 2>/dev/null)" = "{pipe}" ] '
          '&& n=$((n+1)); done; echo $n')
    try:
        r = subprocess.run(F.TV_SSH + [F.TV_HOST, sh], capture_output=True, text=True, timeout=15)
        return int(r.stdout.strip())
    except (subprocess.SubprocessError, ValueError):
        return None


def drift():
    hist = history()
    if hist:
        say("USB calibrations:")
        for prev, cur in zip([None] + hist, hist):
            say(f"  {cur['when']}  " + (drift_line(prev, cur) if prev else f"bias {cur['bias_us'] / 1000:+.2f} ms"))
    else:
        say("no calibration history yet (`fp sync` starts it)")
    s = ask({"cmd": "status"}, timeout=10)
    if not s:
        say("serve not running: no live frame grid")
        return
    n = tv_readers()
    if n is not None and n != 1:
        say(f"WARNING: {n} readers on the TV's trace_pipe (want 1): extra ones steal every other frame; `fp down` then `fp up`")
    fit = grid_fit(s["log"])
    if not fit:
        say("frame grid: not enough samples yet")
        return
    if fit["doubled"]:
        say(f"WARNING: {fit['doubled']} grid updates had a doubled (~33 ms) period: frames were being lost "
            "(usually a second trace reader on the TV)")
    if not fit["minutes"]:
        return
    verdict = "steady" if fit["slips"] == 0 and fit["sd_us"] < 1000 and not fit["doubled"] else "JUMPY: run `fp check`"
    say(f"TV/Switch frames over {fit['minutes']:.0f} min: period {fit['period_ns'] / 1e6:.6f} ms "
        f"({fit['ppm']:+.1f} ppm vs 60 Hz on the Mac clock), phase residual sd {fit['sd_us'] / 1000:.2f} ms, "
        f"max {fit['max_us'] / 1000:.2f} ms, {fit['slips']} frame slips -> {verdict}")


def status():
    s = ask({"cmd": "status"}, timeout=15)
    usb = "on the Mac's USB" if on_mac_usb() else "not on the Mac's USB (dock?)"
    if not s:
        say(f"serve: not running   Flipper: {usb}")
        return
    q = s.get("flipper") or {}
    tv = s.get("tv") or {}
    sync_ = tv.get("sync") or {}
    bi = s.get("bias_info") or {}
    hist = history()
    when = hist[-1]["when"] if hist else bi.get("at", "?")
    say(f"bluetooth: {'up' if s.get('bt') else 'DOWN'}   pings {s['pings'][1]}/{s['pings'][0]}   Flipper: {usb}")
    bias = s.get("bias_us")
    say(f"clock bias: {bias / 1000:+.2f} ms (calibrated {when})" if bias is not None else "clock bias: NOT CALIBRATED (`fp sync`)")
    say(f"controller: usb {q.get('usb', '?')}, playing {q.get('playing', '?')}, poll {q.get('poll_us', '?')} us")
    say(f"tv grid: {tv.get('frames', 0)} frames, rtt best {sync_.get('best_rtt_ms', '?')} / median "
        f"{sync_.get('median_rtt_ms', '?')} ms{', error ' + str(tv['error']) if tv.get('error') else ''}")
    say(f"log: {s['log']}")


def ready(do_check=True):
    if on_mac_usb():
        say(f"warning: {C.SERIAL_PORT} still present; the Flipper looks like it's on the Mac, not the dock")
    up()
    remote("usb-on")
    time.sleep(1.5)
    remote("claim")
    if do_check:
        check()


def check():
    r = subprocess.run(["uv", "run", "-q", "--with", "numpy", "--with", "pillow", "python",
                        os.path.join(HERE, "check_timing.py")], cwd=ROOT)
    if r.returncode:
        say("timing off: `fp sync` on the Mac's USB, or `fp go --safe-us 7000` / 5000 to shift the window")
    return r.returncode


# Get Link into the jump's start state from wherever he is: see
# sequences/oot_setup.txt (B x3 cancel, R wheel + left = Deku Stick on ZR, ZR draw, ZL camera).
SETUP_SEQ = "oot_setup.txt"


# Take controller 1 from whichever controller the owner last used. If another
# controller is active, the first L+R brings up "Press L+R", the second picks the
# Flipper, A confirms. The screen takes a moment to accept A, so A comes late and
# twice; a stray A (or L+R when the Flipper already was controller 1) lands in
# the game, and setup's B presses cancel whatever it opened.
CLAIM = [("L+R", 500, 1000), ("L+R", 500, 1500), ("A", 200, 800), ("A", 200, 800)]


def claim():
    say("claim: L+R, L+R, A, A ...")
    for buttons, ms, gap_ms in CLAIM:
        r = ask({"cmd": "hold", "buttons": buttons, "ms": ms}, timeout=10)
        if not r or r.get("error") or len(r.get("reply") or []) < 2:
            die(f"claim: {buttons} hold failed: {r} (USB controller on? `fp ready`)")
        time.sleep(gap_ms / 1000)


def setup(take_controller=True):
    """Claim controller 1, then play sequences/oot_setup.txt (~9 s). Dies if the Flipper doesn't run it."""
    if take_controller:
        claim()
    say("setup: B x3, R wheel -> Deku Stick, ZR, ZL ...")
    w = ask({"cmd": "write", "local": os.path.join(ROOT, "sequences", SETUP_SEQ), "name": SETUP_SEQ}, timeout=60)
    if not w or not w.get("ok"):
        die(f"setup: write failed: {w}")
    ld = ask({"cmd": "load", "name": SETUP_SEQ}, timeout=20) or {}
    if not (ld.get("reply") or [{}])[0].get("ok"):
        die(f"setup: load failed: {ld}")
    r = ask({"cmd": "go", "delay_ms": 300, "timeout": 15}, timeout=40) or {}
    g = (r.get("go") or [{}])[0]
    if g.get("status") != 0 or not r.get("done"):
        die(f"setup: run failed: {r} (Flipper the controller? `fp ready`)")
    time.sleep(0.3)
    return True


ZR_BURST = "1 ZL ZR\n1 ZL\n1 ZL ZR\n1 ZL\n1 ZL ZR\n"


def variant_burst(extra):
    """The confirmed sequence with `extra` (e.g. 'A', 'LY+') also held through the triple-ZR burst."""
    src = open(os.path.join(ROOT, "sequences", "oot_superjump.txt")).read()
    if src.count(ZR_BURST) != 1:
        die("oot_superjump.txt no longer has the triple-ZR block this variant edits")
    burst = "".join(f"{line} {extra}\n" for line in ZR_BURST.splitlines())
    out = os.path.join(HERE, f".fp_superjump_burst_{extra.replace('+', 'p').replace('-', 'm')}.txt")
    open(out, "w").write(f"# generated by fp_switch2.py: {extra} held through the ZR burst\n" + src.replace(ZR_BURST, burst))
    return out


TURNS = {"left": "-1,0,0,0", "right": "1,0,0,0", "back": "0,-1,0,0", "forward": "0,1,0,0"}


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        say(__doc__.strip())
        return 0
    cmd, rest = argv[0], argv[1:]
    if cmd == "up":
        up()
        status()
    elif cmd == "sync":
        sync(float(rest[0]) if rest else 45)
    elif cmd == "prep":
        sync(float(rest[0]) if rest else 45)
        say("prep done: move the Flipper to the dock, then `fp ready`")
    elif cmd == "ready":
        ready("--no-check" not in rest)
    elif cmd == "check":
        return check()
    elif cmd == "jump":
        loops = next((a for a in rest if a.isdigit()), None) or ("24" if "--capture" in rest else "16")
        if "--no-setup" not in rest:
            setup(take_controller="--no-claim" not in rest)
        elif "--no-claim" not in rest:
            claim()
        passthru = [a for a in rest if not a.isdigit() and a not in ("--claim", "--no-claim", "--no-setup", "--hold-a", "--forward")]
        if "--hold-a" in rest:
            passthru += ["--template", variant_burst("A")]
        elif "--forward" in rest:
            passthru += ["--template", variant_burst("LY+")]
        return remote("superjump", "--loops", loops, *passthru, check=False)
    elif cmd == "setup":
        setup(take_controller="--no-claim" not in rest)
    elif cmd == "claim":
        claim()
    elif cmd == "turn":
        if not rest or rest[0] not in TURNS:
            die("fp turn left|right|back|forward [MS]")
        remote("hold", "NONE", rest[1] if len(rest) > 1 else "450", f"--stick={TURNS[rest[0]]}")
        remote("hold", "ZL", "300")
    elif cmd == "draw":
        remote("hold", "ZR", "120")
    elif cmd == "st":
        status()
    elif cmd == "drift":
        drift()
    elif cmd == "log":
        s = ask({"cmd": "status"}, timeout=10) or die("serve not running")
        os.execvp("tail", ["tail", "-n", rest[0] if rest else "20", "-f", s["log"]])
    elif cmd == "down":
        if "--app" in rest:
            remote("quit-app", check=False)
        if ask({"cmd": "quit"}, timeout=10) is None:
            say("serve was not running")
        else:
            say("serve stopped")
    else:
        return remote(*argv, check=False)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
