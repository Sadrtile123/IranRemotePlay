# Roadmap (development phases)

Phase 1 is complete; each subsequent phase lands as its own reviewable
change set with tests and docs updates.

| # | Phase | Status | Deliverables |
|---|-------|--------|--------------|
| 1 | Qt application skeleton, TCP control channel, session handshake | **DONE** | CMake project; host/client modes; Asio TCP transport; frame protocol; session codes; approval flow; capability + codec negotiation; heartbeats/RTT; input permission state; kick; dark Qt UI; JSON config; structured logging; tests; CI |
| 2 | Window/screen capture | **DONE** | DXGI Output Duplication; D3D11 staging readback; per-frame window-crop capture (client-rect tracking); monitor enumeration; access-lost recovery; capture_dump verification tool |
| 3 | H.264 encoding | **DONE** | FFmpeg pipeline; NVENC/AMF/QSV probe + fallback to x264/x265; no B-frames; short VBV; CBR-ish; runtime bitrate target; letterbox to fixed encode size; encode_test tool |
| 4 | Network video streaming | **DONE** | UDP protocol per spec (30-byte header, 9 types); fragmentation/reassembly (out-of-order tolerant); sequence loss detection; adaptive jitter buffer; PING/PONG RTT; auto keyframe request on loss; 5 test suites |
| 5 | Client decoding + rendering | **DONE** | FFmpeg H.264/HEVC/AV1 software decode (threaded, low-delay); BGRA conversion; VideoWidget (aspect-preserving, F10 overlay, fullscreen); real decoder capability advertising |
| 6 | WASAPI audio capture | **DONE** | Loopback capture of the default render device; swresample to 48 kHz stereo s16; 10-20 ms blocks; capture thread with COM MTA |
| 7 | Opus audio streaming | **DONE** | libopus 20 ms frames (64-192 kbps); s16 FIFO framing; A/V shared microsecond timestamps; client decoder + WASAPI player with latency-capped buffer |
| 8 | XInput controller capture | **DONE** | XInput (dynamic xinput1_4/xinput9_1_0) + DirectInput8 legacy pads with axis normalization; 8 ms polling; change-only sends |
| 9 | Controller transmission | **DONE** | 22-byte packed wire state; host live permission enforcement per packet; InputInjector stats; key-release safety on disconnect |
| 10 | Virtual controller | **DONE** | ViGEm client via runtime DLL load; per-player Xbox 360 pads; vibration callback -> rumble backchannel to the client's physical pad; graceful degradation without the driver |
| 11 | Keyboard/mouse transmission | **DONE** | Qt key/mouse events -> 18-byte event packets (VK + scancode on host); absolute normalized mouse; SendInput injection; per-client permission gating |
| 12 | Session authentication & crypto | **DONE** | CNG ECDH P-256 + HKDF-SHA256 keyed by the session code; AES-256-GCM datagram sealing (header = AAD); 64-entry replay window; fail-closed transport; test suite with known vectors |
| 13 | STUN + NAT traversal + signaling | **DONE** | Standalone Python signaling server (join-by-code, player slots, public-address reporting); SignalingClient (C++); relay TCP pairing that carries the existing session protocol; relay UDP bind |
| 14 | Relay fallback | **DONE** | UDP media relay (opaque ciphertext forwarding); RPBIND token auth; automatic direct-path upgrade on authenticated direct packets; DEPLOYING.md |
| 15 | Adaptive bitrate | **DONE** | Hysteresis controller (streaks, neutral zone, cooldown, proportional steps); RTT/loss/jitter/fps-driven; 7 unit tests |
| 16 | UI polish | **DONE** | HostCoordinator/ClientCoordinator orchestration; capture source picker (windows + monitors); internet mode; live stats panels; approval + encoder crash-recovery dialogs; connection quality banner; gamepad status |
| 17 | Installer | **DONE** | Portable zip distribution (no admin): RemotePlay.exe + Qt/FFmpeg runtimes + verification tools + server + docs; CI builds MSVC release + auto-publishes GitHub Releases on tags |

All phases complete. Remaining improvements are tracked as optimization work
(GPU-direct encode path, hardware decode, in-game overlay) — see
docs/PERFORMANCE.md for the honest latency budget.
