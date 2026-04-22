"""demerzel-voice-server — Stage 1 vertical slice.

One HTTP endpoint that takes a recorded WAV from a wall unit, transcribes it
with whisper.cpp, composes a hardcoded response, synthesizes with Piper, and
returns the response as a WAV in the HTTP body.

Runs on okDemerzel. Listens on :8900.

POST /voice_turn
    Content-Type: audio/wav  (raw body is the WAV file)
    Response: audio/wav  (Piper-synthesized reply)
    Headers:
        X-Transcript:    the STT transcription
        X-Reply-Text:    the text that was spoken back
        X-Latency-Ms:    total server-side wall-clock latency

GET /health
    Returns {"status": "ok", ...}
"""
import asyncio
import base64
import io
import json
import logging
import os
import re
import time
import uuid
import wave

import requests
from aiohttp import web
from piper import PiperVoice

import router as router_mod
import tools as tools_mod
import composer as composer_mod

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("voice-server")

HERE = os.path.dirname(os.path.abspath(__file__))
PIPER_BIN = os.path.join(HERE, ".venv", "bin", "piper")   # legacy; unused now
PIPER_VOICE = os.path.join(HERE, "voices", "en_US-lessac-low.onnx")
WHISPER_URL = "http://127.0.0.1:8891/inference"
LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 8900

# Streaming TTS: chunk granularity for the WS `tts_chunk` frames.
# 40 ms @ 16 kHz int16 mono = 640 samples = 1280 bytes. Small enough for a
# responsive jitter buffer on the firmware side, large enough that the per-
# frame JSON+base64 overhead stays reasonable.
TTS_CHUNK_SAMPLES = 640
TTS_CHUNK_BYTES = TTS_CHUNK_SAMPLES * 2

# Loaded once at startup by _load_voice(). Piper's synthesize() is thread-safe
# for read access; single global instance is fine.
_voice: PiperVoice | None = None


def transcribe(wav_bytes: bytes) -> str:
    """POST a WAV to whisper.cpp and return the transcription text."""
    files = {"file": ("audio.wav", wav_bytes, "audio/wav")}
    data = {"temperature": "0.0", "response_format": "json"}
    r = requests.post(WHISPER_URL, files=files, data=data, timeout=30)
    r.raise_for_status()
    return r.json().get("text", "").strip()


def _load_voice() -> PiperVoice:
    """Load the Piper voice model once at startup. Idempotent."""
    global _voice
    if _voice is None:
        t0 = time.monotonic()
        _voice = PiperVoice.load(PIPER_VOICE)
        log.info("piper voice loaded: %s (sample_rate=%d, %.0f ms)",
                 os.path.basename(PIPER_VOICE),
                 _voice.config.sample_rate,
                 (time.monotonic() - t0) * 1000)
    return _voice


def _wrap_wav(pcm_bytes: bytes, sample_rate: int, channels: int = 1) -> bytes:
    """Wrap raw int16 PCM into a complete WAV container."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)  # int16
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    return buf.getvalue()


def synthesize(text: str) -> bytes:
    """Synthesize text via the Piper library and return complete WAV bytes."""
    v = _load_voice()
    pcm = b"".join(c.audio_int16_bytes for c in v.synthesize(text))
    return _wrap_wav(pcm, v.config.sample_rate)


_WHISPER_ANNOT_RE = re.compile(r"^\s*(?:[\[\(][^\]\)]*[\]\)]\s*)+$")

def _is_routable_transcript(text: str) -> bool:
    """Filter whisper output that isn't actually speech.

    Whisper emits non-speech annotations for ambient sounds: `[BLANK_AUDIO]`,
    `[SOUND]`, `[MUSIC]`, `(water running)`, `(clicking)`, etc. These can
    happen any time the DWU records a turn with no real speech (e.g. false
    wake-word trigger near running water or the TV). Without filtering, the
    router happily hallucinates an intent from the annotation text and we
    end up physically speaking a fabricated response.

    Also reject transcripts with no alphabetic content (whisper occasionally
    returns digit-dot sequences like "( ( 2. 3. 4." for non-speech audio).
    """
    if not text:
        return False
    if _WHISPER_ANNOT_RE.match(text):
        return False
    if not any(c.isalpha() for c in text):
        return False
    return True


# --- Two-phase TTS: pending result store ---
_pending: dict[str, dict] = {}
PENDING_TTL = 120      # auto-cleanup stale entries after this many seconds
PENDING_TIMEOUT = 30   # max seconds firmware will long-poll on /voice_result


# --- WebSocket control channel: unit registry ---
# Keyed by unit_id (wall-unit MAC, lowercase, colon-separated).
# Value is the aiohttp WebSocketResponse. One ws per unit at a time; a
# reconnect supersedes the previous entry.
_clients: dict[str, web.WebSocketResponse] = {}
_clients_lock = asyncio.Lock()
WS_HEARTBEAT = 30   # aiohttp autoping interval, seconds


async def notify_unit(unit_id: str, event_type: str, **payload) -> bool:
    """Push a server→unit event. Returns True if delivered, False if unit
    isn't connected or the send failed. Callers should treat WS as
    best-effort — nothing in the audio path depends on it."""
    ws = _clients.get(unit_id)
    if ws is None or ws.closed:
        return False
    try:
        await ws.send_json({"type": event_type, **payload})
        return True
    except Exception as e:
        log.warning("notify_unit(%s, %s) failed: %s", unit_id, event_type, e)
        return False


def _stage2_from_decision(transcript: str, decision: dict) -> tuple[str, dict]:
    """Like stage2_pipeline but skips the router — decision already known."""
    debug = {"router": decision}
    t1 = time.monotonic()
    tool_result = tools_mod.dispatch(decision["tool"], decision.get("params", {}))
    debug["tool_ms"] = int((time.monotonic() - t1) * 1000)
    debug["tool_result"] = tool_result[:500] if isinstance(tool_result, str) else str(tool_result)[:500]
    t2 = time.monotonic()
    spoken = composer_mod.compose(decision, tool_result, transcript)
    debug["compose_ms"] = int((time.monotonic() - t2) * 1000)
    return spoken, debug


async def _stream_synthesis_to_ws(
    unit_id: str,
    request_id: str,
    text: str,
    pcm_sink: bytearray,
) -> tuple[bool, int, int]:
    """Synthesize `text` one sentence at a time (piper yields AudioChunk per
    sentence). Stream each sentence's PCM to the unit in TTS_CHUNK_BYTES-sized
    sub-chunks over WS, and append every byte to `pcm_sink` so the caller can
    still materialize a WAV for the HTTP fallback.

    Returns (streamed_ok, total_seq, sample_rate). If the unit isn't
    connected or the WS closes mid-stream, returns (False, seq_at_failure, sr)
    and the caller falls back to the pending_ready + /voice_result path.
    Synthesis still completes into `pcm_sink` either way.
    """
    loop = asyncio.get_event_loop()
    v = _load_voice()
    sample_rate = v.config.sample_rate

    ws = _clients.get(unit_id)
    ws_ok = ws is not None and not ws.closed
    if ws_ok:
        try:
            await ws.send_json({
                "type": "tts_start",
                "request_id": request_id,
                "sample_rate": sample_rate,
                "channels": 1,
            })
        except Exception as e:
            log.warning("tts_start send failed (unit=%s): %s", unit_id, e)
            ws_ok = False

    # Producer: run piper in a thread, push AudioChunks into an asyncio queue.
    # Sentinel None = end of stream.
    queue: asyncio.Queue = asyncio.Queue()

    def producer():
        try:
            for chunk in v.synthesize(text):
                asyncio.run_coroutine_threadsafe(queue.put(chunk), loop).result()
        except Exception as e:
            log.exception("piper producer failed: %s", e)
            asyncio.run_coroutine_threadsafe(queue.put(e), loop).result()
        finally:
            asyncio.run_coroutine_threadsafe(queue.put(None), loop).result()

    producer_future = loop.run_in_executor(None, producer)

    seq = 0
    try:
        while True:
            item = await queue.get()
            if item is None:
                break
            if isinstance(item, Exception):
                raise item
            pcm = item.audio_int16_bytes
            pcm_sink.extend(pcm)
            if not ws_ok:
                continue
            for i in range(0, len(pcm), TTS_CHUNK_BYTES):
                sub = pcm[i:i + TTS_CHUNK_BYTES]
                if ws.closed:
                    ws_ok = False
                    break
                try:
                    await ws.send_json({
                        "type": "tts_chunk",
                        "request_id": request_id,
                        "seq": seq,
                        "payload": base64.b64encode(sub).decode("ascii"),
                    })
                    seq += 1
                except Exception as e:
                    log.warning("tts_chunk send failed at seq=%d: %s", seq, e)
                    ws_ok = False
                    break
    finally:
        # Ensure the producer finishes even on failure, so we don't leak threads.
        try:
            await producer_future
        except Exception:
            pass

    if ws_ok and not ws.closed:
        try:
            await ws.send_json({
                "type": "tts_end",
                "request_id": request_id,
                "total_seq": seq,
            })
        except Exception as e:
            log.warning("tts_end send failed: %s", e)
            ws_ok = False

    return ws_ok, seq, sample_rate


async def _slow_background(request_id: str, transcript: str, decision: dict):
    """Run tool dispatch + compose + synthesize in background. Streams TTS
    PCM to the unit over WS as each sentence finishes (if the unit is on the
    control channel), and also accumulates the full PCM into a WAV stored in
    `_pending[request_id].wav` for the HTTP `/voice_result` fallback path."""
    entry = _pending[request_id]
    unit_id = entry.get("unit_id")
    loop = asyncio.get_event_loop()
    try:
        reply, debug = await loop.run_in_executor(
            None, _stage2_from_decision, transcript, decision
        )
        entry["reply_text"] = reply
        entry["debug"] = debug

        pcm_accumulator = bytearray()
        if unit_id:
            t_synth = time.monotonic()
            streamed, total_seq, sample_rate = await _stream_synthesis_to_ws(
                unit_id, request_id, reply, pcm_accumulator,
            )
            synth_ms = int((time.monotonic() - t_synth) * 1000)
            entry["streamed_over_ws"] = streamed
            log.info("slow synth+stream (unit=%s, req=%s): streamed=%s seq=%d %dms",
                     unit_id, request_id, streamed, total_seq, synth_ms)
        else:
            # No unit to stream to — synthesize inline, WAV for HTTP fallback only.
            v = _load_voice()
            sample_rate = v.config.sample_rate
            for chunk in v.synthesize(reply):
                pcm_accumulator.extend(chunk.audio_int16_bytes)
            entry["streamed_over_ws"] = False

        entry["wav"] = _wrap_wav(bytes(pcm_accumulator), sample_rate)
    except Exception as e:
        log.exception("background task failed for %s", request_id)
        fallback = "Sorry, something went wrong."
        try:
            entry["wav"] = await loop.run_in_executor(None, synthesize, fallback)
        except Exception:
            entry["wav"] = b""
        entry["reply_text"] = fallback
        entry["error"] = str(e)
        entry["streamed_over_ws"] = False
    finally:
        entry["event"].set()
        # Always emit pending_ready as a safety net. Firmware that already
        # received tts_end ignores it; firmware that missed the stream uses it
        # to short-circuit the /voice_result long-poll.
        if unit_id:
            await notify_unit(unit_id, "pending_ready", request_id=request_id,
                              streamed=entry.get("streamed_over_ws", False))


def stage2_pipeline(transcript: str) -> tuple[str, dict]:
    """Stage 2: route → dispatch → compose. Returns (spoken_text, debug_info)."""
    debug = {}
    if not _is_routable_transcript(transcript):
        log.info("dropping non-speech transcript: %r", transcript)
        return "I did not catch that.", {"reason": "non_speech", "transcript": transcript}

    t0 = time.monotonic()
    decision = router_mod.route(transcript)
    debug["router_ms"] = int((time.monotonic() - t0) * 1000)
    debug["router"] = decision
    log.info("router decision: %s", json.dumps({k: v for k, v in decision.items() if k != "params"}))

    t1 = time.monotonic()
    tool_result = tools_mod.dispatch(decision["tool"], decision.get("params", {}))
    debug["tool_ms"] = int((time.monotonic() - t1) * 1000)
    debug["tool_result"] = tool_result[:500] if isinstance(tool_result, str) else str(tool_result)[:500]

    t2 = time.monotonic()
    spoken = composer_mod.compose(decision, tool_result, transcript)
    debug["compose_ms"] = int((time.monotonic() - t2) * 1000)

    return spoken, debug


def _hdr_safe(s: str) -> str:
    return " ".join(s.split())[:500]


async def voice_turn(request: web.Request) -> web.Response:
    t0 = time.monotonic()
    wav_in = await request.read()
    unit_id = (request.headers.get("X-DWU-Unit-Id") or "").strip().lower() or None
    log.info("received %d bytes of audio (unit=%s)", len(wav_in), unit_id)

    try:
        transcript = transcribe(wav_in)
    except Exception as e:
        log.exception("whisper failed")
        return web.json_response({"error": "whisper_failed", "detail": str(e)}, status=500)
    log.info("transcript: %r", transcript)

    # Filter non-speech. Return 204 so the firmware stays silent — playing
    # "I did not catch that" on every spurious wake is louder than the
    # original misfire. The X-* headers are still set for debugging.
    if not _is_routable_transcript(transcript):
        log.info("dropping non-speech transcript: %r (returning 204)", transcript)
        return web.Response(
            status=204,
            headers={
                "X-Transcript": _hdr_safe(transcript),
                "X-Dropped-Reason": "non_routable_transcript",
                "X-Latency-Ms": str(int((time.monotonic() - t0) * 1000)),
            },
        )

    # Route (fast: ~100-300 ms)
    t_route = time.monotonic()
    try:
        decision = router_mod.route(transcript)
    except Exception as e:
        log.exception("router failed")
        decision = {"intent": "chat", "latency_class": "quick",
                     "verbal_response": "answer_only", "tool": "llm_chat",
                     "params": {"prompt": transcript}, "confidence": 0.0}
    route_ms = int((time.monotonic() - t_route) * 1000)
    log.info("router decision (%d ms): %s", route_ms,
             json.dumps({k: v for k, v in decision.items() if k != "params"}))

    # --- Two-phase: ack immediately for slow turns ---
    if (decision.get("latency_class") == "slow"
            and decision.get("verbal_response") == "acknowledge_first"):
        ack_text = decision.get("ack_phrase") or "One moment."
        log.info("two-phase: ack=%r, launching background task", ack_text)
        try:
            ack_wav = synthesize(ack_text)
        except Exception:
            log.exception("ack synth failed, falling back to single-phase")
            ack_wav = None

        if ack_wav:
            request_id = uuid.uuid4().hex[:12]
            _pending[request_id] = {
                "event": asyncio.Event(),
                "wav": None, "reply_text": None, "debug": None, "error": None,
                "created": time.monotonic(),
                "unit_id": unit_id,
            }
            asyncio.ensure_future(_slow_background(request_id, transcript, decision))

            elapsed_ms = int((time.monotonic() - t0) * 1000)
            return web.Response(
                body=ack_wav, content_type="audio/wav",
                headers={
                    "X-Transcript": _hdr_safe(transcript),
                    "X-Reply-Text": _hdr_safe(ack_text),
                    "X-Latency-Ms": str(elapsed_ms),
                    "X-DWU-Pending": request_id,
                    "X-Router-Intent": _hdr_safe(str(decision.get("intent", ""))),
                    "X-Router-Ms": str(route_ms),
                },
            )

    # --- Single-phase: dispatch + compose + synthesize inline ---
    try:
        reply, debug = _stage2_from_decision(transcript, decision)
        debug["router_ms"] = route_ms
    except Exception as e:
        log.exception("stage2 pipeline failed")
        reply = "Sorry, something went wrong."
        debug = {"error": str(e), "router_ms": route_ms}
    log.info("reply: %r", reply)

    try:
        wav_out = synthesize(reply)
    except Exception as e:
        log.exception("piper failed")
        return web.json_response({"error": "piper_failed", "detail": str(e)}, status=500)

    elapsed_ms = int((time.monotonic() - t0) * 1000)
    log.info("total %d ms, response %d bytes", elapsed_ms, len(wav_out))

    headers = {
        "X-Transcript": _hdr_safe(transcript),
        "X-Reply-Text": _hdr_safe(reply),
        "X-Latency-Ms": str(elapsed_ms),
    }
    if debug.get("router"):
        d = debug["router"]
        headers["X-Router-Model"] = _hdr_safe(str(d.get("_router_model", "")))
        headers["X-Router-Intent"] = _hdr_safe(str(d.get("intent", "")))
        headers["X-Router-Tool"] = _hdr_safe(str(d.get("tool", "")))
        headers["X-Router-Confidence"] = _hdr_safe(str(d.get("confidence", "")))
    if debug.get("router_ms") is not None:
        headers["X-Router-Ms"] = str(debug["router_ms"])
    if debug.get("tool_ms") is not None:
        headers["X-Tool-Ms"] = str(debug["tool_ms"])
    if debug.get("compose_ms") is not None:
        headers["X-Compose-Ms"] = str(debug["compose_ms"])

    return web.Response(body=wav_out, content_type="audio/wav", headers=headers)


async def voice_result(request: web.Request) -> web.Response:
    """Long-poll endpoint: firmware calls this to fetch the real answer after
    receiving an ack WAV with X-DWU-Pending."""
    request_id = request.match_info["request_id"]
    entry = _pending.get(request_id)
    if entry is None:
        return web.json_response({"error": "not_found"}, status=404)

    try:
        await asyncio.wait_for(entry["event"].wait(), timeout=PENDING_TIMEOUT)
    except asyncio.TimeoutError:
        _pending.pop(request_id, None)
        return web.json_response({"error": "timeout"}, status=504)

    wav = entry.get("wav", b"")
    reply_text = entry.get("reply_text", "")
    debug = entry.get("debug") or {}
    _pending.pop(request_id, None)

    headers = {"X-Reply-Text": _hdr_safe(reply_text)}
    if debug.get("tool_ms") is not None:
        headers["X-Tool-Ms"] = str(debug["tool_ms"])
    if debug.get("compose_ms") is not None:
        headers["X-Compose-Ms"] = str(debug["compose_ms"])

    return web.Response(body=wav, content_type="audio/wav", headers=headers)


async def voice_text(request: web.Request) -> web.Response:
    """Test endpoint: accept a text utterance directly, skip whisper. Returns JSON
    with the router decision, tool result, and composed reply (no TTS audio).
    Useful for fast iteration on Stage 2 routing logic."""
    t0 = time.monotonic()
    body = await request.json()
    text = body.get("text", "").strip()
    if not text:
        return web.json_response({"error": "missing text"}, status=400)
    log.info("voice_text input: %r", text)
    try:
        reply, debug = stage2_pipeline(text)
    except Exception as e:
        log.exception("stage2 pipeline failed")
        return web.json_response({"error": str(e)}, status=500)
    elapsed_ms = int((time.monotonic() - t0) * 1000)
    return web.json_response({
        "transcript": text,
        "reply": reply,
        "router": debug.get("router"),
        "tool_result": debug.get("tool_result"),
        "router_ms": debug.get("router_ms"),
        "tool_ms": debug.get("tool_ms"),
        "compose_ms": debug.get("compose_ms"),
        "total_ms": elapsed_ms,
    })


async def ws_handler(request: web.Request) -> web.WebSocketResponse:
    """Persistent control channel for wall units.

    Protocol (JSON text frames, one event per frame):

      unit→server:
        {"type": "hello",    "unit_id": "<mac>", "room": "...", "mic_id": "...",
                             "fw_version": "...", "caps": [...]}
        {"type": "wake",     "score_peak": N, "ts_us": N}
        {"type": "state",    "state": "idle|recording|uploading|playing|followup",
                             "turn_id": "optional"}
        {"type": "env",      "temp_c": ..., "humidity": ..., "pressure_hpa": ...}
        {"type": "presence", "pir": bool, "radar": bool}

      server→unit:
        {"type": "tts_start",     "request_id": "...", "sample_rate": 16000,
                                  "channels": 1}
        {"type": "tts_chunk",     "request_id": "...", "seq": N,
                                  "payload": "<base64 int16 PCM LE>"}
        {"type": "tts_end",       "request_id": "...", "total_seq": N}
        {"type": "pending_ready", "request_id": "...", "streamed": bool}
        {"type": "barge_in",      "reason": "..."}         # future: Stage 4
        {"type": "suppress",      "ms": N}                 # future: arbitration
        {"type": "config",        ...}                     # future

    TTS streaming: for slow-class turns, the background synthesizer pushes
    `tts_start`, a sequence of `tts_chunk` frames (one per TTS_CHUNK_BYTES of
    PCM), and a final `tts_end`. `pending_ready` is always emitted afterwards
    as a safety net — its `streamed` flag tells firmware whether it already
    received the full stream and can skip the `/voice_result` long-poll.

    Registration happens on the first `hello` frame — no unit_id, no entry in
    the registry. Keepalive is handled by aiohttp autoping (heartbeat=30 s).
    """
    ws = web.WebSocketResponse(heartbeat=WS_HEARTBEAT)
    await ws.prepare(request)

    unit_id: str | None = None
    peer = request.remote

    try:
        async for msg in ws:
            if msg.type != web.WSMsgType.TEXT:
                # binary frames not expected; close cleanly
                if msg.type == web.WSMsgType.ERROR:
                    log.warning("ws error from %s: %s", peer, ws.exception())
                continue

            try:
                evt = json.loads(msg.data)
            except json.JSONDecodeError:
                log.warning("ws %s: non-JSON frame, ignoring", peer)
                continue

            etype = evt.get("type")

            if etype == "hello":
                new_id = str(evt.get("unit_id") or "").strip().lower()
                if not new_id:
                    log.warning("ws %s: hello without unit_id, closing", peer)
                    await ws.close(code=1008, message=b"unit_id required")
                    return ws
                async with _clients_lock:
                    prev = _clients.get(new_id)
                    if prev is not None and not prev.closed:
                        log.info("ws %s: superseding previous connection for %s",
                                 peer, new_id)
                        try:
                            await prev.close(code=1000, message=b"superseded")
                        except Exception:
                            pass
                    _clients[new_id] = ws
                unit_id = new_id
                log.info("ws hello: unit=%s room=%s mic=%s fw=%s peer=%s",
                         unit_id, evt.get("room"), evt.get("mic_id"),
                         evt.get("fw_version"), peer)
                await ws.send_json({"type": "hello_ack"})
                continue

            # All other events require a prior hello.
            if unit_id is None:
                log.warning("ws %s: %r before hello, ignoring", peer, etype)
                continue

            if etype in ("wake", "state", "env", "presence"):
                log.info("ws %s %s %s", unit_id, etype,
                         {k: v for k, v in evt.items() if k != "type"})
            else:
                log.warning("ws %s: unknown event type %r", unit_id, etype)
    except asyncio.CancelledError:
        raise
    except Exception:
        log.exception("ws handler crashed (unit=%s peer=%s)", unit_id, peer)
    finally:
        if unit_id is not None:
            async with _clients_lock:
                # Only remove if we still own the slot (a superseding reconnect
                # may already have replaced us).
                if _clients.get(unit_id) is ws:
                    _clients.pop(unit_id, None)
            log.info("ws closed: unit=%s peer=%s", unit_id, peer)

    return ws


async def health(request: web.Request) -> web.Response:
    sr = _voice.config.sample_rate if _voice is not None else None
    return web.json_response({
        "status": "ok",
        "piper_voice": PIPER_VOICE,
        "piper_sample_rate": sr,
        "whisper_url": WHISPER_URL,
        "ws_clients": sorted(_clients.keys()),
        "tts_streaming": True,
        "tts_chunk_bytes": TTS_CHUNK_BYTES,
    })


async def _cleanup_pending(app):
    """Periodically sweep stale pending entries (orphaned by firmware crash, etc)."""
    while True:
        await asyncio.sleep(30)
        now = time.monotonic()
        stale = [k for k, v in _pending.items() if now - v["created"] > PENDING_TTL]
        for k in stale:
            _pending.pop(k, None)
        if stale:
            log.info("cleaned %d stale pending entries", len(stale))


async def on_startup(app):
    # Load the Piper model up-front so the first turn doesn't pay ONNX
    # cold-start cost. ~300-800 ms on okDemerzel.
    await asyncio.get_event_loop().run_in_executor(None, _load_voice)
    asyncio.ensure_future(_cleanup_pending(app))


def main():
    app = web.Application(client_max_size=10 * 1024 * 1024)  # 10 MB max upload
    app.on_startup.append(on_startup)
    app.router.add_post("/voice_turn", voice_turn)
    app.router.add_get("/voice_result/{request_id}", voice_result)
    app.router.add_post("/voice_text", voice_text)
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/health", health)
    log.info("starting on %s:%d", LISTEN_HOST, LISTEN_PORT)
    web.run_app(app, host=LISTEN_HOST, port=LISTEN_PORT, access_log=None)


if __name__ == "__main__":
    main()
