# RemotePlay Wire Protocol (Phase 1)

This document specifies the **implemented** TCP control-channel protocol
byte-for-byte, and the **planned** UDP media datagram format that arrives with
Phase 4. Any implementation must match this document; the tests in
`tests/test_packet.cpp` and `tests/test_protocol.cpp` enforce it.

## 1. TCP frame format

All integers are **little-endian**. One frame per message:

| Offset | Size | Field        | Value                                  |
|--------|------|--------------|----------------------------------------|
| 0      | 2    | magic        | `0x5250` ("RP")                        |
| 2      | 1    | version      | `0x01`                                 |
| 3      | 2    | msgType      | `Id` value, see section 2              |
| 5      | 1    | flags        | `0x00` (reserved)                      |
| 6      | 4    | payloadSize  | 0 .. 65536 (`kMaxControlPayload`)      |

Payload: `payloadSize` bytes, serialized per message (section 3).

Validation rules (violations drop the connection with reason
`PROTOCOL_ERROR`):

* magic and version must match exactly;
* payloadSize must not exceed `kMaxControlPayload`;
* the frame body must decode fully: no trailing bytes, no truncation;
* unknown msgType values are protocol violations.

## 2. Message types

| Id     | Name               | Direction | Purpose                                  |
|--------|--------------------|-----------|------------------------------------------|
| 0x0001 | CLIENT_HELLO       | C -> H    | name, session code, app version          |
| 0x0002 | HOST_REJECT        | H -> C    | reject + disconnect (reason code)        |
| 0x0003 | HOST_APPROVED      | H -> C    | host accepted the join request           |
| 0x0004 | HOST_CAPABILITIES  | H -> C    | host/game identity + stream parameters   |
| 0x0005 | CLIENT_CAPABILITIES| C -> H    | codecs the client can decode             |
| 0x0006 | NEGOTIATION_RESULT | H -> C    | final stream parameters or failure       |
| 0x0007 | PING               | both      | heartbeat, carries sender timestamp      |
| 0x0008 | PONG               | both      | echoes PING timestamp for RTT            |
| 0x0009 | INPUT_PERMISSION   | H -> C    | per-client input permission state        |
| 0x000A | KICK               | H -> C    | host kicked the client                   |
| 0x000B | BYE                | both      | graceful disconnect                      |
| 0x0010 | STREAM_START       | reserved  | Phase 4 (UDP media begin)                |
| 0x0011 | STREAM_STOP        | reserved  | Phase 4                                  |
| 0x0012 | KEYFRAME_REQUEST   | reserved  | Phase 4 (loss recovery)                  |

Reason codes used by HOST_REJECT / disconnects:

| Code | Name              | Meaning                              |
|------|-------------------|--------------------------------------|
| 0    | OK                | clean disconnect                     |
| 1    | BAD_SESSION_CODE  | code did not match (constant-time)  |
| 2    | REJECTED_BY_HOST  | host clicked REJECT                  |
| 3    | PROTOCOL_ERROR    | malformed/unexpected message         |
| 4    | UNSUPPORTED_VERSION | frame version mismatch             |
| 5    | INACTIVITY_TIMEOUT | heartbeat watchdog fired           |
| 6    | KICKED            | host removed the client              |
| 7    | SERVER_FULL       | 4 clients already connected          |
| 8    | NO_COMMON_CODEC   | codec negotiation failed             |

## 3. Payload serialization

Primitive encoding (little-endian, bounds-checked reader):

| Primitive | Encoding                              |
|-----------|---------------------------------------|
| u8 / bool | 1 byte (bool: 0 or 1)                 |
| u16       | 2 bytes LE                            |
| u32       | 4 bytes LE                            |
| u64       | 8 bytes LE                            |
| string    | u16 length + UTF-8 bytes (max 1024)   |
| codec list| u8 count + count x u8 codec code      |

Video codec codes: `0` = H.264, `1` = HEVC, `2` = AV1.
Capture mode codes: `0` = window, `1` = monitor.

### 3.1 CLIENT_HELLO
```
string clientName      (display name, e.g. "Mahdyar")
string sessionCode     (e.g. "ABC7-K92P")
string appVersion      (e.g. "0.1.0", informational)
```

### 3.2 HOST_REJECT
```
u16    reasonCode      (Reason table)
string reasonText      (human-readable)
```

### 3.3 HOST_APPROVED
```
(empty payload)
```

### 3.4 HOST_CAPABILITIES
```
string hostName        ("Mahdyar's PC")
string gameName        ("Rayman Legends")
u8     captureMode     (0 window / 1 monitor)
u16    width           (1920)
u16    height          (1080)
u8     fps             (60)
u32    bitrateKbps     (8000)
codecList offeredCodecs (preference order, H.264 always included)
```

### 3.5 CLIENT_CAPABILITIES
```
codecList supportedCodecs
```

### 3.6 NEGOTIATION_RESULT
```
bool   accepted
u8     codec           (negotiated codec code)
u16    width
u16    height
u8     fps
u32    bitrateKbps
string note            (e.g. "H.264" or the failure explanation)
```

### 3.7 PING / 3.8 PONG
```
u64 epochMs            (PING: sender's steady-clock ms; PONG: verbatim echo)
```

### 3.9 INPUT_PERMISSION
```
bool controller
bool keyboard
bool mouse
bool vibration
```

### 3.10 KICK / 3.11 BYE
```
string reason
```

## 4. Session state machines

Client states: `CONNECTING -> AUTHENTICATING -> CONNECTED -> DISCONNECTED`
(the `STREAMING` state is entered in Phase 4).

Host side per-client states mirror these. Timeouts:

| State          | Timeout | Rationale                          |
|----------------|---------|------------------------------------|
| CONNECTING     | 10 s    | client must send HELLO promptly    |
| AUTHENTICATING | 75 s    | human approval window (60 s UI)   |
| CONNECTED      | 15 s    | heartbeats every 2 s both ways     |

RTT is computed by the ping originator as `now - echo` and smoothed with an
EWMA (`rtt = rtt*7/8 + sample/8`, first sample seeds it).

## 5. Planned UDP media datagram (Phase 4 - NOT YET IMPLEMENTED)

The values below are the committed design for the media transport so the
control channel can stay stable; they will be implemented and tested in
Phase 4. Every datagram:

```
u16 magic         0x5252
u8  version       0x01
u8  type          UdpType: 0 VIDEO, 1 AUDIO, 2 INPUT, 3 CONTROL, 4 PING,
                  5 PONG, 6 KEYFRAME_REQUEST, 7 STREAM_START, 8 STREAM_STOP
u8  flags         (fragment bit, keyframe bit, ...)
u32 sessionId
u64 sequence      per-type monotonic
u16 fragmentIndex
u16 fragmentCount
u64 timestampNs   capture timestamp for A/V sync
u16 payloadSize
payload bytes
```

Receivers implement: sequence-gap loss detection, fragment reassembly, a
small adaptive jitter buffer (config `network.jitterBufferMs`), and
KEYFRAME_REQUEST on unrecoverable loss. Media packets are encrypted with
per-session keys derived in Phase 12; the relay (Phase 14) forwards
ciphertext only.
