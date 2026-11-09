#!/bin/sh
# Usage: run.sh 'python args...'  -> runs tools/<script> inside the Bluetooth-enabled app, waits, prints its log.
B="$(cd "$(dirname "$0")" && pwd)"; TOOLS="$(dirname "$B")"; LOG="$B/last-run.log"
cat > "$B/cmd.sh" <<EOS
#!/bin/sh
export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin HOME="$HOME"
cd "$TOOLS/.." && exec uv run -q --with bleak --with pyserial --with pyobjc-framework-CoreBluetooth python $1 > "$LOG" 2>&1
EOS
: > "$LOG"
open -W -n "$B/FlipperBLETest.app"
cat "$LOG"
