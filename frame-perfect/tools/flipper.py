#!/usr/bin/env python3
"""flipper: generic Flipper Zero helpers from the Mac, for any task.

Nothing here knows about Frame Perfect or the Switch; task scripts (such as
fp_switch2.py) import it.

  flipper where                      is it on the Mac's USB (serial port), and which name it uses
  flipper cli 'COMMAND' [--no-wait]  one USB CLI command, prints the reply (e.g. 'info device', 'ps', 'storage list /ext')
  flipper log [flipper_log.py args]  stream the system log over USB (default: Bluetooth lines)
  flipper start PATH [ARGS]          start an app: USB CLI `loader open` if plugged in, else Bluetooth RPC
  flipper close                      close the running app (USB only: `loader close`)
  flipper upload LOCAL REMOTE        upload a file over Bluetooth RPC (MD5-checked)
  flipper ble 'SCRIPT ARGS'          run a tools/ script inside the Bluetooth-enabled app wrapper
                                     (macOS gives Bluetooth only to that app); waits, prints its output
  flipper awake PID                  keep the Mac awake until process PID exits

Settings (Flipper name, serial port) come from fp_config.py / private/config.json.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, HERE)
import fp_config as C  # noqa: E402

ROOT = os.path.dirname(HERE)  # scripts run from here, so 'tools/x.py' paths work
RUN = os.path.join(HERE, "ble-app", "run.sh")
RUN_LOG = os.path.join(HERE, "ble-app", "last-run.log")


def say(*a):
    print(*a, flush=True)


def die(msg, code=1):
    print(f"{os.path.basename(sys.argv[0])}: {msg}", file=sys.stderr)
    sys.exit(code)


def on_usb():
    """True when the Flipper's CLI serial port is on the Mac."""
    return os.path.exists(C.SERIAL_PORT)


def cli(command, wait=True):
    """One USB CLI command; returns its output."""
    if not on_usb():
        raise RuntimeError(f"{C.SERIAL_PORT} not found: the Flipper isn't on the Mac's USB")
    args = [sys.executable, os.path.join(HERE, "flipper_cli.py"), command] + ([] if wait else ["--no-wait"])
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True).stdout


def ble(script_args, background=False):
    """Run 'tools/x.py args' inside the Bluetooth app wrapper.

    Foreground: waits and returns its output. Background: returns at once; the
    output goes to ble-app/last-run.log (shared: a later run overwrites it)."""
    if background:
        subprocess.Popen([RUN, script_args], cwd=ROOT, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
        return None
    r = subprocess.run([RUN, script_args], cwd=ROOT, capture_output=True, text=True)
    return (r.stdout + r.stderr).strip()


def last_ble_output():
    return open(RUN_LOG).read() if os.path.exists(RUN_LOG) else ""


def start(path, args=""):
    """Start an app; USB if plugged in, else Bluetooth RPC. Returns (ok, transport, output)."""
    if on_usb():
        out = cli(f"loader open {path} {args}".strip())
        ok = "error" not in out.lower() and "can't" not in out.lower()
        return ok, "usb", out.strip()
    out = ble(f"tools/ble_rpc.py start {path} {args}".strip())
    return "status 0" in out, "bluetooth", out


def keep_awake(pid):
    """caffeinate -is until pid exits; no-op if one is already watching it."""
    if subprocess.run(["pgrep", "-f", f"caffeinate -is -w {pid}"], capture_output=True).returncode:
        subprocess.Popen(["caffeinate", "-is", "-w", str(pid)], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        say(__doc__.strip())
        return 0
    cmd, rest = argv[0], argv[1:]
    try:
        if cmd == "where":
            say(f"name: {C.FLIPPER_NAME or '(any)'}   USB: {C.SERIAL_PORT if on_usb() else 'not on the Mac'}")
        elif cmd == "cli":
            if not rest:
                die("flipper cli 'COMMAND' [--no-wait]")
            say(cli(rest[0], "--no-wait" not in rest).strip())
        elif cmd == "log":
            os.execv(sys.executable, [sys.executable, os.path.join(HERE, "flipper_log.py"), *rest])
        elif cmd == "start":
            if not rest:
                die("flipper start PATH [ARGS]")
            ok, how, out = start(rest[0], " ".join(rest[1:]))
            say(f"start over {how}: {'ok' if ok else 'FAILED'}")
            if not ok:
                say(out)
            return 0 if ok else 1
        elif cmd == "close":
            say(cli("loader close").strip())
        elif cmd == "upload":
            if len(rest) < 2:
                die("flipper upload LOCAL REMOTE")
            say(ble(f"tools/ble_rpc.py upload {os.path.abspath(rest[0])} {rest[1]}"))
        elif cmd == "ble":
            if not rest:
                die("flipper ble 'tools/SCRIPT.py ARGS'")
            say(ble(" ".join(rest)))
        elif cmd == "awake":
            keep_awake(int(rest[0]))
        else:
            die(f"unknown command {cmd} (flipper --help)")
    except RuntimeError as e:
        die(str(e))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
