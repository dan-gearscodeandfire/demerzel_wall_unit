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


class _FakeAudioChunk:
    def __init__(self, pcm_bytes: bytes):
        self.audio_int16_bytes = pcm_bytes


class _FakeVoice:
    """Minimal stand-in for piper.PiperVoice for tests. `synthesize(text)`
    yields one AudioChunk per entry in `sentences`."""
    class _Cfg:
        sample_rate = 16000

    def __init__(self, sentences):
        self._sentences = list(sentences)
        self.config = _FakeVoice._Cfg()

    def synthesize(self, text):
        for pcm in self._sentences:
            yield _FakeAudioChunk(pcm)


def _install_fake_voice(sentences):
    """Install a fake piper voice + remember originals for restore."""
    originals = {"_voice": voice_server._voice}
    voice_server._voice = _FakeVoice(sentences)
    return originals


def _restore_fake_voice(originals):
    voice_server._voice = originals["_voice"]


async def _stub_stream(unit_id, request_id, text, pcm_sink, *,
                        pcm=b"\x11\x22" * 8, streamed=True, sample_rate=16000):
    """Drop-in replacement for _stream_synthesis_to_ws that writes a canned
    blob into the sink and skips the real piper + ws paths."""
    pcm_sink.extend(pcm)
    return streamed, 3 if streamed else 0, sample_rate


async def _case_slow_background_pushes_pending_ready(port):
    """End-to-end: _slow_background's finally block must push pending_ready
    to the originating unit's WS after the synthesis completes."""
    url = f"http://127.0.0.1:{port}/ws"
    unit_id = "aa:bb:cc:00:00:04"

    orig_stage2 = voice_server._stage2_from_decision
    orig_stream = voice_server._stream_synthesis_to_ws
    voice_server._stage2_from_decision = lambda t, d: ("ok", {})
    voice_server._stream_synthesis_to_ws = _stub_stream

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
                _expect("pending_ready carries streamed flag",
                        evt is not None and evt.get("streamed") is True,
                        detail=f"evt={evt}")

                entry = voice_server._pending.get(request_id)
                _expect("pending entry still populated after push",
                        entry is not None and entry["event"].is_set())
                _expect("entry.wav populated with WAV container",
                        entry is not None and entry["wav"].startswith(b"RIFF"))
            finally:
                voice_server._pending.pop(request_id, None)
                voice_server._stage2_from_decision = orig_stage2
                voice_server._stream_synthesis_to_ws = orig_stream


async def _case_slow_background_no_ws_client(port):
    """If unit_id is set but there's no live WS, _slow_background must NOT
    raise — pending_ready push is best-effort only, and the tee-to-WAV path
    still delivers `wav` for the HTTP /voice_result fallback."""
    orig_stage2 = voice_server._stage2_from_decision
    orig_stream = voice_server._stream_synthesis_to_ws
    voice_server._stage2_from_decision = lambda t, d: ("ok", {})

    async def _offline_stream(unit_id, request_id, text, pcm_sink):
        # Simulate "unit not in _clients": no ws frames emitted, streamed=False,
        # but synthesis still produced PCM into the sink.
        pcm_sink.extend(b"\xaa\xbb" * 16)
        return False, 0, 16000

    voice_server._stream_synthesis_to_ws = _offline_stream

    request_id = "slowtest2"
    voice_server._pending[request_id] = {
        "event": asyncio.Event(),
        "wav": None, "reply_text": None, "debug": None, "error": None,
        "created": 0.0,
        "unit_id": "ff:ff:ff:ff:ff:00",   # not connected
    }
    try:
        await voice_server._slow_background(request_id, "x", {"tool": "t"})
        entry = voice_server._pending[request_id]
        _expect("no-ws path is a no-op (no exception)", True)
        _expect("event still set so /voice_result can return",
                entry["event"].is_set())
        _expect("entry.wav still populated despite offline unit",
                entry["wav"].startswith(b"RIFF"))
        _expect("streamed_over_ws flag is False",
                entry.get("streamed_over_ws") is False)
    except Exception as e:
        _expect("no-ws path is a no-op (no exception)", False,
                detail=f"raised {e!r}")
    finally:
        voice_server._pending.pop(request_id, None)
        voice_server._stage2_from_decision = orig_stage2
        voice_server._stream_synthesis_to_ws = orig_stream


async def _case_stream_synthesis_delivers_chunks(port):
    """Real _stream_synthesis_to_ws with a fake PiperVoice must emit
    tts_start, one tts_chunk per TTS_CHUNK_BYTES of PCM, and a final tts_end
    with the correct total_seq."""
    import base64 as _b64
    url = f"http://127.0.0.1:{port}/ws"
    unit_id = "aa:bb:cc:00:00:05"

    # Two sentences: first = 2.5 sub-chunks worth of PCM, second = 0.5.
    # Total expected sub-chunks = 3 + 1 = 4.
    CHUNK = voice_server.TTS_CHUNK_BYTES
    s1 = bytes(range(256)) * (CHUNK * 3 // 256 - 10)  # just under 3 chunks
    s1 = (b"\x01" * (CHUNK * 2 + CHUNK // 2))         # exactly 2.5 chunks
    s2 = (b"\x02" * (CHUNK // 2))                     # 0.5 chunks
    # sub-chunks emitted: ceil(2.5)=3 from s1, ceil(0.5)=1 from s2 → 4 total
    expected_seq = 4

    originals = _install_fake_voice([s1, s2])

    async with aiohttp.ClientSession() as sess:
        async with sess.ws_connect(url) as ws:
            await ws.send_json({"type": "hello", "unit_id": unit_id, "room": "den"})
            await _drain_until(ws, "hello_ack", timeout=2.0)

            try:
                pcm_sink = bytearray()
                streamed, seq, sr = await voice_server._stream_synthesis_to_ws(
                    unit_id, "streamtest1", "two sentences here.", pcm_sink)

                _expect("_stream_synthesis_to_ws returned streamed=True", streamed)
                _expect("total_seq matches expected sub-chunks",
                        seq == expected_seq, detail=f"seq={seq} expected={expected_seq}")
                _expect("sample_rate forwarded from voice config", sr == 16000)
                _expect("pcm_sink contains all sentence bytes",
                        bytes(pcm_sink) == s1 + s2,
                        detail=f"got {len(pcm_sink)} bytes, expected {len(s1)+len(s2)}")

                start = await _drain_until(ws, "tts_start", timeout=2.0)
                _expect("tts_start delivered",
                        start is not None
                        and start.get("request_id") == "streamtest1"
                        and start.get("sample_rate") == 16000
                        and start.get("channels") == 1,
                        detail=f"start={start}")

                chunks = []
                for _ in range(expected_seq):
                    c = await _drain_until(ws, "tts_chunk", timeout=2.0)
                    if c is None:
                        break
                    chunks.append(c)

                _expect(f"received {expected_seq} tts_chunk frames",
                        len(chunks) == expected_seq,
                        detail=f"got {len(chunks)}")

                seqs = [c.get("seq") for c in chunks]
                _expect("seq numbers are 0..N-1 contiguous",
                        seqs == list(range(expected_seq)),
                        detail=f"seqs={seqs}")

                reconstructed = b"".join(
                    _b64.b64decode(c.get("payload", "")) for c in chunks)
                _expect("payload bytes round-trip to original PCM",
                        reconstructed == s1 + s2,
                        detail=f"reconstructed {len(reconstructed)} vs expected {len(s1)+len(s2)}")

                end = await _drain_until(ws, "tts_end", timeout=2.0)
                _expect("tts_end delivered with correct total_seq",
                        end is not None
                        and end.get("request_id") == "streamtest1"
                        and end.get("total_seq") == expected_seq,
                        detail=f"end={end}")
            finally:
                _restore_fake_voice(originals)


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
        await _case_stream_synthesis_delivers_chunks(port)
    finally:
        await runner.cleanup()

    passed = sum(_results)
    failed = len(_results) - passed
    print()
    print(f"{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
