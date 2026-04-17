"""Offline unit tests for voice_server.py — specifically the whisper
non-speech transcript filter.

Whisper regularly emits bracketed annotations ([BLANK_AUDIO], [SOUND]) or
parenthesized ones ((water running), (clicking)) for ambient audio that
isn't speech. Before the filter, these passed straight to the router which
would hallucinate an intent and produce real HA / LLM actions.

Run with: python3 server/test_voice_server.py

Guard against regression: any change to _is_routable_transcript must keep
all these cases returning the expected boolean.
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Stub out the heavy imports that voice_server.py would otherwise pull in
# at module load time, so we can exercise _is_routable_transcript in
# isolation without needing whisper / Piper / HA / etc.
import types
for name in ("requests", "tools", "router", "composer"):
    if name not in sys.modules:
        sys.modules[name] = types.SimpleNamespace(
            dispatch=lambda *a, **k: "",
            route=lambda *a, **k: {},
            compose=lambda *a, **k: "",
        )
_web_stub = types.SimpleNamespace(
    Request=object,
    Response=object,
    Application=lambda: types.SimpleNamespace(
        router=types.SimpleNamespace(add_post=lambda *a, **k: None,
                                      add_get=lambda *a, **k: None),
    ),
    run_app=lambda *a, **k: None,
    json_response=lambda *a, **k: None,
)
sys.modules.setdefault("aiohttp", types.SimpleNamespace(web=_web_stub))

import voice_server  # noqa: E402


def check(label, got, expected):
    ok = got == expected
    mark = "OK  " if ok else "FAIL"
    print(f"  [{mark}] {label}   got={got}  expected={expected}")
    return ok


def run():
    print("== voice_server._is_routable_transcript ==")
    passed = 0
    failed = 0

    cases = [
        # (label, transcript, expected)

        # Reject: empty / whitespace-only
        ("empty string",                 "",                          False),
        ("whitespace only",              "   ",                       False),

        # Reject: pure bracketed whisper annotations
        ("[BLANK_AUDIO] alone",          "[BLANK_AUDIO]",             False),
        ("[SOUND] alone",                "[SOUND]",                   False),
        ("[MUSIC] alone",                "[MUSIC]",                   False),
        ("[NOISE] alone",                "[NOISE]",                   False),
        ("[SOUND] with leading/trailing ws", "  [SOUND]  ",           False),
        ("multiple bracketed annots",    "[SOUND] [MUSIC]",           False),

        # Reject: pure parenthesized whisper annotations
        ("(water running) alone",        "(water running)",           False),
        ("(clicking) alone",             "(clicking)",                False),
        ("multiple paren annots",        "(water running) (clicking)", False),

        # Reject: no alphabetic content (whisper digit-dot garbage)
        ("digit-dot garbage",            "( ( 2. 3. 4. 5.",           False),
        ("just dots",                    "...",                       False),
        ("just punctuation",             "... ???",                   False),
        ("digits and spaces",            "1 2 3 4 5",                 False),

        # Accept: real speech, even minimal
        ("clean wake word",              "Yo Demerzel",               True),
        ("clean command",                "Turn off the lights",       True),
        ("single word",                  "hello",                     True),
        ("question with punct",          "what time is it?",          True),
        ("real speech after whisper annotation", "[BLANK_AUDIO] Yo Demerzel", True),
        ("single letter (minimal)",      "a",                         True),
    ]

    for label, transcript, expected in cases:
        got = voice_server._is_routable_transcript(transcript)
        if check(label, got, expected):
            passed += 1
        else:
            failed += 1

    print()
    print(f"{passed} passed, {failed} failed")
    return failed


if __name__ == "__main__":
    sys.exit(run())
