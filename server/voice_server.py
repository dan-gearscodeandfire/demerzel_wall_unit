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


def compose_reply(transcript: str) -> str:
    """Stage 1: hardcoded response so we can see the loop work end-to-end."""
    if not transcript:
        return "I did not catch that."
    return f"You said: {transcript}"


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

    reply = compose_reply(transcript)
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

    return web.Response(
        body=wav_out,
        content_type="audio/wav",
        headers={
            "X-Transcript": hdr_safe(transcript),
            "X-Reply-Text": hdr_safe(reply),
            "X-Latency-Ms": str(elapsed_ms),
        },
    )


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
    app.router.add_get("/health", health)
    log.info("starting on %s:%d", LISTEN_HOST, LISTEN_PORT)
    web.run_app(app, host=LISTEN_HOST, port=LISTEN_PORT, access_log=None)


if __name__ == "__main__":
    main()
