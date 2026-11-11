#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
test "$(uname -s)" = Darwin
swift test --package-path macos -c release
swift build --package-path macos -c release --arch arm64
binary_dir=$(swift build --package-path macos -c release --arch arm64 --show-bin-path)
bundle=dist/LocalForwarder.app
mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources"
cp LICENSE README.md "$bundle/Contents/Resources/"
cp "$binary_dir/LocalForwarder" "$bundle/Contents/MacOS/LocalForwarder"
cat > "$bundle/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.gurujung.LocalForwarder</string>
<key>CFBundleName</key><string>LocalForwarder</string>
<key>CFBundleExecutable</key><string>LocalForwarder</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>15.0</string>
<key>LSUIElement</key><true/>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
codesign --force --sign - "$bundle"
codesign --verify --strict --verbose=2 "$bundle"
python3 tests/integration.py --app "$bundle/Contents/MacOS/LocalForwarder"
ditto -c -k --keepParent "$bundle" dist/LocalForwarder-macos-arm64.zip
stat -f 'macOS artifact bytes: %z' dist/LocalForwarder-macos-arm64.zip
