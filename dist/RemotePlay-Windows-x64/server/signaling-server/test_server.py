#!/usr/bin/env python3
"""Integration test for the signaling + relay server (run on any machine).

Covers: host_register, join, public-address reporting, TCP relay pairing
with opaque byte piping, UDP relay binding + forwarding, player slots.
"""

from __future__ import annotations

import asyncio
import json
import sys

sys.path.insert(0, ".")

import server as signaling  # noqa: E402

HOST = "127.0.0.1"
PORT = 9410


async def read_json(reader: asyncio.StreamReader) -> dict:
    line = await asyncio.wait_for(reader.readline(), timeout=5)
    return json.loads(line)


async def write_json(writer: asyncio.StreamWriter, obj: dict) -> None:
    writer.write((json.dumps(obj) + "\n").encode())
    await writer.drain()


async def test_full_flow():
    # -- start server in-process --
    server_task = asyncio.create_task(
        signaling.main(signaling.parse_args(["--host", HOST, "--port", str(PORT)])))
    await asyncio.sleep(0.3)

    host_r, host_w = await asyncio.open_connection(HOST, PORT)
    client_r, client_w = await asyncio.open_connection(HOST, PORT)

    # 1. host register
    await write_json(host_w, {"type": "host_register", "name": "TestHost"})
    reply = await read_json(host_r)
    assert reply["type"] == "host_registered", reply
    code = reply["code"]
    host_token = reply["host_token"]
    relay_tcp = reply["relay_tcp_port"]
    relay_udp = reply["relay_udp_port"]
    print(f"[ok] host registered: code={code} tcp={relay_tcp} udp={relay_udp}")

    # 2. join with a bad code
    await write_json(client_w, {"type": "join", "code": "XXXXXXXX", "name": "P"})
    reply = await read_json(client_r)
    assert reply["type"] == "join_rejected", reply
    print("[ok] bad code rejected")

    # 3. join properly
    await write_json(client_w, {"type": "join", "code": code, "name": "Mahdyar"})
    reply = await read_json(client_r)
    assert reply["type"] == "joined", reply
    assert reply["player_index"] == 0
    client_token = reply["token"]
    assert reply["host_public"]["addr"] == "127.0.0.1"
    print(f"[ok] client joined as player 1, token={client_token[:8]}...")

    # host is told about the client (with public address)
    notice = await read_json(host_r)
    assert notice["type"] == "client_joined", notice
    assert notice["player_index"] == 0
    assert notice["client_public"]["addr"] == "127.0.0.1"
    print("[ok] host notified of client + public address (STUN-equivalent)")

    # 4. TCP relay pairing + opaque byte pipe
    # register the pair (server does this at join time via register_pair)
    signaling.register_pair(client_token, signaling.REGISTRY.lookup_code(code), 0)

    hrel_r, hrel_w = await asyncio.open_connection(HOST, relay_tcp)
    crel_r, crel_w = await asyncio.open_connection(HOST, relay_tcp)
    await write_json(hrel_w, {"type": "relay", "token": client_token, "role": "host"})
    await write_json(crel_w, {"type": "relay", "token": client_token, "role": "client"})
    assert (await read_json(hrel_r))["type"] == "relay_accepted"
    assert (await read_json(crel_r))["type"] == "relay_accepted"
    # both sides get relay_paired once the pair completes
    hpaired = (await read_json(hrel_r))["type"]
    cpaired = (await read_json(crel_r))["type"]
    assert hpaired == "relay_paired" and cpaired == "relay_paired", (hpaired, cpaired)
    print("[ok] relay TCP pair established")

    # opaque bytes both directions (simulates the RemotePlay session protocol)
    hrel_w.write(b"HOST-HELLO-abc")
    await hrel_w.drain()
    data = await asyncio.wait_for(crel_r.readexactly(14), timeout=5)
    assert data == b"HOST-HELLO-abc", data
    crel_w.write(b"CLIENT-REPLY-xy")
    await crel_w.drain()
    data = await asyncio.wait_for(hrel_r.readexactly(15), timeout=5)
    assert data == b"CLIENT-REPLY-xy", data
    print("[ok] opaque relay bytes flow both ways")

    # 5. UDP relay bind + forwarding
    class Probe(asyncio.DatagramProtocol):
        def __init__(self):
            self.got = asyncio.Queue()

        def connection_made(self, transport):
            self.transport = transport

        def datagram_received(self, data, addr):
            self.got.put_nowait((data, addr))

    loop = asyncio.get_event_loop()
    htr, hproto = await loop.create_datagram_endpoint(Probe, local_addr=("127.0.0.1", 0))
    ctr, cproto = await loop.create_datagram_endpoint(Probe, local_addr=("127.0.0.1", 0))

    # bind both (RPBIND + 32-hex token)
    htr.sendto(b"RPBIND" + host_token.encode(), (HOST, relay_udp))
    ctr.sendto(b"RPBIND" + client_token.encode(), (HOST, relay_udp))
    ack1, _ = await asyncio.wait_for(hproto.got.get(), timeout=5)
    ack2, _ = await asyncio.wait_for(cproto.got.get(), timeout=5)
    assert ack1 == b"RPBINDOK" and ack2 == b"RPBINDOK"
    print("[ok] UDP relay endpoints bound")

    # host -> client datagram (opaque)
    htr.sendto(b"\x52\x52\x01\x00fake-encrypted-payload", (HOST, relay_udp))
    data, _ = await asyncio.wait_for(cproto.got.get(), timeout=5)
    assert data == b"\x52\x52\x01\x00fake-encrypted-payload", data
    # client -> host datagram
    ctr.sendto(b"\x52\x52\x01\x01client-uplink", (HOST, relay_udp))
    data, _ = await asyncio.wait_for(hproto.got.get(), timeout=5)
    assert data == b"\x52\x52\x01\x01client-uplink", data
    print("[ok] UDP media forwarded both directions (opaque)")

    # 6. teardown
    await write_json(client_w, {"type": "leave"})
    await write_json(host_w, {"type": "leave"})
    for w in (host_w, client_w, hrel_w, crel_w):
        w.close()
    htr.close()
    ctr.close()
    print("[ok] clean teardown")

    server_task.cancel()
    try:
        await server_task
    except asyncio.CancelledError:
        pass
    print("\nALL SERVER TESTS PASSED")


if __name__ == "__main__":
    asyncio.run(test_full_flow())
