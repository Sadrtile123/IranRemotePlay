# Security Model

RemotePlay is a game streaming and input application, **not** remote desktop
software. This document defines what the remote side can and cannot do, what
Phase 1 already enforces, and what the scheduled phases add.

## Non-negotiable restrictions (design-level)

The client can **never**:

* execute CMD, PowerShell, or any process on the host;
* read or write arbitrary files on the host;
* browse the host filesystem;
* install software on the host;
* control applications other than the captured game session;
* escalate privileges.

The protocol has no messages that convey files, processes, shells, or
anything besides: session identity, stream parameters, input events
(Phase 8+), and heartbeats. The input channel is the **only** actuator a
client gets, and it is gated by host-controlled permissions.

## What Phase 1 enforces today

* **Host approval gate**: every join request triggers ACCEPT/REJECT on the
  host; until acceptance, no state is shared beyond the display name. The
  approval window auto-rejects after 60 seconds.
* **Session codes**: 8 characters from a 32-symbol unambiguous alphabet
  (40 bits) drawn from a cryptographically secure source
  (`std::random_device`: rand_s/BCryptGenRandom on Windows,
  /dev/urandom-based on Linux). Validation is constant-time to avoid
  leaking how many characters matched.
* **Input permissions are host-authoritative and default-restrictive**:
  keyboard OFF, mouse OFF, controller ON, vibration ON. The host can revoke
  any input at any time; the state is pushed to the client and is the
  enforcement point for the Phase 8+ input forwarding path.
* **Protocol hardening**: every inbound frame is length-capped (64 KiB),
  magic/version checked, and fully decoded - malformed input drops the
  connection rather than being "best-effort parsed". Unknown message types
  and messages that do not fit the current state are protocol violations.
* **No remote shell surface exists in the codebase**; there is no API in the
  protocol that could carry one.
* **Client identity is a display name only** - no credentials are stored or
  transmitted in Phase 1.

## Honest limitations of Phase 1 (by design of the roadmap)

* The TCP control channel is **not encrypted**. A network observer can read
  session metadata (names, game title, settings) and a man-in-the-middle
  could impersonate either endpoint. **Do not use Phase 1 over untrusted
  networks.** It is intended for LAN play and development.
* Session codes are the only "authentication" right now; they are entry
  tickets, not a proof of identity.

These are scheduled fixes, not permanent states:

| Capability | Phase | Mechanism |
|------------|-------|-----------|
| Authenticated key exchange | 12 | libsodium: X25519 + Ed25519, session code bootstraps the exchange |
| Per-session media encryption | 12 | AES-256-GCM / ChaCha20-Poly1305, keys never leave the peers; replay protection via sequence numbers |
| Signaling confidentiality | 13 | TLS to the signaling server; server sees only routing metadata |
| NAT traversal | 13 | STUN + UDP hole punching; no user port forwarding |
| Relay fallback | 14 | Encrypted-packet relay that cannot inspect the stream |

## Threat model summary

| Threat | Phase 1 | Phase 13+ |
|--------|---------|-----------|
| Random internet scans | Port requires a valid 40-bit code + host approval | Not reachable without signaling-issued tokens |
| Code guessing | 1 in ~1.1e12 per attempt, constant-time check, host sees every attempt | Same, plus rate limiting at the signaling layer |
| Passive eavesdropping | Visible metadata (LAN) | Encrypted end to end |
| Malicious client | Can only request a session and (after approval) send input events, bounded by permission flags | Same, plus authenticated identity |
| Malicious host | Host always has full local control of its own machine; clients should only join hosts they trust | Same |
| Relay operator | n/a | Sees only ciphertext + routing headers |

## Responsible use

Stream only games you own, respect the games' terms of service and local law,
and only join sessions when invited. RemotePlay does not circumvent any DRM
or copy protection.
