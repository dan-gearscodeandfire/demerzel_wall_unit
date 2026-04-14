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
import json
import logging
import os
import subprocess
import tempfile
import time
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


def stage2_pipeline(transcript: str) -> tuple[str, dict]:
    """Stage 2: route → dispatch → compose. Returns (spoken_text, debug_info)."""
    debug = {}
    if not transcript:
        return "I did not catch that.", {"reason": "empty_transcript"}

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


async def voice_turn(request: web.Request) -> web.Response:
    t0 = time.monotonic()
    wav_in = await request.read()
    log.info("received %d bytes of audio", len(wav_in))

    try:
        transcript = transcribe(wav_in)
    except Exception as e:
        log.exception("whisper failed")
        return web.json_response({"error": "whisper_failed", "detail": str(e)}, status=500)
    log.info("transcript: %r", transcript)

    try:
        reply, debug = stage2_pipeline(transcript)
    except Exception as e:
        log.exception("stage2 pipeline failed")
        reply = "Sorry, something went wrong."
        debug = {"error": str(e)}
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

    def hdr_safe(s: str) -> str:
        # Strip CR/LF/NUL and cap length so we never fail header serialization
        return " ".join(s.split())[:500]

    headers = {
        "X-Transcript": hdr_safe(transcript),
        "X-Reply-Text": hdr_safe(reply),
        "X-Latency-Ms": str(elapsed_ms),
    }
    # Surface stage-2 routing decisions for debugging
    if debug.get("router"):
        d = debug["router"]
        headers["X-Router-Model"] = hdr_safe(str(d.get("_router_model", "")))
        headers["X-Router-Intent"] = hdr_safe(str(d.get("intent", "")))
        headers["X-Router-Tool"] = hdr_safe(str(d.get("tool", "")))
        headers["X-Router-Confidence"] = hdr_safe(str(d.get("confidence", "")))
    if debug.get("router_ms") is not None:
        headers["X-Router-Ms"] = str(debug["router_ms"])
    if debug.get("tool_ms") is not None:
        headers["X-Tool-Ms"] = str(debug["tool_ms"])
    if debug.get("compose_ms") is not None:
        headers["X-Compose-Ms"] = str(debug["compose_ms"])

    return web.Response(
        body=wav_out,
        content_type="audio/wav",
        headers=headers,
    )


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


async def health(request: web.Request) -> web.Response:
    return web.json_response({
        "status": "ok",
        "piper_bin": PIPER_BIN,
        "piper_voice": PIPER_VOICE,
        "whisper_url": WHISPER_URL,
    })


def main():
    app = web.Application(client_max_size=10 * 1024 * 1024)  # 10 MB max upload
    app.router.add_post("/voice_turn", voice_turn)
    app.router.add_post("/voice_text", voice_text)
    app.router.add_get("/health", health)
    log.info("starting on %s:%d", LISTEN_HOST, LISTEN_PORT)
    web.run_app(app, host=LISTEN_HOST, port=LISTEN_PORT, access_log=None)


if __name__ == "__main__":
    main()
