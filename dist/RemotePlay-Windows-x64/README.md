# RemotePlay

Low-latency remote game streaming and controller input for Windows - an
independent, open implementation for playing **local multiplayer games
together over the Internet**. A host runs the game; friends receive the
video/audio stream and their controllers behave as if plugged into the host
PC. Think "game streaming for couch co-op", not remote desktop.

> Independent project. RemotePlay is not affiliated with Valve/Steam or any
> other streaming product; no proprietary code, protocols, or assets are
> used. Stream only games you own and respect their terms of service.

## Status: All 17 phases complete (v0.1.0)

One app, two roles:

* **HOST** - pick your game window (or a whole monitor), share the code,
  approve players, watch live stats, kick/disable input per player.
* **JOIN** - enter host address + code (LAN) or server + code (Internet),
  play with your own controller, keyboard and mouse.

Feature set (all implemented and building):

* **Video**: DXGI desktop/window capture, FFmpeg encoding with automatic
  NVENC / AMF / QSV hardware encoders and x264 software fallback, H.264 +
  HEVC, 720p-4K at 30/60/120 fps, 2-50 Mbps, letterboxed to your chosen
  resolution so window moves never restart the encoder.
* **Transport**: custom UDP media protocol (30-byte header, fragmentation
  and out-of-order reassembly, sequence-loss detection, adaptive jitter
  buffer, receiver-driven keyframe recovery, PING/PONG RTT/jitter stats).
* **Decoding + display**: threaded low-delay H.264/HEVC/AV1 software
  decoding, aspect-preserving render, F10 diagnostics overlay, fullscreen,
  connection-quality banner.
* **Audio**: WASAPI loopback capture of whatever the game plays, Opus
  64-192 kbps, shared timestamps for A/V sync, low-latency WASAPI playback.
* **Input**: XInput + DirectInput controllers, 8 ms polling, change-only
  sending; virtual Xbox 360 pads on the host via ViGEm (vibration echoes
  back to the player's real pad); keyboard/mouse events with host-side
  per-player permission enforcement and stuck-key release safety.
* **Security**: ECDH P-256 key exchange bound to the session code,
  HKDF-SHA256 key schedule, AES-256-GCM on every UDP datagram (header is
  AAD), 64-entry replay window, fail-closed transport, plaintext rejection
  after key confirmation. The relay only ever sees ciphertext.
* **Internet mode**: single-file Python signaling + relay server (join by
  code, no port forwarding); relay TCP carries the session handshake;
  encrypted UDP media flows via relay with automatic direct-path upgrade
  when NAT permits, falling back to the relay if the direct path breaks.
* **Adaptive bitrate**: hysteresis controller on RTT/loss/jitter/fps with
  proportional steps and cooldown; manual slider override.
* **Crash recovery**: encoder failures offer Restart / Switch-to-software;
  capture loss auto-recovers; client disconnects release held keys and
  remove virtual pads.
* **Tests**: 10 suites (packet, protocol, session codes, handshake, UDP,
  ABR, crypto, signaling+relay integration with the real Python server,
  plus the server's own test) - green on Linux and Windows CI.
* **Distribution**: portable zip (no installer, no admin) + verification
  tools (`rp_tool_capturedump`, `rp_tool_encodetest`); GitHub Actions
  builds the MSVC release and publishes it on tags.

### v1 known limits (documented, by design)

* One full CPU copy in the capture path and one in the render path
  (see `docs/PERFORMANCE.md`); GPU-direct encode/decode is future work.
* Software H.264 decoding (hardware decode is future work); 1080p60 is
  comfortable on modern CPUs.
* The TCP control channel is not itself encrypted; the session-code-bound
  key exchange protects all media and input. Use the TLS options in
  `docs/DEPLOYING.md` for the signaling server.
* Internet mode without a deployed server: put the included `server.py` on
  any VPS (one command, no dependencies).

## Quick start (Windows 10/11 x64)

Prerequisites: Visual Studio 2022 (Desktop C++ workload), Qt 6.5+ MSVC 64-bit,
CMake 3.22+. Asio and nlohmann/json are vendored - nothing else to install.

```bat
git clone https://github.com/Sadrtile123/IranRemotePlay.git
cd IranRemotePlay
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.1/msvc2022_64"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\bin\Release\RemotePlay.exe
```

Full instructions (including the Qt installer and `windeployqt`): see
[docs/BUILDING.md](docs/BUILDING.md).

### Two-minute tour (Phase 1)

1. Host: **HOST GAME** -> set game/resolution/fps/bitrate/codec (defaults:
   1080p, 60 FPS, 8 Mbps, H.264) -> **START HOSTING** -> a session code and
   your LAN IP appear.
2. Friend: **JOIN SESSION** -> enter name + host IP:port + code ->
   **CONNECT**.
3. Host clicks **ACCEPT** on the join request.
4. Both sides show live connection state, negotiated stream parameters, and
   ping. Host can kick players or disable their input; client disconnects
   gracefully with BYE.

## Repository layout

```
RemotePlay/
├── app/          entry point
├── common/       version, types, logging, paths, JSON config
├── networking/   framing, protocol messages, Asio TCP transport
├── security/     session codes, constant-time verification
├── host/         HostSession state machine + HostApp facade
├── client/       ClientSession state machine + ClientApp facade
├── ui/           Qt Widgets pages (main/host/client/settings)
├── capture/ encoding/ decoding/ audio/ input/ signaling/ server/   (future phases)
├── tests/        packet, protocol, session-codes, handshake suites
├── docs/         architecture, protocol, building, testing, security, roadmap
└── third_party/  vendored Asio 1.30.2 + nlohmann/json 3.11.3
```

The Qt-free core (`remoteplay_core`) is deliberately separated from the GUI so
the whole protocol/session layer is testable headless and reusable by the
future signaling server.

## Documentation

* [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - components, threading, handshake flow
* [docs/PROTOCOL.md](docs/PROTOCOL.md) - byte-level wire specification
* [docs/BUILDING.md](docs/BUILDING.md) - VS 2022 build instructions
* [docs/TESTING.md](docs/TESTING.md) - automated + manual test procedures
* [docs/SECURITY.md](docs/SECURITY.md) - threat model and hard restrictions
* [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) - vendored and planned dependencies
* [docs/ROADMAP.md](docs/ROADMAP.md) - all 17 phases with current status

## Security highlights

The remote client can never execute commands, read files, or browse the host -
the only actuator is input events, gated by host-controlled, default-off
permissions. Phase 1's control channel is plain TCP and intended for
LAN/trusted testing; authenticated encryption (per-session
AES-256-GCM/ChaCha20-Poly1305) lands in Phase 12. Details:
[docs/SECURITY.md](docs/SECURITY.md).

## License

MIT - see [LICENSE](LICENSE). Third-party licenses live in `third_party/`
(Asio: Boost Software License; nlohmann/json: MIT).
