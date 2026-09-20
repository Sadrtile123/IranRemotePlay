#!/usr/bin/env python3
"""RemotePlay signaling + relay server (Phases 13-14).

Dependency-free asyncio server you deploy on any VPS.

  1. Signaling (TLS-capable TCP, newline JSON):
       host_register  -> session created, 8-char code issued
       join           -> player slot assigned, host + client notified with a
                         per-player RELAY TOKEN
  2. NAT info (STUN-equivalent): the server reports to each side the other
     side's public address as the server itself observes it.
  3. TCP relay pairing: host + client connect to relay_port with the same
     token; the server blindly pipes bytes between them (opaque; the existing
     RemotePlay session protocol runs over the pipe unchanged).
  4. UDP media relay: binds one datagram socket per session; peers
     authenticate with RPBIND + token; the server forwards OPAQUE encrypted
     datagrams host<->clients and cannot read stream content (end-to-end
     AES-256-GCM; see security/SecureChannel.h).
  5. Direct-path upgrade: the signaling channel tells both sides the peer's
     public address; RemotePlay endpoints probe each other directly and
     authenticate datagrams with session keys, so the relay leaves the trust
     path automatically once direct packets flow.

Usage:
    python3 server.py [--host 0.0.0.0] [--port 9000] [--tls-cert c.pem --tls-key k.pem]
Tests:
    python3 test_server.py
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import secrets
import signal
import sys
from dataclasses import dataclass, field
from typing import Optional

log = logging.getLogger("remoteplay-signaling")

MAX_PLAYERS = 4
SESSION_TTL_SECONDS = 6 * 3600
CODE_ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"   # unambiguous


@dataclass
class ClientSlot:
    player_index: int
    name: str
    token: str
    signaling_conn: "SignalingConn" = None
    public: tuple = (0, 0)          # (ip, port) as the server sees the client
    relay_attached: bool = False


@dataclass
class Session:
    session_id: int
    code: str
    host_name: str
    host_conn: "SignalingConn" = None
    host_token: str = ""
    host_public: tuple = (0, 0)
    clients: dict = field(default_factory=dict)     # player_index -> ClientSlot
    created_at: float = 0.0
    tcp_relay_port: int = 0
    udp_relay_port: int = 0
    tcp_relay_server: object = None
    udp_relay: "UdpSessionRelay" = None


class Registry:
    def __init__(self) -> None:
        self.by_id: dict[int, Session] = {}
        self.by_code: dict[str, Session] = {}

    def create(self, host_name: str) -> Session:
        while True:
            code = "".join(secrets.choice(CODE_ALPHABET) for _ in range(8))
            if code not in self.by_code:
                break
        s = Session(session_id=len(self.by_id) + 1, code=code, host_name=host_name,
                    created_at=asyncio.get_event_loop().time())
        self.by_id[s.session_id] = s
        self.by_code[code] = s
        log.info("session %d created by %r code=%s", s.session_id, host_name, code)
        return s

    def lookup_code(self, code: str) -> Optional[Session]:
        return self.by_code.get(code.strip().upper())

    def free_slot(self, s: Session) -> Optional[int]:
        for i in range(MAX_PLAYERS):
            if i not in s.clients:
                return i
        return None

    def remove(self, s: Session) -> None:
        self.by_code.pop(s.code, None)
        self.by_id.pop(s.session_id, None)
        if s.tcp_relay_server:
            s.tcp_relay_server.close()
        if s.udp_relay and s.udp_relay.transport:
            s.udp_relay.transport.close()
        log.info("session %d removed", s.session_id)

    def expire_old(self) -> None:
        now = asyncio.get_event_loop().time()
        for s in [x for x in list(self.by_id.values()) if now - x.created_at > SESSION_TTL_SECONDS]:
            self.remove(s)


REGISTRY = Registry()


# ----------------------------------------------------------------------------
# Signaling connection

class SignalingConn:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self.reader = reader
        self.writer = writer
        self.session: Optional[Session] = None
        self.role: Optional[str] = None
        self.name = ""

    def public(self) -> tuple:
        peer = self.writer.get_extra_info("peername")
        return (peer[0], peer[1]) if peer else ("", 0)

    async def send_json(self, obj: dict) -> None:
        try:
            self.writer.write((json.dumps(obj) + "\n").encode())
            await self.writer.drain()
        except Exception:
            pass

    async def run(self) -> None:
        try:
            while True:
                line = await self.reader.readline()
                if not line:
                    break
                line = line.strip()
                if not line or len(line) > 65536:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    await self.send_json({"type": "error", "reason": "bad json"})
                    continue
                await self.handle(msg)
        except (ConnectionResetError, asyncio.IncompleteReadError):
            pass
        except Exception:
            log.exception("signaling connection error")
        finally:
            await self.on_disconnect()

    async def handle(self, msg: dict) -> None:
        t = msg.get("type")
        if t == "host_register":
            await self.on_host_register(msg)
        elif t == "join":
            await self.on_join(msg)
        elif t == "keepalive":
            await self.send_json({"type": "keepalive_ok"})
        elif t == "leave":
            await self.on_leave()
        else:
            await self.send_json({"type": "error", "reason": f"unknown type {t!r}"})

    async def on_host_register(self, msg: dict) -> None:
        if self.role:
            await self.send_json({"type": "error", "reason": "already registered"})
            return
        REGISTRY.expire_old()
        self.role = "host"
        self.name = str(msg.get("name", "Host"))[:64]
        s = REGISTRY.create(self.name)
        self.session = s
        s.host_conn = self
        s.host_token = secrets.token_hex(16)
        s.host_public = self.public()
        tcp_port, udp_port = await start_session_relays(s)
        s.tcp_relay_port, s.udp_relay_port = tcp_port, udp_port
        await self.send_json({
            "type": "host_registered",
            "session_id": s.session_id,
            "code": s.code,
            "max_players": MAX_PLAYERS,
            "host_token": s.host_token,
            "relay_tcp_port": tcp_port,
            "relay_udp_port": udp_port,
        })

    async def on_join(self, msg: dict) -> None:
        if self.role:
            await self.send_json({"type": "error", "reason": "already registered"})
            return
        code = str(msg.get("code", ""))[:16].strip().upper()
        name = str(msg.get("name", "Player"))[:64]
        s = REGISTRY.lookup_code(code)
        if not s or not s.host_conn:
            await self.send_json({"type": "join_rejected", "reason": "unknown session code"})
            return
        idx = REGISTRY.free_slot(s)
        if idx is None:
            await self.send_json({"type": "join_rejected", "reason": "session full"})
            return
        token = secrets.token_hex(16)
        self.role = "client"
        self.name = name
        self.session = s
        slot = ClientSlot(player_index=idx, name=name, token=token,
                          signaling_conn=self, public=self.public())
        s.clients[idx] = slot
        register_pair(token, s, idx)   # TCP relay pairing entry for this player

        await self.send_json({
            "type": "joined",
            "session_id": s.session_id,
            "player_index": idx,
            "host_name": s.host_name,
            "token": token,
            "relay_tcp_port": s.tcp_relay_port,
            "relay_udp_port": s.udp_relay_port,
            "host_public": {"addr": s.host_public[0], "port": s.host_public[1]},
        })
        # Tell the host about the new player + its public address (NAT info).
        await s.host_conn.send_json({
            "type": "client_joined",
            "player_index": idx,
            "name": name,
            "token": token,
            "client_public": {"addr": slot.public[0], "port": slot.public[1]},
        })

    async def on_leave(self) -> None:
        log.info("on_leave: role=%s", self.role)
        s, self.session = self.session, None
        if not s:
            return
        if self.role == "client":
            for idx, slot in list(s.clients.items()):
                if slot.signaling_conn is self:
                    s.clients.pop(idx, None)
                    if s.host_conn:
                        await s.host_conn.send_json({"type": "client_left", "player_index": idx})
        elif self.role == "host":
            for slot in s.clients.values():
                await slot.signaling_conn.send_json({"type": "session_closed", "reason": "host left"})
            REGISTRY.remove(s)

    async def on_disconnect(self) -> None:
        log.info("conn disconnect: role=%s name=%r", self.role, self.name)
        try:
            await self.on_leave()
        finally:
            try:
                self.writer.close()
                await self.writer.wait_closed()
            except Exception:
                pass


# ----------------------------------------------------------------------------
# TCP relay: pair (host, client) by shared token, then pipe opaque bytes.

PAIRS: dict[str, dict] = {}   # token -> {"session", "player_index", "host": pipe|None, "client": pipe|None, "ready": Event}


async def handle_relay_tcp(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    role = token = None
    try:
        line = await asyncio.wait_for(reader.readline(), timeout=15)
        msg = json.loads(line.strip() or b"{}")
        if msg.get("type") != "relay":
            writer.write((json.dumps({"type": "relay_rejected", "reason": "expected relay handshake"}) + "\n").encode())
            await writer.drain()
            return
        token = msg.get("token", "")
        role = msg.get("role")
        pair = PAIRS.get(token)
        if not pair or role not in ("host", "client"):
            writer.write((json.dumps({"type": "relay_rejected", "reason": "unknown token"}) + "\n").encode())
            await writer.drain()
            return

        writer.write((json.dumps({"type": "relay_accepted"}) + "\n").encode())
        await writer.drain()
        pair[role] = (reader, writer)
        if pair["host"] and pair["client"]:
            pair["ready"].set()
        await pair["ready"].wait()
        other = pair["client" if role == "host" else "host"]
        if other is None:
            return
        writer.write((json.dumps({"type": "relay_paired"}) + "\n").encode())
        await writer.drain()

        # Blind byte pipe; content is opaque to this server.
        other_writer = other[1]
        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                other_writer.write(data)
                await other_writer.drain()
        except Exception:
            pass
        finally:
            try:
                other_writer.close()
            except Exception:
                pass
    except Exception:
        log.exception("relay tcp error")
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass
        if token and role and token in PAIRS:
            PAIRS[token][role] = None
            if PAIRS[token]["host"] is None and PAIRS[token]["client"] is None:
                PAIRS.pop(token, None)


async def start_session_relays(s: Session) -> tuple[int, int]:
    # TCP relay (shared port; tokens disambiguate players).
    async def handler(reader, writer):
        await handle_relay_tcp(reader, writer)

    srv = await asyncio.start_server(handler, "0.0.0.0", 0)
    tcp_port = srv.sockets[0].getsockname()[1]
    s.tcp_relay_server = srv

    # UDP relay (one socket per session).
    loop = asyncio.get_event_loop()
    transport, proto = await loop.create_datagram_endpoint(
        lambda: UdpSessionRelay(s), local_addr=("0.0.0.0", 0))
    udp_port = transport.get_extra_info("sockname")[1]
    s.udp_relay = proto
    return tcp_port, udp_port


def register_pair(token: str, session: Session, player_index: int) -> None:
    PAIRS[token] = {"session": session, "player_index": player_index,
                    "host": None, "client": None, "ready": asyncio.Event()}


# ----------------------------------------------------------------------------
# UDP media relay (opaque forwarding)

class UdpSessionRelay(asyncio.DatagramProtocol):
    """Forwards encrypted datagrams between the host and clients of a session.

    Authentication: RPBIND<token> registers the sender's public endpoint.
    The host binds with host_token; clients bind with their pair token.
    Forwarding rules:
      - from host endpoint  -> to every bound client endpoint
      - from client endpoint -> to the host endpoint only
    Payload bytes are never parsed (opaque ciphertext; endpoints authenticate
    with their session keys, not with this server's word).
    """

    def __init__(self, session: Session):
        self.session = session
        self.transport: Optional[asyncio.DatagramTransport] = None
        self.host_endpoint: Optional[tuple] = None
        self.client_endpoints: dict[int, tuple] = {}    # player_index -> addr

    def connection_made(self, transport) -> None:
        self.transport = transport

    def token_owner(self, token: str):
        s = self.session
        if token == s.host_token:
            return ("host", -1)
        for idx, slot in s.clients.items():
            if slot.token == token:
                return ("client", idx)
        return (None, -1)

    def datagram_received(self, data: bytes, addr: tuple) -> None:
        if len(data) < 6:
            return
        if data[:6] == b"RPBIND":
            token = data[6:38].decode(errors="ignore")
            who, idx = self.token_owner(token)
            if who == "host":
                if self.host_endpoint is None or addr != self.host_endpoint:
                    log.debug("session %d: host udp bound %s", self.session.session_id, addr)
                self.host_endpoint = addr
                self.transport.sendto(b"RPBINDOK", addr)
            elif who == "client":
                self.client_endpoints[idx] = addr
                self.transport.sendto(b"RPBINDOK", addr)
                log.debug("session %d: player %d udp bound %s", self.session.session_id, idx + 1, addr)
            return

        # Opaque media: route by source endpoint.
        if addr == self.host_endpoint:
            for ep in list(self.client_endpoints.values()):
                if ep != addr:
                    try:
                        self.transport.sendto(data, ep)
                    except Exception:
                        pass
            return
        for idx, ep in list(self.client_endpoints.items()):
            if ep == addr and self.host_endpoint:
                try:
                    self.transport.sendto(data, self.host_endpoint)
                except Exception:
                    pass
                return


# ----------------------------------------------------------------------------
# Main

async def main(args) -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    ssl_ctx = None
    if args.tls_cert:
        import ssl
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(args.tls_cert, args.tls_key)

    async def signaling_handler(reader, writer):
        await SignalingConn(reader, writer).run()

    server = await asyncio.start_server(signaling_handler, host=args.host, port=args.port, ssl=ssl_ctx)
    sockname = server.sockets[0].getsockname()
    log.info("RemotePlay signaling server on %s:%s tls=%s", sockname[0], sockname[1], ssl_ctx is not None)

    stop = asyncio.Event()
    loop = asyncio.get_event_loop()

    def request_stop(*_):
        stop.set()

    if sys.platform != "win32":
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, request_stop)
            except NotImplementedError:
                pass

    async def expiry_loop():
        while True:
            await asyncio.sleep(300)
            REGISTRY.expire_old()

    asyncio.create_task(expiry_loop())
    async with server:
        await stop.wait()
    log.info("shutdown complete")


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="RemotePlay signaling server")
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=int(os.environ.get("REMOTEPLAY_PORT", "9000")))
    p.add_argument("--tls-cert", default=None)
    p.add_argument("--tls-key", default=None)
    return p.parse_args(argv)


if __name__ == "__main__":
    try:
        asyncio.run(main(parse_args()))
    except KeyboardInterrupt:
        pass
