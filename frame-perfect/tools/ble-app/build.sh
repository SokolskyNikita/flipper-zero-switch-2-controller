#!/bin/sh
# Builds FlipperBLETest.app: a tiny launcher bundle that runs cmd.sh (written by
# run.sh) as its child. macOS then checks *this* app's Bluetooth permission
# instead of the terminal's, so Python/bleak can use Bluetooth. Ad-hoc signed.
# First run: allow "Flipper BLE Test" under System Settings > Privacy & Security > Bluetooth.
set -e
B="$(cd "$(dirname "$0")" && pwd)"
APP="$B/FlipperBLETest.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cc -O2 -o "$APP/Contents/MacOS/launcher" "$B/launcher.c"
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>launcher</string>
  <key>CFBundleIdentifier</key><string>local.frameperfect.bletest</string>
  <key>CFBundleName</key><string>Flipper BLE Test</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSUIElement</key><true/>
  <key>NSBluetoothAlwaysUsageDescription</key><string>Talks to the Flipper Zero over Bluetooth for Frame Perfect.</string>
</dict>
</plist>
PLIST
codesign --force --sign - "$APP"
echo "built $APP"
