#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
test "$(uname -s)" = Darwin
if [[ "${GITHUB_ACTIONS:-}" != true || "${RUNNER_ENVIRONMENT:-}" != github-hosted ]]; then
    echo "Real login-item fixture requires disposable GitHub-hosted CI." >&2
    exit 2
fi

mkdir -p dist
evidence_dir=$(cd dist && pwd)
fixture_root=$(mktemp -d "${RUNNER_TEMP:?}/LocalForwarder-login-item.XXXXXX")
bundle="$fixture_root/LocalForwarderLoginItemTest.app"
executable="$bundle/Contents/MacOS/LocalForwarderLoginItemTest"
identifier="com.gurujung.LocalForwarder.LoginItemTest.$(uuidgen | tr '[:upper:]' '[:lower:]')"
cleanup() {
    local result=$?
    trap - EXIT
    if [[ -x "$executable" ]]; then
        "$executable" cleanup "$evidence_dir/macos-login-item-cleanup-evidence.json" || result=1
    fi
    # fixture_root is the exact mktemp directory created above, never the product app.
    rm -rf -- "$fixture_root"
    exit "$result"
}
trap cleanup EXIT
mkdir -p "$bundle/Contents/MacOS"
xcrun swiftc -parse-as-library -target arm64-apple-macosx15.0 \
    macos/Sources/LocalForwarderCore/LoginItemController.swift \
    macos/Sources/LocalForwarderCore/MacLoginItemService.swift \
    scripts/login-item-fixture.swift -o "$executable"
cat > "$bundle/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>$identifier</string>
<key>CFBundleName</key><string>LocalForwarderLoginItemTest</string>
<key>CFBundleExecutable</key><string>LocalForwarderLoginItemTest</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>15.0</string>
<key>LSUIElement</key><true/>
</dict></plist>
PLIST
codesign --force --sign - "$bundle"
codesign --verify --strict --verbose=2 "$bundle"
"$executable" exercise "$evidence_dir/macos-login-item-evidence.json"
