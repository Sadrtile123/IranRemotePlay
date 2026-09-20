#!/bin/bash
# Package the RemotePlay Windows x64 distribution (locally cross-compiled).
set -euo pipefail

ROOT=/home/z/my-project/RemotePlay
QTW=/home/z/qtwin/6.8.1/mingw_64
FF=/home/z/toolchain/ffmpeg/ffmpeg-master-latest-win64-gpl-shared
MINGW_LIB=/home/z/toolchain/root/usr/x86_64-w64-mingw32/lib
OUT=/home/z/my-project/RemotePlay/dist/RemotePlay-Windows-x64

rm -rf "$OUT"
mkdir -p "$OUT/platforms" "$OUT/styles" "$OUT/imageformats" "$OUT/tools"

# Application
cp "$ROOT/build-qt/bin/RemotePlay.exe" "$OUT/"
cp "$ROOT/build-qt/bin/rp_tool_capturedump.exe" "$OUT/tools/" 2>/dev/null || true
cp "$ROOT/build-qt/bin/rp_tool_encodetest.exe" "$OUT/tools/" 2>/dev/null || true

# Qt runtime
for dll in Qt6Core.dll Qt6Gui.dll Qt6Widgets.dll; do
    cp "$QTW/bin/$dll" "$OUT/"
done
cp "$QTW/plugins/platforms/qwindows.dll" "$OUT/platforms/"
cp "$QTW/plugins/styles/qmodernwindowsstyle.dll" "$OUT/styles/" 2>/dev/null || true
# NOTE: qsvg is intentionally NOT shipped - it needs Qt6Svg.dll which the app
# does not otherwise require (no SVG is ever rendered).
for fmt in qico qgif qjpeg; do
    cp "$QTW/plugins/imageformats/$fmt.dll" "$OUT/imageformats/" || exit 1
done

# FFmpeg shared runtime
for dll in avcodec-63.dll avutil-61.dll swscale-10.dll swresample-7.dll; do
    cp "$FF/bin/$dll" "$OUT/"
done

# MinGW runtime. CRITICAL: Qt's own MinGW-built DLLs (Qt6Core/Gui/Widgets,
# qwindows, styles) dynamically import libgcc_s_seh-1.dll and libstdc++-6.dll.
# The Qt distribution ships exactly matching copies in its own bin/ directory -
# use those (the Debian cross toolchain only has import stubs, no real DLLs).
# Fail HARD if any runtime DLL is missing: a silent skip here previously
# produced the "libgcc_s_seh-1.dll was not found" launch error on user PCs.
for dll in libgcc_s_seh-1.dll libstdc++-6.dll; do
    if [ ! -f "$QTW/bin/$dll" ]; then
        echo "FATAL: $QTW/bin/$dll not found" >&2; exit 1
    fi
    cp "$QTW/bin/$dll" "$OUT/"
done
if [ ! -f "$MINGW_LIB/libwinpthread-1.dll" ]; then
    echo "FATAL: $MINGW_LIB/libwinpthread-1.dll not found" >&2; exit 1
fi
cp "$MINGW_LIB/libwinpthread-1.dll" "$OUT/"

# Signaling server (optional deployment) + docs
mkdir -p "$OUT/server/signaling-server"
cp "$ROOT/server/signaling-server/server.py" "$OUT/server/signaling-server/"
cp "$ROOT/server/signaling-server/test_server.py" "$OUT/server/signaling-server/"

cp "$ROOT/README.md" "$OUT/README.md"
cp "$ROOT/LICENSE" "$OUT/LICENSE"
cp "$ROOT/docs/DEPLOYING.md" "$OUT/docs-DEPLOYING.md" 2>/dev/null || true
cp "$ROOT/docs/BUILDING.md" "$OUT/docs-BUILDING.md" 2>/dev/null || true

cp "$ROOT/RUN-THIS-FIRST.txt" "$OUT/RUN-THIS-FIRST.txt"

echo "Package contents:"
(cd "$OUT" && ls -R | head -40)
echo "Size: $(du -sh "$OUT" | cut -f1)"

ZIP=/home/z/my-project/download/RemotePlay-0.1.1-Windows-x64.zip
rm -f "$ZIP"
(cd /home/z/my-project/RemotePlay/dist && zip -qr "$ZIP" RemotePlay-Windows-x64)
echo "ZIP: $ZIP ($(du -h "$ZIP" | cut -f1))"

# Post-packaging dependency audit: every import of every binary must resolve
# against the package or a Windows system DLL. Prevents regressions of the
# missing-runtime-DLL class of bug from ever shipping again.
echo "Running dependency audit..."
python3 "$ROOT/scripts/audit_deps.py" || exit 1
