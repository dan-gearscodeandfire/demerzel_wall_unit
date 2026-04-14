"""router.py — intent router for Demerzel voice turns.

Sends a transcribed user utterance to a small LLM (Llama 3.2 3B) with a
structured prompt and parses the JSON output. Falls back to GPT-OSS-120B
when the small model produces low confidence.

Output shape:
{
    "intent":          "ha_command | calendar | email | query | chat | system",
    "latency_class":   "instant | quick | slow",
    "verbal_response": "got_it | acknowledge_first | answer_only",
    "tool":            "ha_action | n8n_calendar | n8n_email | llm_chat",
    "params":          {...},
    "ack_phrase":      "Let me check..."   (optional)
    "confidence":      0.0 - 1.0
}
"""
import json
import logging
import os
import re

import requests

log = logging.getLogger("router")

LLAMA_3B_URL = os.environ.get("LLAMA_3B_URL", "http://127.0.0.1:8081")
GPTOSS_URL = os.environ.get("GPTOSS_URL", "http://127.0.0.1:8080")
CONFIDENCE_THRESHOLD = 0.7
LLM_TIMEOUT = 15

ROUTER_SYSTEM_PROMPT = """You are the intent router for Demerzel, a household voice assistant. \
Given a user utterance, classify it into a JSON object with EXACTLY these fields:

- intent: one of "ha_command", "ha_query", "calendar", "email", "query", "chat", "system"
    - ha_command: change the state of a device (turn on/off, set brightness, lock/unlock)
    - ha_query: read the state of a sensor or entity (temperature, humidity, lock state, lights on/off)
- latency_class: one of "instant", "quick", "slow"
    - instant: trivial action, no thinking needed (e.g. turn off lights)
    - quick:   simple lookup answered in <500ms (e.g. what time is it)
    - slow:    requires reasoning, external API, or multi-step lookup (e.g. read calendar)
- verbal_response: one of "got_it", "acknowledge_first", "answer_only"
    - got_it:             after performing an action, just say "Got it."
    - acknowledge_first:  for slow lookups, say a brief ack ("Let me check") then the real answer
    - answer_only:        speak the answer directly, no ack
- tool: one of "ha_action", "ha_query", "n8n_calendar", "n8n_email", "llm_chat"
- params: object with parameters appropriate for the tool
    - ha_action: {"domain": "light|switch|cover|...", "service": "turn_on|turn_off|toggle|...", "entity": "<entity_id or human name>"}
    - ha_query: {"entity": "<friendly name like 'den temperature' or 'shop multisensor'>"}
    - n8n_calendar: {"query": "<paraphrased question>"}
    - n8n_email: {"query": "<paraphrased question>"}
    - llm_chat: {"prompt": "<the user utterance>"}
- ack_phrase: only if verbal_response is "acknowledge_first" - a brief phrase like "Let me check that"
- confidence: your confidence 0.0-1.0 that this classification is correct

Output ONLY the JSON object. No prose, no markdown fences, no commentary.

Examples:

User: "turn off the lights"
{"intent":"ha_command","latency_class":"instant","verbal_response":"got_it","tool":"ha_action","params":{"domain":"light","service":"turn_off","entity":"all"},"confidence":0.98}

User: "what's on my calendar this week"
{"intent":"calendar","latency_class":"slow","verbal_response":"acknowledge_first","tool":"n8n_calendar","params":{"query":"events for this week"},"ack_phrase":"Let me check your calendar","confidence":0.95}

User: "what time is it"
{"intent":"query","latency_class":"quick","verbal_response":"answer_only","tool":"llm_chat","params":{"prompt":"What is the current time? Be brief."},"confidence":0.92}

User: "tell me a joke"
{"intent":"chat","latency_class":"quick","verbal_response":"answer_only","tool":"llm_chat","params":{"prompt":"Tell me a short joke."},"confidence":0.99}

User: "do I have any unread email"
{"intent":"email","latency_class":"slow","verbal_response":"acknowledge_first","tool":"n8n_email","params":{"query":"unread email count"},"ack_phrase":"Let me check","confidence":0.94}

User: "what's the temperature in the den"
{"intent":"ha_query","latency_class":"quick","verbal_response":"answer_only","tool":"ha_query","params":{"entity":"den temperature"},"confidence":0.96}

User: "is the front door locked"
{"intent":"ha_query","latency_class":"quick","verbal_response":"answer_only","tool":"ha_query","params":{"entity":"front door lock"},"confidence":0.94}

User: "how warm is it in the shop"
{"intent":"ha_query","latency_class":"quick","verbal_response":"answer_only","tool":"ha_query","params":{"entity":"shop temperature"},"confidence":0.94}
"""


def _call_llm(url: str, prompt: str, max_tokens: int = 300) -> str:
    """POST a chat completion to a llama.cpp-compatible server, return the text."""
    body = {
        "messages": [
            {"role": "system", "content": ROUTER_SYSTEM_PROMPT},
            {"role": "user", "content": prompt},
        ],
        "max_tokens": max_tokens,
        "temperature": 0.0,
        "stream": False,
    }
    r = requests.post(f"{url}/v1/chat/completions", json=body, timeout=LLM_TIMEOUT)
    r.raise_for_status()
    data = r.json()
    return data["choices"][0]["message"]["content"]


def _extract_json(text: str) -> dict | None:
    """Pull the first JSON object out of model output. Tolerates fences and prefixes."""
    if not text:
        return None
    # Strip markdown fences if present
    text = re.sub(r"^```(?:json)?\s*", "", text.strip())
    text = re.sub(r"\s*```$", "", text)
    # Find the first { ... } balanced span
    start = text.find("{")
    if start < 0:
        return None
    depth = 0
    in_str = False
    esc = False
    for i in range(start, len(text)):
        c = text[i]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        else:
            if c == '"':
                in_str = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    candidate = text[start:i + 1]
                    try:
                        return json.loads(candidate)
                    except json.JSONDecodeError:
                        return None
    return None


def _validate(obj: dict) -> bool:
    required = {"intent", "latency_class", "verbal_response", "tool", "params", "confidence"}
    if not isinstance(obj, dict):
        return False
    if not required.issubset(obj.keys()):
        return False
    try:
        obj["confidence"] = float(obj["confidence"])
    except (TypeError, ValueError):
        return False
    return True


def route(transcript: str) -> dict:
    """Classify a user utterance. Returns a router output dict."""
    if not transcript or not transcript.strip():
        return {
            "intent": "system",
            "latency_class": "instant",
            "verbal_response": "answer_only",
            "tool": "llm_chat",
            "params": {"prompt": "User said nothing."},
            "confidence": 1.0,
            "fallback_reason": "empty_transcript",
            "_router_model": "none",
        }

    # Try Llama 3B first
    try:
        raw = _call_llm(LLAMA_3B_URL, transcript)
        log.info("router 3B raw: %s", raw[:300])
        parsed = _extract_json(raw)
        if parsed and _validate(parsed):
            parsed["_router_model"] = "llama-3b"
            if parsed["confidence"] >= CONFIDENCE_THRESHOLD:
                return parsed
            else:
                log.info("router 3B confidence %.2f below threshold, escalating",
                         parsed["confidence"])
        else:
            log.warning("router 3B output invalid, escalating")
    except Exception as e:
        log.warning("router 3B failed: %s, escalating", e)

    # Escalate to GPT-OSS-120B
    try:
        raw = _call_llm(GPTOSS_URL, transcript, max_tokens=400)
        log.info("router 120B raw: %s", raw[:300])
        parsed = _extract_json(raw)
        if parsed and _validate(parsed):
            parsed["_router_model"] = "gpt-oss-120b"
            return parsed
    except Exception as e:
        log.error("router 120B failed: %s", e)

    # Total fallback — treat as chat
    return {
        "intent": "chat",
        "latency_class": "quick",
        "verbal_response": "answer_only",
        "tool": "llm_chat",
        "params": {"prompt": transcript},
        "confidence": 0.0,
        "fallback_reason": "all_routers_failed",
        "_router_model": "fallback",
    }
