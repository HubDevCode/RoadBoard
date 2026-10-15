#!/usr/bin/env bash
# Build a portable AppImage (runs on any recent distribution with PipeWire).
# Usage: packaging/appimage/build-appimage.sh   (from the repository root)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${WORK:-$ROOT/build-appimage}"
TOOLS="$WORK/tools"
mkdir -p "$TOOLS"
ARCH="$(uname -m)"

fetch() { [ -f "$TOOLS/$1" ] || curl -L --fail -o "$TOOLS/$1" "$2"; chmod +x "$TOOLS/$1"; }
fetch linuxdeploy.AppImage "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage"
fetch linuxdeploy-plugin-qt.AppImage "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage"

cmake -S "$ROOT" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$WORK/build"
rm -rf "$WORK/AppDir"
DESTDIR="$WORK/AppDir" cmake --install "$WORK/build"

# PipeWire itself must come from the host (it talks to the running daemon):
# never bundle libpipewire / libspa.
# the strip bundled with linuxdeploy cannot handle .relr.dyn sections of recent distributions
export NO_STRIP=1
export EXTRA_QT_PLUGINS="svg;wayland-shell-integration;wayland-decoration-client;wayland-graphics-integration-client"
# native Wayland + X11, offscreen for headless use
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqoffscreen.so"
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
cd "$WORK"
APPIMAGE_EXTRACT_AND_RUN=1 "$TOOLS/linuxdeploy.AppImage" --appdir AppDir \
  --desktop-file AppDir/usr/share/applications/io.github.roadboard.RoadBoard.desktop \
  --icon-file AppDir/usr/share/icons/hicolor/scalable/apps/io.github.roadboard.RoadBoard.svg \
  --exclude-library 'libpipewire-0.3.so*' --exclude-library 'libasound.so*' \
  --plugin qt --output appimage
echo "AppImage created in $WORK"
