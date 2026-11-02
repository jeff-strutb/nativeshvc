#!/bin/sh
# text_ios.sh: tests/unit/text_ios.m as an app for the iOS simulator, run
# there: it checks text entry (nshvc_ext_text_input) itself and exits 0
# when it holds.
#   sh tests/unit/text_ios.sh build [BUILD]   the app, at BUILD/text.app
#                                             (BUILD: build/ios-iphonesimulator)
#   sh tests/unit/text_ios.sh run [BUILD] [DEVICE]
#                                             installed on the simulator
#                                             DEVICE (a name or UDID; the
#                                             booted one, else an iPhone 16,
#                                             booted for it), run, its report
#                                             printed; exit 0 when it holds
set -e
cd "$(dirname "$0")/../.."
B=${2:-build/ios-iphonesimulator}
APP="$B/text.app"
case "$1" in
build)
  cmake --build "$B" >/dev/null
  rm -rf "$APP"
  mkdir -p "$APP"
  xcrun -sdk iphonesimulator clang -target arm64-apple-ios15.0-simulator -O2 -fobjc-arc -Wall -DNSHVC_HOST_IOS=1 \
    -Iinclude tests/unit/text_ios.m "$B/libnshvc.a" -o "$APP/text" \
    -framework UIKit -framework CoreGraphics -framework QuartzCore -framework AudioToolbox \
    -framework AVFoundation -framework GameController -framework Metal -framework Foundation -lm
  cat > "$APP/Info.plist" <<EOP
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>text</string>
  <key>CFBundleExecutable</key><string>text</string>
  <key>CFBundleIdentifier</key><string>com.strutb.nativeshvc.text</string>
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
  <key>UISupportedInterfaceOrientations</key><array><string>UIInterfaceOrientationLandscapeLeft</string></array>
</dict>
</plist>
EOP
  codesign -s - --force "$APP" >/dev/null 2>&1
  echo "$APP"
  ;;
run)
  D=${3:-$(xcrun simctl list devices booted | sed -n 's/.*(\([0-9A-F-]\{36\}\)) (Booted).*/\1/p' | head -1)}
  if [ -z "$D" ]; then
    D=$(xcrun simctl list devices available | sed -n 's/^ *iPhone 16 (\([0-9A-F-]\{36\}\)).*/\1/p' | head -1)
    xcrun simctl boot "$D"
  fi
  xcrun simctl bootstatus "$D" -b >/dev/null
  xcrun simctl install "$D" "$APP"
  xcrun simctl launch --console-pty --terminate-running-process "$D" com.strutb.nativeshvc.text 2>&1 | tee /dev/stderr | grep -q "text_ios: ok"
  ;;
*)
  echo "usage: sh tests/unit/text_ios.sh build|run [BUILD] [DEVICE]" >&2
  exit 2
  ;;
esac
