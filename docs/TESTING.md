# Testing RemotePlay

## Automated tests (ctest)

Run everything: `ctest --test-dir build -C Release --output-on-failure`
(Windows) or `ctest --test-dir build-verify` (Linux/CI).

| Suite | File | What it verifies |
|-------|------|------------------|
| packet | `tests/test_packet.cpp` | Frame build/parse round-trips; magic/version/length validation; header rejects short buffers; byte writer/reader round-trips (u8..u64, strings, underflow behavior, string cap) |
| protocol | `tests/test_protocol.cpp` | Every message type round-trips through encode/decode; unknown ids rejected; truncated and trailing-garbage payloads rejected; codec negotiation precedence (host preference order, H.264 fallback, no-overlap failure) |
| session codes | `tests/test_session_codes.cpp` | Code format (XXXX-XXXX), 32-char unambiguous alphabet, 1000-code uniqueness, normalization of sloppy input (lowercase, missing dash, spaces), rejection of ambiguous characters, constant-time equality, correct/incorrect verification |
| handshake | `tests/test_handshake.cpp` | Full in-process integration over real loopback TCP: HELLO -> auto-approval -> capability exchange -> H.264 negotiation -> CONNECTED; bidirectional heartbeat RTT sanity (< 250 ms loopback); kick delivery; wrong-code rejection; manual ACCEPT flow with approval gating (no connection before approval); INPUT_PERMISSION disable delivery; graceful BYE disconnect; codec negotiation failure with AV1-only client |

The handshake suite is the key end-to-end check: a real `HostSession` and a
real `ClientSession`, each on its own network thread, speaking the actual
wire protocol over `127.0.0.1` sockets.

## Manual test (two players, Phase 1 scope)

Phase 1 has no streaming yet - this procedure validates the session/handshake
experience end to end. It can run on one PC (two instances) or two PCs on a
LAN.

### Setup
1. Build per `docs/BUILDING.md`.
2. Run `build\bin\Release\RemotePlay.exe` on the host machine.
3. Run a second instance on the friend's machine (or the same machine).

### Host side
1. Click **HOST GAME**.
2. Fill *Game* (e.g. `Rayman Legends`), pick capture mode, resolution
   (default 1080p), frame rate (default 60), codec (default H.264), bitrate
   (default 8 Mbps), listen port (default 41717).
3. Click **START HOSTING**.
4. Expected:
   * A session code appears (format `ABCD-EFGH`).
   * The local IP addresses and port are shown to give the friend.
   * Log lines: `Session created (code ..., port ...)`.

### Client side
1. Click **JOIN SESSION**.
2. Enter your name, the host's IP and port, and the session code
   (typos like `abc7 k92p` are normalized automatically).
3. Click **CONNECT**.
4. Expected:
   * Status moves CONNECTING -> WAITING FOR HOST APPROVAL.
   * An error appears if the code is wrong (`Host rejected: Invalid session code.`).

### Approval
1. On the host, a dialog "X wants to join your session" appears with
   ACCEPT / REJECT and a 60-second auto-reject countdown.
2. Click **ACCEPT**.
3. Expected on the client: status CONNECTED, "Connected to: Host's PC",
   game name, negotiated stream parameters, and a live ping (updates every
   500 ms).
4. Expected on the host: the player table lists the player with state
   CONNECTED and a live latency value (from the host-side heartbeat).

### Controls to exercise
* **DISABLE INPUT** on the host -> the client shows "Input: disabled by
  host"; **ENABLE INPUT** reverses it. (Input data itself arrives in Phase 8;
  the permission state and enforcement point are live now.)
* **KICK** -> the client disconnects with "Kicked by host".
* **DISCONNECT** on the client -> graceful BYE; the host's table empties.
* **STOP HOSTING** -> all clients disconnect; the host returns to setup mode.
* Close either application -> the other side returns to a clean disconnected
  state within a heartbeat or two (no crash).

### Verification in the logs
`%LOCALAPPDATA%\RemotePlay\Logs\remoteplay_<date>.log` should contain lines like:

```
[INFO] Session created (code ABC7-K92P, port 41717)
[INFO] Client 'Mahdyar' connected from 192.168.1.20:51344 (RemotePlay 0.1.0)
[INFO] Host accepted client 'Mahdyar'
[INFO] Negotiating H.264
[INFO] Client 'Alex' disconnected
```

## Known limitations (Phase 1)

* Direct IP connection only: the friend needs host IP + port + code. The
  signaling server (Phase 13) will reduce this to just the code, and NAT
  traversal removes the reachability requirement.
* The control channel is plain TCP: **not yet encrypted**. Treat Phase 1 as
  LAN/trusted-network testing. Authenticated encryption (per-session keys,
  AES-256-GCM/ChaCha20-Poly1305) is scheduled with Phase 12-13 - see
  `docs/SECURITY.md`.
* No video/audio yet (Phases 2-7), no input forwarding yet (Phases 8-11).
* The "Controller" column in the player table is a placeholder row label
  until Phase 8 lands real controller state.
