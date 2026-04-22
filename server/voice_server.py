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
import json
import logging
import os
import re
import subprocess
import tempfile
import time
import uuid
import wave

import requests
from aiohttp import web

import router as router_mod
import tools as tools_mod
import composer as composer_mod

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("voice-server")

HERE = os.path.dirname(os.path.abspath(__file__))
PIPER_BIN = os.path.join(HERE, ".venv", "bin", "piper")
PIPER_VOICE = os.path.join(HERE, "voices", "en_US-lessac-medium.onnx")
WHISPER_URL = "http://127.0.0.1:8891/inference"
LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 8900


def transcribe(wav_bytes: bytes) -> str:
    """POST a WAV to whisper.cpp and return the transcription text."""
    files = {"file": ("audio.wav", wav_bytes, "audio/wav")}
    data = {"temperature": "0.0", "response_format": "json"}
    r = requests.post(WHISPER_URL, files=files, data=data, timeout=30)
    r.raise_for_status()
    return r.json().get("text", "").strip()


def synthesize(text: str) -> bytes:
    """Run Piper as a subprocess and return the generated WAV bytes."""
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        out_path = f.name
    try:
        proc = subprocess.run(
            [PIPER_BIN, "--model", PIPER_VOICE, "--output_file", out_path],
            input=text,
            text=True,
            capture_output=True,
            check=True,
        )
        with open(out_path, "rb") as fh:
            return fh.read()
    finally:
        try:
            os.unlink(out_path)
        except FileNotFoundError:
            pass


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


async def _slow_background(request_id: str, transcript: str, decision: dict):
    """Run tool dispatch + compose + synthesize in background, store result."""
    entry = _pending[request_id]
    try:
        loop = asyncio.get_event_loop()
        reply, debug = await loop.run_in_executor(
            None, _stage2_from_decision, transcript, decision
        )
        wav_out = await loop.run_in_executor(None, synthesize, reply)
        entry["wav"] = wav_out
        entry["reply_text"] = reply
        entry["debug"] = debug
    except Exception as e:
        log.exception("background task failed for %s", request_id)
        fallback = "Sorry, something went wrong."
        try:
            entry["wav"] = await loop.run_in_executor(None, synthesize, fallback)
        except Exception:
            entry["wav"] = b""
        entry["reply_text"] = fallback
        entry["error"] = str(e)
    finally:
        entry["event"].set()
        # Best-effort WS push: lets the firmware GET /voice_result immediately
        # instead of waiting out its long-poll. Falls through silently if the
        # unit isn't on the control channel.
        unit_id = entry.get("unit_id")
        if unit_id:
            await notify_unit(unit_id, "pending_ready", request_id=request_id)


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

    # Filter non-speech
    if not _is_routable_transcript(transcript):
        log.info("dropping non-speech transcript: %r", transcript)
        fallback = "I did not catch that."
        try:
            wav_out = synthesize(fallback)
        except Exception:
            return web.json_response({"error": "synth_failed"}, status=500)
        return web.Response(
            body=wav_out, content_type="audio/wav",
            headers={
                "X-Transcript": _hdr_safe(transcript),
                "X-Reply-Text": _hdr_safe(fallback),
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
    except subprocess.CalledProcessError as e:
        log.exception("piper failed: %s", e.stderr)
        return web.json_response({"error": "piper_failed", "detail": e.stderr}, status=500)
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
        {"type": "pending_ready", "request_id": "..."}
        {"type": "barge_in",      "reason": "..."}         # future: Stage 4
        {"type": "suppress",      "ms": N}                 # future: arbitration
        {"type": "config",        ...}                     # future

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
    return web.json_response({
        "status": "ok",
        "piper_bin": PIPER_BIN,
        "piper_voice": PIPER_VOICE,
        "whisper_url": WHISPER_URL,
        "ws_clients": sorted(_clients.keys()),
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
