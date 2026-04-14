"""tools.py — backend tool dispatcher for the router.

Each handler takes the router's `params` dict and returns a string result.
The voice-server passes that result to the response composer.
"""
import logging
import os
import re

import requests

log = logging.getLogger("tools")

HA_URL = os.environ.get("HA_URL", "http://192.168.1.96:8123").rstrip("/")
HA_TOKEN = os.environ.get("HA_TOKEN", "")
N8N_BASE = os.environ.get("N8N_BASE_URL", "http://localhost:5678").rstrip("/")
GPTOSS_URL = os.environ.get("GPTOSS_URL", "http://127.0.0.1:8080").rstrip("/")
LLAMA_3B_URL = os.environ.get("LLAMA_3B_URL", "http://127.0.0.1:8081").rstrip("/")

HA_TIMEOUT = 8
N8N_TIMEOUT = 30
LLM_TIMEOUT = 30


# ---------- Home Assistant ----------

def _ha_headers():
    return {
        "Authorization": f"Bearer {HA_TOKEN}",
        "Content-Type": "application/json",
    }


def _ha_resolve_entity(domain: str, hint: str) -> str | None:
    """Resolve a fuzzy entity hint to a real HA entity_id.

    Strategies (in order):
    1. If hint is already a full entity_id (contains a dot), use as-is
    2. If hint is "all" or empty, return f"{domain}.all" (HA accepts this for groups)
    3. Look up all entities in the domain, find one whose friendly name matches hint
    """
    if not hint:
        return f"{domain}.all"
    if "." in hint:
        return hint
    if hint.lower() in ("all", "every", "everything"):
        return "all"  # HA service call accepts entity_id="all"
    try:
        r = requests.get(f"{HA_URL}/api/states", headers=_ha_headers(), timeout=HA_TIMEOUT)
        r.raise_for_status()
        states = r.json()
        hint_norm = re.sub(r"\s+", "", hint.lower())
        # Exact friendly_name match first
        for s in states:
            if not s["entity_id"].startswith(f"{domain}."):
                continue
            fn = s.get("attributes", {}).get("friendly_name", "")
            if fn and re.sub(r"\s+", "", fn.lower()) == hint_norm:
                return s["entity_id"]
        # Substring match
        for s in states:
            if not s["entity_id"].startswith(f"{domain}."):
                continue
            fn = s.get("attributes", {}).get("friendly_name", "")
            if fn and hint_norm in re.sub(r"\s+", "", fn.lower()):
                return s["entity_id"]
    except Exception as e:
        log.warning("HA entity resolution failed: %s", e)
    return None


def ha_action(params: dict) -> str:
    """Call a Home Assistant service. Returns a status string."""
    domain = params.get("domain") or "light"
    service = params.get("service") or "turn_off"
    entity_hint = params.get("entity") or "all"

    entity_id = _ha_resolve_entity(domain, entity_hint)
    if entity_id is None:
        return f"could not find entity matching '{entity_hint}'"

    body = {"entity_id": entity_id} if entity_id != "all" else {}

    try:
        r = requests.post(
            f"{HA_URL}/api/services/{domain}/{service}",
            headers=_ha_headers(),
            json=body,
            timeout=HA_TIMEOUT,
        )
        r.raise_for_status()
        return f"called {domain}.{service} on {entity_id}"
    except requests.HTTPError as e:
        return f"HA error {e.response.status_code}: {e.response.text[:200]}"
    except Exception as e:
        return f"HA call failed: {e}"


# ---------- n8n webhooks ----------

def n8n_calendar(params: dict) -> str:
    """Hit the n8n /webhook/check-calendar endpoint and return raw or normalized text."""
    query = params.get("query", "today's events")
    try:
        r = requests.post(
            f"{N8N_BASE}/webhook/check-calendar",
            json={"query": query},
            timeout=N8N_TIMEOUT,
        )
        r.raise_for_status()
        try:
            return _n8n_normalize(r.json(), "calendar")
        except ValueError:
            return r.text
    except requests.HTTPError as e:
        return f"calendar lookup error {e.response.status_code}: {e.response.text[:200]}"
    except Exception as e:
        return f"calendar lookup failed: {e}"


def _n8n_normalize(data, kind: str) -> str:
    """n8n webhook responses come in a few shapes. Normalize them."""
    if isinstance(data, dict):
        # n8n's "No item to return was found" sentinel = no items came out the end
        if data.get("code") == 0 and "No item" in str(data.get("message", "")):
            return f"no {kind} items"
        for key in ("summary", "result", "text", "answer", "messages", "events"):
            if key in data:
                v = data[key]
                if isinstance(v, list):
                    if not v:
                        return f"no {kind} items"
                    return str(v)
                return str(v)
        return str(data)
    if isinstance(data, list):
        if not data:
            return f"no {kind} items"
        return str(data)
    return str(data)


def n8n_email(params: dict) -> str:
    """Hit the n8n /webhook/check-email endpoint and return raw or normalized text."""
    query = params.get("query", "unread emails")
    try:
        r = requests.post(
            f"{N8N_BASE}/webhook/check-email",
            json={"query": query},
            timeout=N8N_TIMEOUT,
        )
        r.raise_for_status()
        try:
            return _n8n_normalize(r.json(), "email")
        except ValueError:
            return r.text
    except requests.HTTPError as e:
        return f"email lookup error {e.response.status_code}: {e.response.text[:200]}"
    except Exception as e:
        return f"email lookup failed: {e}"


# ---------- LLM chat ----------

def _extract_llm_text(message: dict) -> str:
    """Handle GPT-OSS-120B's reasoning_content quirk."""
    content = (message.get("content") or "").strip()
    if content:
        return content
    reasoning = (message.get("reasoning_content") or "").strip()
    if reasoning:
        for marker in ("\nAnswer:", "\nFinal answer:", "\n\nFinal:"):
            if marker in reasoning:
                return reasoning.split(marker, 1)[-1].strip()
        return reasoning
    return ""


def llm_chat(params: dict) -> str:
    """Direct chat with GPT-OSS-120B for general questions / chitchat."""
    prompt = params.get("prompt", "")
    try:
        r = requests.post(
            f"{GPTOSS_URL}/v1/chat/completions",
            json={
                "messages": [
                    {
                        "role": "system",
                        "content": (
                            "You are Demerzel, a household voice assistant. "
                            "Keep responses brief (1-2 sentences) and conversational. "
                            "Always answer directly, never show your reasoning. "
                            "If you don't know something, say so plainly."
                        ),
                    },
                    {"role": "user", "content": prompt},
                ],
                "max_tokens": 250,
                "temperature": 0.5,
                "stream": False,
            },
            timeout=LLM_TIMEOUT,
        )
        r.raise_for_status()
        text = _extract_llm_text(r.json()["choices"][0]["message"])
        return text or "I'm not sure how to answer that."
    except Exception as e:
        log.warning("llm_chat failed: %s", e)
        return f"I had trouble answering that: {e}"


# ---------- dispatcher ----------

DISPATCH = {
    "ha_action": ha_action,
    "n8n_calendar": n8n_calendar,
    "n8n_email": n8n_email,
    "llm_chat": llm_chat,
}


def dispatch(tool: str, params: dict) -> str:
    fn = DISPATCH.get(tool)
    if fn is None:
        return f"no dispatcher for tool '{tool}'"
    log.info("dispatching tool=%s params=%s", tool, params)
    result = fn(params)
    log.info("tool result: %s", result[:200] if isinstance(result, str) else result)
    return result
