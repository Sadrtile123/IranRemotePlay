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
for fmt in qico qgif qjpeg qsvg; do
    cp "$QTW/plugins/imageformats/$fmt.dll" "$OUT/imageformats/" 2>/dev/null || true
done

# FFmpeg shared runtime
for dll in avcodec-63.dll avutil-61.dll swscale-10.dll swresample-7.dll; do
    cp "$FF/bin/$dll" "$OUT/"
done

# MinGW runtime (C++/threading)
for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    cp "$MINGW_LIB/$dll" "$OUT/" 2>/dev/null || true
done

# Signaling server (optional deployment) + docs
mkdir -p "$OUT/server/signaling-server"
cp "$ROOT/server/signaling-server/server.py" "$OUT/server/signaling-server/"
cp "$ROOT/server/signaling-server/test_server.py" "$OUT/server/signaling-server/"

cp "$ROOT/README.md" "$OUT/README.md"
cp "$ROOT/LICENSE" "$OUT/LICENSE"
cp "$ROOT/docs/DEPLOYING.md" "$OUT/docs-DEPLOYING.md" 2>/dev/null || true
cp "$ROOT/docs/BUILDING.md" "$OUT/docs-BUILDING.md" 2>/dev/null || true

cat > "$OUT/RUN-THIS-FIRST.txt" <<'EOF'
RemotePlay 0.1.1 - Windows 10/11 x64
====================================

WHAT'S NEW IN 0.1.1
  * Fixed the "Stream start failed: no usable encoder (frame alloc failed)"
    crash - streaming now starts.
  * Fixed host freeze / "not responding" (dialog storm + thread deadlocks).
  * F11 fullscreen now works (window-level, also double-click; Esc exits).
  * New dark UI, stats grid, copy-code + open-logs buttons, recent hosts.
  * Client shows a clear error if the host never starts the stream.

QUICK START
  1. Run RemotePlay.exe (no installation needed).
  2. Host: pick your game window, choose LAN or Internet mode, share the code.
  3. Join: enter the address + code, click Join.

GAMEPADS (remote players appear as Xbox 360 controllers on the host)
  Gamepad streaming needs the free ViGEmBus driver (one-time install):
    https://github.com/nefarius/ViGEmBus/releases
  Install it ONLY on the HOST PC. Without it, keyboard/mouse streaming still
  works and RemotePlay tells you gamepads are unavailable.

FOR PLAYING OVER THE INTERNET (no port forwarding)
  Deploy the included signaling server on any VPS:
    python3 server/signaling-server/server.py --port 9000
  Hosts select "Internet mode" and enter your server address.
  All media + input is end-to-end encrypted (AES-256-GCM); the server only
  relays ciphertext.

VERIFY YOUR INSTALL (optional, run from cmd):
  tools\rp_tool_capturedump.exe 3     - tests screen capture
  tools\rp_tool_encodetest.exe        - tests the video encoder

Requires a GPU or CPU with H.264 encode (hardware encoders NVENC/AMF/QSV are
used automatically when present; otherwise libx264).
EOF

echo "Package contents:"
(cd "$OUT" && ls -R | head -40)
echo "Size: $(du -sh "$OUT" | cut -f1)"

ZIP=/home/z/my-project/download/RemotePlay-0.1.1-Windows-x64.zip
rm -f "$ZIP"
(cd /home/z/my-project/RemotePlay/dist && zip -qr "$ZIP" RemotePlay-Windows-x64)
echo "ZIP: $ZIP ($(du -h "$ZIP" | cut -f1))"
