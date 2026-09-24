#!/bin/zsh
# Package the knit border daemon as a proper menu bar app bundle.
# LSUIElement keeps it out of the Dock. Ad-hoc signing verifies the local
# bundle; it is not Developer ID signing or notarization for distribution.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZIP="$ROOT/outputs/Window Sweaters.zip"
STAGE="$(mktemp -d /tmp/window-sweaters-build.XXXXXX)"
trap 'trash "$STAGE"' EXIT

make -C "$ROOT" >/dev/null

STAGED="$STAGE/Window Sweaters.app/Contents"
mkdir -p "$STAGED/MacOS"
cp -f "$ROOT/bin/borders"     "$STAGED/MacOS/WindowSweaters"
cp -f "$ROOT/AppInfo.plist"   "$STAGED/Info.plist"
mkdir -p "$STAGED/Resources" "$STAGE/AppIcon.iconset"
clang -O2 -fobjc-arc -I"$ROOT/src" "$ROOT/scripts/build-icon.m" -framework Cocoa -o "$STAGE/build-icon"
"$STAGE/build-icon" "$STAGE/AppIcon.iconset"
iconutil -c icns "$STAGE/AppIcon.iconset" -o "$STAGED/Resources/AppIcon.icns"
chmod +x "$STAGED/MacOS/WindowSweaters"

xattr -cr "$STAGE/Window Sweaters.app"
codesign --force --deep --sign - "$STAGE/Window Sweaters.app"
codesign --verify --deep --strict "$STAGE/Window Sweaters.app"

mkdir -p "$ROOT/outputs"
if [[ -e "$ZIP" ]]; then trash "$ZIP"; fi
ditto -c -k --keepParent --norsrc "$STAGE/Window Sweaters.app" "$ZIP"
mkdir -p "$STAGE/verify"
ditto -x -k "$ZIP" "$STAGE/verify"
codesign --verify --deep --strict "$STAGE/verify/Window Sweaters.app"
echo "$ZIP"
