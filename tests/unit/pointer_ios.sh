#!/bin/sh
# pointer_ios.sh: tests/unit/pointer_ios.c as an app for the iOS simulator,
# and the check of what it wrote.
#   sh tests/unit/pointer_ios.sh build      the app, at build/ios-iphonesimulator/
#                                           pointer.app (signed ad hoc)
#   sh tests/unit/pointer_ios.sh check FILE each touch in its pointer.txt
#                                           against the dot and line its place
#                                           on the picture is
set -e
cd "$(dirname "$0")/../.."
B=build/ios-iphonesimulator
case "$1" in
build)
  cmake --build "$B" >/dev/null
  APP="$B/pointer.app"
  rm -rf "$APP"
  mkdir -p "$APP"
  xcrun -sdk iphonesimulator clang -target arm64-apple-ios15.0-simulator -O2 -std=gnu11 -Wall -DNSHVC_HOST_IOS=1 \
    -Iinclude tests/unit/pointer_ios.c "$B/libnshvc.a" -o "$APP/pointer" \
    -framework UIKit -framework CoreGraphics -framework QuartzCore -framework AudioToolbox \
    -framework AVFoundation -framework GameController -framework Metal -framework Foundation -lm
  cat > "$APP/Info.plist" <<EOP
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>pointer</string>
  <key>CFBundleExecutable</key><string>pointer</string>
  <key>CFBundleIdentifier</key><string>com.strutb.nativeshvc.pointer</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleSupportedPlatforms</key><array><string>iPhoneSimulator</string></array>
  <key>MinimumOSVersion</key><string>15.0</string>
  <key>LSRequiresIPhoneOS</key><true/>
  <key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
  <key>UILaunchScreen</key><dict/>
  <key>UIRequiresFullScreen</key><true/>
  <key>UIStatusBarHidden</key><true/>
  <key>UISupportedInterfaceOrientations</key><array><string>UIInterfaceOrientationPortrait</string></array>
</dict>
</plist>
EOP
  codesign -s - --force "$APP" >/dev/null 2>&1
  echo "$APP"
  ;;
check)
  python3 - "$2" <<'EOP'
import sys
lines = open(sys.argv[1]).read().split("\n")
w, h = map(float, lines[0].split()[1:3])
pw, ph = (h * 4 / 3, h) if w / h > 4 / 3 else (w, w * 3 / 4)
x0, y0 = (w - pw) / 2, (h - ph) / 2
fails = n = 0
for l in lines[1:]:
    if not l.startswith("touch"):
        continue
    f = l.split()
    x, y, latched, dot, line = float(f[1]), float(f[2]), int(f[6]), int(f[8]), int(f[10])
    u, v = (x - x0) / pw, (y - y0) / ph
    on = 0 <= u < 1 and 0 <= v < 1
    ed, el = int(u * 256), int(v * 224)
    ok = (latched and dot == ed and line == el) if on else not latched
    n += 1
    fails += not ok
    print("%s touch (%.1f, %.1f): %s; the console latched %d: dot %d line %d" % (
        "ok  " if ok else "FAIL", x, y, "dot %d line %d" % (ed, el) if on else "off the picture", latched, dot, line))
print("%d touch(es), %d check(s) failed" % (n, fails))
sys.exit(1 if fails or not n else 0)
EOP
  ;;
*)
  echo "pointer_ios.sh build | check FILE" >&2
  exit 2
  ;;
esac
