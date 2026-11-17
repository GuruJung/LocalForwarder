#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
test "$(uname -s)" = Darwin
python3 scripts/version.py --check
python3 -m unittest discover -s tests -p 'test_release*.py'
version=$(python3 scripts/version.py --print)
# Probe registration early, but still collect independent build/test evidence if it fails.
login_item_result=0
if [[ "${GITHUB_ACTIONS:-}" == true && "${RUNNER_ENVIRONMENT:-}" == github-hosted ]]; then
    if bash scripts/test-macos-login-item.sh; then
        echo "Real login-item registration fixture passed."
    else
        login_item_result=$?
        echo "Real login-item registration fixture failed; continuing native checks before returning failure." >&2
    fi
else
    echo "Skipping real login-item registration outside disposable GitHub-hosted CI (unit tests still run)."
fi
swift test --package-path macos -c release
swift build --package-path macos -c release --arch arm64
binary_dir=$(swift build --package-path macos -c release --arch arm64 --show-bin-path)
bundle=dist/LocalForwarder.app
mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources"
swift scripts/generate-macos-icon.swift "$bundle/Contents/Resources/LocalForwarder.icns"
test -s "$bundle/Contents/Resources/LocalForwarder.icns"
cp LICENSE README.md README_ko.md "$bundle/Contents/Resources/"
cp "$binary_dir/LocalForwarder" "$bundle/Contents/MacOS/LocalForwarder"
cat > "$bundle/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.gurujung.LocalForwarder</string>
<key>CFBundleName</key><string>LocalForwarder</string>
<key>CFBundleExecutable</key><string>LocalForwarder</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>${version}</string>
<key>CFBundleVersion</key><string>${version}</string>
<key>CFBundleIconFile</key><string>LocalForwarder</string>
<key>LSMinimumSystemVersion</key><string>15.0</string>
<key>LSUIElement</key><true/>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$bundle/Contents/Info.plist")" = "$version"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$bundle/Contents/Info.plist")" = "$version"
python3 scripts/package-info.py --platform macos --arch arm64 --executable "$bundle/Contents/MacOS/LocalForwarder" --output "$bundle/Contents/Resources/build-info.json"
codesign --force --sign - "$bundle"
codesign --verify --strict --verbose=2 "$bundle"
"$bundle/Contents/MacOS/LocalForwarder" --test-ui
python3 tests/integration.py --app "$bundle/Contents/MacOS/LocalForwarder"
archive="dist/LocalForwarder-${version}-macos-arm64.zip"
ditto -c -k --keepParent "$bundle" "$archive"
stat -f 'macOS artifact bytes: %z' "$archive"

# A fixture failure remains a required-check failure even when all other checks pass.
exit "$login_item_result"
