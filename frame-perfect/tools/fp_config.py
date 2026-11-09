"""Local settings for the Mac-side tools.

Defaults are generic. Put your own values in `<repo>/private/config.json`
(git-ignored) or in environment variables:

    {
      "flipper_name": "MyFlipper",          # the Flipper's name (Settings > Desktop > Name); env FP_FLIPPER_NAME
      "serial_port": "/dev/cu.usbmodemflip_MyFlipper1",              # env FLIPPER_PORT
      "tv_host": "root@192.168.1.50",       # rooted LG webOS TV for the frame grid; env FP_TV_HOST
      "tv_ssh_key": "~/.ssh/id_ed25519",    # env FP_TV_SSH_KEY
      "tv_capture_dir": "/path/to/tv-capture"   # PicCap-based capture scripts; env FP_TVCAP
    }

Without a TV, `fp_remote.py serve --fake-grid` sends a fixed 60 Hz grid with an
unknown phase (see the README's "Without a rooted TV").
"""
import glob
import json
import os

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(TOOLS, "..", ".."))
_path = os.environ.get("FP_CONFIG", os.path.join(REPO, "private", "config.json"))
_cfg = json.load(open(_path)) if os.path.exists(_path) else {}


def _get(key, env, default=None):
    v = os.environ.get(env) or _cfg.get(key) or default
    return os.path.expanduser(v) if isinstance(v, str) and v.startswith("~") else v


def _first_port():
    ports = sorted(glob.glob("/dev/cu.usbmodemflip_*"))
    return ports[0] if ports else "/dev/cu.usbmodemflip_Flipper1"


FLIPPER_NAME = _get("flipper_name", "FP_FLIPPER_NAME")  # None = match any Flipper
STOCK_BLE_NAME = FLIPPER_NAME  # the stock Bluetooth profile advertises the Flipper's name
REMOTE_BLE_NAME = f"FP {FLIPPER_NAME}" if FLIPPER_NAME else None  # Frame Perfect's remote profile
SERIAL_PORT = _get("serial_port", "FLIPPER_PORT", _first_port())
TV_HOST = _get("tv_host", "FP_TV_HOST")
TV_SSH_KEY = _get("tv_ssh_key", "FP_TV_SSH_KEY")
TVCAP_DIR = _get("tv_capture_dir", "FP_TVCAP")


def is_remote_name(name):
    """Frame Perfect's remote profile names itself 'FP <flipper name>'."""
    if not name:
        return False
    return name == REMOTE_BLE_NAME if REMOTE_BLE_NAME else name.startswith("FP ")


def need_tvcap():
    if not TVCAP_DIR:
        raise SystemExit("No TV capture configured: set tv_capture_dir in private/config.json (or FP_TVCAP).")
    return TVCAP_DIR
