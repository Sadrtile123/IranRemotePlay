# Deploying the RemotePlay signaling server (Internet mode)

RemotePlay clients and hosts can connect through a signaling + relay server
so nobody needs to open ports. The server is a single dependency-free
Python file; any small VPS works (1 vCPU / 512 MB easily serves dozens of
concurrent sessions).

## Quick start

```bash
python3 server.py --port 9000
```

Hosts choose **Internet mode** in the app and enter `your-vps-address`.
Players join with the 8-character code shown by the host.

## What the server does (and does NOT do)

- Signaling: session codes, player slots, NAT info (it tells each side the
  other's public address — equivalent to STUN).
- TCP relay: pairs host/client connections and blindly pipes bytes between
  them. The RemotePlay handshake runs through this pipe unchanged.
- UDP media relay: forwards opaque datagrams between the peers of a
  session. **The server cannot read any stream content** — video, audio and
  input are end-to-end encrypted with AES-256-GCM; the session code is the
  authentication secret. See `security/SecureChannel.h`.
- Sessions expire after 6 hours of inactivity.

## Direct-path upgrade

Each peer's public address is exchanged during join. Both peers keep
probing each other directly; datagrams that arrive from the peer's real
address (and authenticate with session keys) switch the transport to the
direct path automatically. If the direct path breaks, media falls back to
the relay.

## TLS (recommended for production)

Option A — built-in:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 365
python3 server.py --port 9000 --tls-cert cert.pem --tls-key key.pem
```

Option B — reverse proxy (nginx/stunnel) in front of the plain server.

Without TLS, an on-path attacker can read signaling metadata (player names,
addresses, codes) and inject relay traffic — but still cannot decrypt or
forge stream content. For private/friendly use this is acceptable; for a
public service, use TLS.

## systemd unit

```ini
[Unit]
Description=RemotePlay signaling server
After=network.target

[Service]
ExecStart=/usr/bin/python3 /opt/remoteplay/server.py --port 9000
Restart=always
DynamicUser=yes

[Install]
WantedBy=multi-user.target
```

## Tests

```bash
cd server/signaling-server
python3 test_server.py
```

Covers registration, code validation, player slots, public-address
reporting, TCP relay pairing with opaque byte piping, and UDP media
forwarding.
