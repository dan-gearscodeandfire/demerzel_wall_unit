"""Integration tests for the /ws control channel.

Spins up voice_server in-process on a random localhost port, exercises the
handshake + event plumbing + notify_unit push from a real aiohttp client.
No whisper / piper / router / HA dependency — the ws_handler doesn't touch
those paths.

Run on okDemerzel (where aiohttp is installed):

    cd ~/demerzel/voice-server && source .venv/bin/activate && \
        python test_ws.py

The test binds to 127.0.0.1 on an OS-assigned port, so it's safe to run
while the production systemd service is listening on :8900.
"""
import asyncio
import json
import os
import socket
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Stub the heavy modules — ws_handler doesn't use any of them.
for name in ("requests", "tools", "router", "composer"):
    if name not in sys.modules:
        sys.modules[name] = types.SimpleNamespace(
            dispatch=lambda *a, **k: "",
            route=lambda *a, **k: {},
            compose=lambda *a, **k: "",
        )

import aiohttp
from aiohttp import web

import voice_server  # noqa: E402


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


async def _start_server(port: int):
    app = web.Application()
    app.router.add_get("/ws", voice_server.ws_handler)
    app.router.add_get("/health", voice_server.health)
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", port)
    await site.start()
    return runner


_results = []


def _expect(label, cond, detail=""):
    ok = bool(cond)
    mark = "OK  " if ok else "FAIL"
    msg = f"  [{mark}] {label}"
    if detail:
        msg += f"   {detail}"
    print(msg)
    _results.append(ok)


async def _drain_until(ws, type_, timeout=2.0):
    """Read frames until we see one with the expected type. Returns that
    frame's parsed payload, or None on timeout."""
    loop = asyncio.get_event_loop()
    deadline = loop.time() + timeout
    while True:
        remaining = deadline - loop.time()
        if remaining <= 0:
            return None
        try:
            msg = await asyncio.wait_for(ws.receive(), timeout=remaining)
        except asyncio.TimeoutError:
            return None
        if msg.type == aiohttp.WSMsgType.TEXT:
            data = json.loads(msg.data)
            if data.get("type") == type_:
                return data
        elif msg.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED,
                          aiohttp.WSMsgType.ERROR):
            return None


async def _case_hello_handshake(port):
    url = f"http://127.0.0.1:{port}/ws"
    async with aiohttp.ClientSession() as sess:
        async with sess.ws_connect(url) as ws:
            await ws.send_json({
                "type": "hello",
                "unit_id": "aa:bb:cc:00:00:01",
                "room": "den",
                "mic_id": "inmp441-left",
                "fw_version": "ws-test-1",
                "caps": ["voice_turn", "ws"],
            })
            ack = await _drain_until(ws, "hello_ack", timeout=2.0)
            _expect("hello → hello_ack", ack is not None)
            _expect(
                "unit registered in _clients",
                "aa:bb:cc:00:00:01" in voice_server._clients,
                detail=f"keys={sorted(voice_server._clients.keys())}",
            )
            await ws.close()

    # Give the server loop a tick to process the close before asserting.
    await asyncio.sleep(0.05)
    _expect(
        "unit unregistered on close",
        "aa:bb:cc:00:00:01" not in voice_server._clients,
        detail=f"keys={sorted(voice_server._clients.keys())}",
    )


async def _case_hello_missing_unit_id(port):
    url = f"http://127.0.0.1:{port}/ws"
    async with aiohttp.ClientSession() as sess:
        async with sess.ws_connect(url) as ws:
            await ws.send_json({"type": "hello"})  # no unit_id
            # Server should close with 1008. Read until close.
            closed = False
            for _ in range(5):
                try:
                    msg = await asyncio.wait_for(ws.receive(), timeout=1.0)
                except asyncio.TimeoutError:
                    break
                if msg.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED):
                    closed = True
                    break
            _expect("hello without unit_id → close", closed)


async def _case_reconnect_supersedes(port):
    url = f"http://127.0.0.1:{port}/ws"
    unit_id = "aa:bb:cc:00:00:02"
    async with aiohttp.ClientSession() as s1, aiohttp.ClientSession() as s2:
        ws1 = await s1.ws_connect(url)
        await ws1.send_json({"type": "hello", "unit_id": unit_id, "room": "den"})
        await _drain_until(ws1, "hello_ack", timeout=2.0)
        _expect("ws1 registered", voice_server._clients.get(unit_id) is not None)

        # Second connection for the SAME unit_id should supersede ws1.
        ws2 = await s2.ws_connect(url)
        await ws2.send_json({"type": "hello", "unit_id": unit_id, "room": "den"})
        await _drain_until(ws2, "hello_ack", timeout=2.0)

        # ws1 should now be closed by the server.
        closed = False
        for _ in range(10):
            try:
                msg = await asyncio.wait_for(ws1.receive(), timeout=0.5)
            except asyncio.TimeoutError:
                break
            if msg.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED):
                closed = True
                break
        _expect("previous connection superseded", closed)

        # Functional check: notify_unit should reach ws2 (not the closed ws1).
        # Registry contents are an implementation detail; delivery is what matters.
        delivered = await voice_server.notify_unit(
            unit_id, "pending_ready", request_id="abc123")
        _expect("notify_unit returned True", delivered)
        evt = await _drain_until(ws2, "pending_ready", timeout=2.0)
        _expect("pending_ready delivered to ws2",
                evt is not None and evt.get("request_id") == "abc123",
                detail=f"evt={evt}")

        await ws2.close()


async def _case_notify_unknown_unit(port):
    delivered = await voice_server.notify_unit(
        "ff:ff:ff:ff:ff:ff", "pending_ready", request_id="nope")
    _expect("notify_unit for unknown unit → False", delivered is False)


async def _case_slow_background_pushes_pending_ready(port):
    """End-to-end: _slow_background's finally block must push pending_ready
    to the originating unit's WS after the synthesis completes."""
    url = f"http://127.0.0.1:{port}/ws"
    unit_id = "aa:bb:cc:00:00:04"

    # Patch the heavy bits so we can run the background task inline.
    orig_stage2 = voice_server._stage2_from_decision
    orig_synth = voice_server.synthesize
    voice_server._stage2_from_decision = lambda t, d: ("ok", {})
    voice_server.synthesize = lambda text: b"RIFF\x00\x00\x00\x00WAVEfake"

    async with aiohttp.ClientSession() as sess:
        async with sess.ws_connect(url) as ws:
            await ws.send_json({"type": "hello", "unit_id": unit_id, "room": "den"})
            await _drain_until(ws, "hello_ack", timeout=2.0)

            request_id = "slowtest1"
            voice_server._pending[request_id] = {
                "event": asyncio.Event(),
                "wav": None, "reply_text": None, "debug": None, "error": None,
                "created": 0.0,
                "unit_id": unit_id,
            }
            try:
                await voice_server._slow_background(
                    request_id, "turn off dan's office lights",
                    {"intent": "ha_command", "tool": "ha_turn_off",
                     "latency_class": "slow", "verbal_response": "acknowledge_first",
                     "params": {}})

                evt = await _drain_until(ws, "pending_ready", timeout=2.0)
                _expect("_slow_background pushed pending_ready",
                        evt is not None and evt.get("request_id") == request_id,
                        detail=f"evt={evt}")

                entry = voice_server._pending.get(request_id)
                _expect("pending entry still populated after push",
                        entry is not None and entry["event"].is_set())
            finally:
                voice_server._pending.pop(request_id, None)
                voice_server._stage2_from_decision = orig_stage2
                voice_server.synthesize = orig_synth


async def _case_slow_background_no_ws_client(port):
    """If unit_id is set but there's no live WS, _slow_background must NOT
    raise — pending_ready push is best-effort only."""
    orig_stage2 = voice_server._stage2_from_decision
    orig_synth = voice_server.synthesize
    voice_server._stage2_from_decision = lambda t, d: ("ok", {})
    voice_server.synthesize = lambda text: b"RIFF"

    request_id = "slowtest2"
    voice_server._pending[request_id] = {
        "event": asyncio.Event(),
        "wav": None, "reply_text": None, "debug": None, "error": None,
        "created": 0.0,
        "unit_id": "ff:ff:ff:ff:ff:00",   # not connected
    }
    try:
        await voice_server._slow_background(request_id, "x", {"tool": "t"})
        _expect("no-ws push is a no-op (no exception)", True)
        _expect("event still set so /voice_result can return",
                voice_server._pending[request_id]["event"].is_set())
    except Exception as e:
        _expect("no-ws push is a no-op (no exception)", False,
                detail=f"raised {e!r}")
    finally:
        voice_server._pending.pop(request_id, None)
        voice_server._stage2_from_decision = orig_stage2
        voice_server.synthesize = orig_synth


async def _case_events_accepted(port):
    url = f"http://127.0.0.1:{port}/ws"
    unit_id = "aa:bb:cc:00:00:03"
    async with aiohttp.ClientSession() as sess:
        async with sess.ws_connect(url) as ws:
            await ws.send_json({"type": "hello", "unit_id": unit_id, "room": "den"})
            await _drain_until(ws, "hello_ack", timeout=2.0)
            # Server accepts these without closing; no ack expected. We just
            # want to confirm the connection stays open after sending them.
            for evt in [
                {"type": "state", "state": "recording", "turn_id": "t1"},
                {"type": "wake", "score_peak": 231, "ts_us": 123456},
                {"type": "env", "temp_c": 22.1, "humidity": 41.0, "pressure_hpa": 1013.2},
                {"type": "presence", "pir": True, "radar": False},
                {"type": "bogus_type", "foo": "bar"},
            ]:
                await ws.send_json(evt)
            # Round-trip a notify to confirm the ws is still live.
            await voice_server.notify_unit(unit_id, "pending_ready", request_id="x")
            got = await _drain_until(ws, "pending_ready", timeout=2.0)
            _expect("connection survives assorted events (including unknown)",
                    got is not None and got.get("request_id") == "x")


async def main():
    port = _free_port()
    runner = await _start_server(port)
    try:
        print(f"== /ws integration tests (port {port}) ==")
        await _case_hello_handshake(port)
        await _case_hello_missing_unit_id(port)
        await _case_reconnect_supersedes(port)
        await _case_notify_unknown_unit(port)
        await _case_events_accepted(port)
        await _case_slow_background_pushes_pending_ready(port)
        await _case_slow_background_no_ws_client(port)
    finally:
        await runner.cleanup()

    passed = sum(_results)
    failed = len(_results) - passed
    print()
    print(f"{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
