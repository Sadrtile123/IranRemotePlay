# Roadmap (development phases)

Phase 1 is complete; each subsequent phase lands as its own reviewable
change set with tests and docs updates.

| # | Phase | Status | Deliverables |
|---|-------|--------|--------------|
| 1 | Qt application skeleton, TCP control channel, session handshake | **DONE** | CMake project; host/client modes; Asio TCP transport; frame protocol; session codes; approval flow; capability + codec negotiation; heartbeats/RTT; input permission state; kick; dark Qt UI (main/host/client/settings); JSON config; structured logging; 4 test suites; CI workflow |
| 2 | Window/screen capture | next | Windows Graphics Capture (window-first, monitor fallback); D3D11 GPU textures; no CPU copies; game/window picker |
| 3 | H.264 encoding | planned | NVENC (NVIDIA), AMF (AMD), QSV (Intel) via FFmpeg; low-latency presets (no B-frames, short GOP); bitrate control; encode-latency stats; capture -> encode -> file test path |
| 4 | Network video streaming | planned | UDP media transport per docs/PROTOCOL.md section 5; fragmentation/reassembly; sequence loss detection; adaptive jitter buffer; A/V timestamping |
| 5 | Client decoding + rendering | planned | Hardware decode (D3D11VA/NVDEC); GPU -> screen path; 1080p60 end to end; F10 stats overlay; decoder capability advertising (replaces the hardcoded H.264 client list) |
| 6 | WASAPI audio capture | planned | Loopback capture, 48 kHz stereo, low-latency buffers |
| 7 | Opus audio streaming | planned | Opus 64-192 kbps; A/V sync via shared timestamps |
| 8 | XInput controller capture | planned | Client-side XInput/DirectInput polling; input packet serialization |
| 9 | Controller transmission | planned | Host-side input permission enforcement on the live path; latency compensation basics |
| 10 | Virtual controller | planned | Remote controller appears as a local gamepad (isolated driver component, separate install step, documented); vibration backchannel |
| 11 | Keyboard/mouse transmission | planned | Raw Input capture; SendInput injection; host on/off switches per client |
| 12 | Session authentication & crypto | planned | libsodium key exchange bootstrapped by the session code; per-session AEAD keys; replay protection |
| 13 | STUN + NAT traversal + signaling | planned | Standalone signaling server (TLS); join-by-code; UDP hole punching |
| 14 | Relay fallback | planned | Encrypted-packet relay for failed hole punches |
| 15 | Adaptive bitrate | planned | RTT/loss/jitter/decode-queue driven rate control with hysteresis and smoothing |
| 16 | UI polish | planned | Game library (Add Game by executable), stream view overlay, connection quality banners, crash-recovery dialogs (encoder restart etc.) |
| 17 | Installer | planned | Bundles RemotePlay.exe, Qt DLLs, FFmpeg/Opus DLLs; Start Menu shortcut; optional driver step explained; no admin unless a component genuinely needs it |

Testing per phase: unit tests for each new module plus the integration
matrix (LAN/Internet, 1/3/5/10% loss, 20-250 ms latency, multiple clients)
defined in the spec's testing section.
