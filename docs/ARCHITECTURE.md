# RemotePlay Architecture

**Status: Phase 1 of 17 (application skeleton, TCP control channel, session handshake).**
This document describes what is implemented today and how the remaining phases
plug into the same skeleton without redesign.

## 1. System overview

RemotePlay is a host/client application for streaming a locally running game to
remote friends and injecting their controller/keyboard input back into the
game, so local-multiplayer titles behave as if the remote player sat next to
the host.

```
        HOST PC (runs the game)                     CLIENT PC (friend)
+------------------------------+          +------------------------------+
| Qt UI (HostWindow)           |          | Qt UI (ClientWindow)         |
|        | HostApp (facade)    |          |        | ClientApp (facade)  |
|        v                     |          |        v                     |
|   HostSession ---------------+-- TCP -->|   ClientSession              |
|   (asio io_context thread)  |  control |   (asio io_context thread)   |
|     | frame protocol        |  channel |     | frame protocol          |
|     | state machine         |<---------+--     | state machine        |
+------------------------------+ PING etc +------------------------------+
        (Phases 2-11: capture, encode,  |    (Phases 5,7,8: decode, render,
         UDP media, input injection)    |     input capture, overlay)
```

Phase 1 implements the **control channel**: connection, session-code
validation, host approval, capability/codec negotiation, heartbeats, input
permission state, kick/disconnect. Video/audio media transport (Phase 4+) is a
separate UDP path that reuses the session identity established here.

## 2. Module map

| Directory      | Responsibility | Qt-free? | Phase |
|----------------|----------------|----------|-------|
| `common/`      | Version, enums/value types, structured logging, platform paths, JSON config | yes | 1 |
| `networking/`  | Frame encoding (`Packet`), message structs (`Protocol`), Asio TCP transport | yes | 1 |
| `security/`    | Session codes, constant-time comparison, secure random | yes | 1 |
| `host/`        | `HostSession` state machine + `HostApp` facade | yes | 1 |
| `client/`      | `ClientSession` state machine + `ClientApp` facade | yes | 1 |
| `ui/`          | Qt Widgets pages (main/host/client/settings), dark theme | Qt | 1 |
| `app/`         | `main.cpp` | Qt | 1 |
| `capture/`     | Windows Graphics Capture / DXGI desktop & window capture | - | 2 |
| `encoding/`    | NVENC/AMF/QSV/video encoders | - | 3 |
| `decoding/`    | Hardware/software decoders | - | 5 |
| `audio/`       | WASAPI capture, Opus encode/decode | - | 6-7 |
| `input/`       | XInput/Raw Input capture, input injection, virtual gamepad | - | 8-11 |
| `signaling/`   | Signaling client (session discovery by code) | - | 13 |
| `server/`      | Standalone signaling server | - | 13 |
| `tests/`       | Unit + integration test suites (ctest) | yes | 1+ |
| `third_party/` | Vendored standalone Asio, nlohmann/json | - | 1 |

The `remoteplay_core` static library contains everything Qt-free; the `RemotePlay`
executable adds `ui/` and links Qt6::Widgets. This split is deliberate: the
whole session/protocol core is exercised by headless integration tests and by
CI without a display server, and a future dedicated server binary reuses it.

## 3. Threading model

Phase 1 threads:

1. **Qt main thread** - all UI. Never blocks on network operations.
2. **Host network thread** - one `asio::io_context` per `HostSession`, run in a
   dedicated `std::thread` with a work guard. All sockets, timers, and session
   state mutations happen here (single-threaded executor: no strands needed).
3. **Client network thread** - same design in `ClientSession`.

Cross-thread rules:

* Public session methods (`approveClient`, `kickClient`, `setClientInput`,
  `connect`, `stop`) are callable from any thread; they `asio::post` onto the
  network thread.
* Events flow network-thread -> UI via `QMetaObject::invokeMethod(widget, fn,
  Qt::QueuedConnection)`; the widget context object guarantees no invocation
  after widget destruction.
* UI reads live state by **polling thread-safe snapshots**
  (`HostSession::clients()`, `ClientSession::status()`) guarded by a small
  mutex; the control plane is low-frequency (500 ms UI poll), so a mutex is
  appropriate here. The Phase 4 media plane will use lock-free queues instead.
* Statistics counters are atomics or rebuilt snapshots; never read raw session
  internals from the UI thread.

Shutdown is structured so no thread is ever joined from itself:
`ClientSession::finish()` (invoked on the network thread) releases the work
guard and stops the io_context; the thread exits on its own and is reaped by
the next `start()` or the destructor. `stop()` gives a graceful BYE up to
700 ms to flush before forcing the loop down.

## 4. Session handshake (Phase 1 protocol)

```
CLIENT                                HOST (TcpServer on listen port)
  | --- TCP connect ---------------->  |   state: CONNECTING (10 s timeout)
  | --- CLIENT_HELLO (name, code) -->  |   verifySessionCode (constant-time)
  |                                   |   [bad code] -- HOST_REJECT --> close
  |                                   |   state: AUTHENTICATING (75 s window)
  |                                   |   UI: "Mahdyar wants to join" ACCEPT/REJECT
  | <-- HOST_APPROVED ---------------  |
  | <-- HOST_CAPABILITIES ------------ |   host name, game, res/fps/bitrate, codecs
  | --- CLIENT_CAPABILITIES -------->  |   client-supported codec list
  | <-- NEGOTIATION_RESULT ----------  |   negotiated codec (or reject+close)
  |                                   |   both sides: CONNECTED
  | <-- INPUT_PERMISSION ------------  |   initial per-client permissions
  | --- PING t ----------------------> |   every 2 s, both directions
  | <-- PONG t ----------------------- |   RTT = now - t (EWMA smoothed)
  | <-- KICK / BYE ----------------->  |   teardown
```

The full wire format is specified in `docs/PROTOCOL.md`.

## 5. Design decisions worth knowing

* **Standalone Asio (vendored)** over Boost.Asio: zero external dependencies
  for the transport, identical API surface, Boost license. Header-only.
* **Custom binary protocol** over something like protobuf: the Phase 4 UDP
  media path needs a 10-byte header with zero copies; a hand-rolled
  little-endian serializer is ~200 lines, fully testable, and dependency-free.
  Every message has a round-trip unit test.
* **TCP for the control channel** (as the spec requires): authentication,
  approval, negotiation and heartbeats are low-rate and must be reliable.
  Media never goes through this path.
* **Session codes as capability tokens**: 8 characters from a 32-symbol
  unambiguous alphabet = 40 bits of entropy, validated in constant time, with
  a human approval gate on top. Phase 12 upgrades this into the key exchange.
* **Host-authoritative input permissions**: the permission state lives on the
  host, is pushed to the client (`INPUT_PERMISSION`), and is the single
  enforcement point the Phase 8+ input path will consult before forwarding any
  event.
* **Qt-free core**: the entire protocol/session logic compiles and tests
  headless; the GUI is a shell over facades. This keeps CI fast and makes the
  future signaling server share the same code.

## 6. Performance principles (carried forward)

* GPU -> GPU pipelines in later phases; the control channel never allocates in
  steady state except small control frames.
* One dedicated network thread per session; no cross-thread locks on the
  network thread's data (posts, not mutexes), except the UI snapshot mutex.
* Heartbeats are 2 s with a 12-15 s watchdog; the Phase 4 UDP path adds its
  own adaptive jitter buffer and congestion control (spec sections 5, 12, 13).

## 7. Failure handling today

| Failure | Behavior |
|---------|----------|
| Port bind failure | `start()` returns false; UI logs "port unavailable" |
| Client sends garbage/oversized frame | Connection dropped with `PROTOCOL_ERROR` |
| Client presents wrong code | `HOST_REJECT(BAD_SESSION_CODE)`, connection closed |
| Host rejects approval | `HOST_REJECT(REJECTED_BY_HOST)`, connection closed |
| No codec overlap | `NEGOTIATION_RESULT(accepted=false)`, connection closed |
| Heartbeat timeout (12-15 s) | Connection dropped, both sides return to a clean state |
| Host kills the app | Client detects EOF and returns to the join screen |
| Session full (4 clients) | `HOST_REJECT(SERVER_FULL)` |

Crash recovery beyond this (encoder restart dialogs, reconnect) lands with the
streaming phases, per the roadmap.
