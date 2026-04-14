"""composer.py — produces the natural-language reply from a tool result.

Logic:
    verbal_response = "got_it"            -> "Got it."
    verbal_response = "answer_only"       -> tool result if it's already speech-ready,
                                             otherwise summarized via LLM
    verbal_response = "acknowledge_first" -> the ack_phrase is spoken first by the caller;
                                             this function returns the *follow-up* answer
"""
import logging
import os

import requests

log = logging.getLogger("composer")

GPTOSS_URL = os.environ.get("GPTOSS_URL", "http://127.0.0.1:8080").rstrip("/")
LLAMA_3B_URL = os.environ.get("LLAMA_3B_URL", "http://127.0.0.1:8081").rstrip("/")
LLM_TIMEOUT = 20

# Use the small fast model for composition by default. GPT-OSS-120B has a
# reasoning quirk that leaks chain-of-thought into the content field.
COMPOSER_LLM_URL = LLAMA_3B_URL

GOT_IT_PHRASES = [
    "Got it.",
]


def _llm_message_text(message: dict) -> str:
    """Extract the user-visible text from a llama.cpp / GPT-OSS-120B response.

    GPT-OSS-120B has a quirk where it sometimes puts the answer in
    `reasoning_content` instead of `content` when max_tokens is hit or
    reasoning is enabled. Check both, prefer content if non-empty.
    """
    content = (message.get("content") or "").strip()
    if content:
        return content
    reasoning = (message.get("reasoning_content") or "").strip()
    if reasoning:
        # If the reasoning has a clear "Answer:" or final-paragraph marker, take that
        for marker in ("\nAnswer:", "\nFinal answer:", "\n\nFinal:"):
            if marker in reasoning:
                return reasoning.split(marker, 1)[-1].strip()
        return reasoning
    return ""


def _summarize(user_query: str, tool_result: str) -> str:
    """Use the small LLM (Llama 3B) to convert a tool result into a brief spoken answer."""
    try:
        r = requests.post(
            f"{COMPOSER_LLM_URL}/v1/chat/completions",
            json={
                "messages": [
                    {
                        "role": "system",
                        "content": (
                            "You are Demerzel, a household voice assistant speaking out loud. "
                            "Given the user's question and a raw data result, produce a brief "
                            "natural spoken response (1 sentence, max 2). Rules: "
                            "(1) Report the facts from the data exactly as given. "
                            "(2) Do NOT add subjective qualifiers like 'cool', 'warm', 'hot', "
                            "'cold', 'nice', 'pleasant', 'quite', 'pretty', 'really'. "
                            "(3) Do NOT comment on whether something is good or bad. "
                            "(4) Do NOT show reasoning, just speak the answer. "
                            "(5) No markdown, no lists, no quotes around the answer."
                        ),
                    },
                    {
                        "role": "user",
                        "content": f"Question: {user_query}\n\nData: {tool_result}\n\nReply:",
                    },
                ],
                "max_tokens": 250,
                "temperature": 0.4,
                "stream": False,
            },
            timeout=LLM_TIMEOUT,
        )
        r.raise_for_status()
        message = r.json()["choices"][0]["message"]
        text = _llm_message_text(message)
        if not text:
            log.warning("composer LLM returned empty message: %s", message)
            return tool_result[:300] if isinstance(tool_result, str) else str(tool_result)[:300]
        return text
    except Exception as e:
        log.warning("composer LLM failed: %s", e)
        return tool_result if isinstance(tool_result, str) else str(tool_result)


def compose(router: dict, tool_result: str, user_query: str) -> str:
    """Return the text Demerzel should speak."""
    verbal = router.get("verbal_response", "answer_only")

    # ha_action paths: short ack regardless of underlying result
    if router.get("intent") == "ha_command":
        if "could not find entity" in tool_result or "could not find" in tool_result:
            return "I couldn't find that device."
        if "error" in tool_result.lower() or "failed" in tool_result.lower():
            return f"Something went wrong. {tool_result}"
        return GOT_IT_PHRASES[0]

    # ha_query: a sensor reading, naturalize via the small LLM
    if router.get("tool") == "ha_query":
        if "could not find" in tool_result:
            return "I couldn't find that sensor."
        return _summarize(user_query, tool_result)

    # llm_chat path: result is already a natural reply
    if router.get("tool") == "llm_chat":
        return tool_result

    # calendar/email: need to summarize the raw API output
    if router.get("tool") in ("n8n_calendar", "n8n_email"):
        return _summarize(user_query, tool_result)

    # Fallback
    return tool_result
