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
run_fixture() {
    local mode=$1
    local evidence=$2
    # Match opening the packaged application in Finder, including LaunchServices app identity.
    # open's own exit code is not the app's exit code, so require explicit result evidence too.
    open -W -n -g --env GITHUB_ACTIONS=true --env RUNNER_ENVIRONMENT=github-hosted \
        "$bundle" --args "$mode" "$evidence" || return 1
    python3 - "$evidence" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
if not path.is_file():
    raise SystemExit("Fixture did not write evidence: " + str(path))
data = json.loads(path.read_text())
print(json.dumps(data, indent=2, sort_keys=True))
if data.get("passed") is not True or data.get("cleanupSucceeded") is not True:
    raise SystemExit("Real login-item fixture failed; see evidence above")
PY
}
cleanup() {
    local result=$?
    trap - EXIT
    if [[ -x "$executable" ]]; then
        run_fixture cleanup "$evidence_dir/macos-login-item-cleanup-evidence.json" || result=1
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
run_fixture exercise "$evidence_dir/macos-login-item-evidence.json"
