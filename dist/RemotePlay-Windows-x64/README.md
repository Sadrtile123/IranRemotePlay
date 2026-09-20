# RemotePlay

Low-latency remote game streaming and controller input for Windows - an
independent, open implementation for playing **local multiplayer games
together over the Internet**. A host runs the game; friends receive the
video/audio stream and their controllers behave as if plugged into the host
PC. Think "game streaming for couch co-op", not remote desktop.

> Independent project. RemotePlay is not affiliated with Valve/Steam or any
> other streaming product; no proprietary code, protocols, or assets are
> used. Stream only games you own and respect their terms of service.

## Status: Phase 1 of 17

Done (this repository):

* Full CMake + Qt 6 (C++20, MSVC 2022) application skeleton - host mode and
  client mode in one app with a modern dark UI.
* Standalone-Asio TCP control channel with a framed binary protocol.
* Complete session handshake: session codes (`ABC7-K92P` style), host
  ACCEPT/REJECT approval, capability exchange, H.264 codec negotiation
  (HEVC/AV1 negotiation ready), heartbeats with live RTT.
* Multi-client session management: player table with latency, kick, and
  per-player input enable/disable (host authority, default-restrictive).
* Structured logging (`%LOCALAPPDATA%\RemotePlay\Logs`) and JSON settings
  (`%APPDATA%\RemotePlay\config.json`).
* Four automated test suites, including a full in-process host<->client
  handshake integration test over real sockets.
* GitHub Actions CI building and testing on Windows (MSVC + Qt).

Coming next (see `docs/ROADMAP.md`): Windows Graphics Capture (Phase 2),
NVENC/AMF/QSV H.264 encoding (Phase 3), UDP media streaming (Phase 4),
decoding/rendering (Phase 5), WASAPI + Opus audio (6-7), controllers and
virtual gamepad (8-10), keyboard/mouse (11), authenticated encryption (12),
STUN/NAT traversal + signaling + relay (13-14), adaptive bitrate (15),
polished UI (16), installer (17).

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
