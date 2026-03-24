#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build}"
BINARY_NAME="foo_taglookup"
INPUT_BIN="$BUILD_DIR/${BINARY_NAME}.dylib"
OUT_DIR="$ROOT_DIR/dist"
BUNDLE_DIR="$OUT_DIR/${BINARY_NAME}.component"
CONTENTS_DIR="$BUNDLE_DIR/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"
PLIST_PATH="$CONTENTS_DIR/Info.plist"

if [[ ! -f "$INPUT_BIN" ]]; then
  echo "Built binary not found: $INPUT_BIN"
  echo "Build first: cmake -S . -B build && cmake --build build --config Release"
  exit 1
fi

mkdir -p "$MACOS_DIR"
cp "$INPUT_BIN" "$MACOS_DIR/$BINARY_NAME"
chmod 755 "$MACOS_DIR/$BINARY_NAME"

cat > "$PLIST_PATH" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleExecutable</key>
  <string>foo_taglookup</string>
  <key>CFBundleIdentifier</key>
  <string>com.foobar2000.foo-taglookup</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleName</key>
  <string>foo_taglookup</string>
  <key>CFBundlePackageType</key>
  <string>BNDL</string>
  <key>CFBundleShortVersionString</key>
  <string>0.1.11</string>
  <key>CFBundleVersion</key>
  <string>0.1.11</string>
</dict>
</plist>
PLIST

codesign --force --sign - "$BUNDLE_DIR" >/dev/null 2>&1 || true

echo "Created component bundle: $BUNDLE_DIR"
echo "Install by copying it to:"
echo "  ~/Library/Application Support/foobar2000-v2/components"
