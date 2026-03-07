#!/usr/bin/env python3
"""s06: context compact (micro + auto + manual)."""

from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any

from common import create_client, extract_text_blocks, handlers_base, tool_specs_base

WORKDIR = Path.cwd()
TRANSCRIPT_DIR = WORKDIR / ".transcripts"
THRESHOLD = 50000
KEEP_RECENT_TOOL_RESULTS = 3

SYSTEM = f"""You are an fxsh coding agent at {WORKDIR}.
Use tools to solve tasks.
Use the compact tool when context gets noisy."""

COMPACT_TOOL = {
    "name": "compact",
    "description": "Trigger manual conversation compression.",
    "input_schema": {
        "type": "object",
        "properties": {
            "focus": {
                "type": "string",
                "description": "Optional focus to preserve in summary",
            }
        },
    },
}


def estimate_tokens(messages: list[dict[str, Any]]) -> int:
    return len(json.dumps(messages, default=str, ensure_ascii=False)) // 4


def _block_type(block: Any) -> str:
    if isinstance(block, dict):
        return str(block.get("type", ""))
    return str(getattr(block, "type", ""))


def _block_name(block: Any) -> str:
    if isinstance(block, dict):
        return str(block.get("name", ""))
    return str(getattr(block, "name", ""))


def _block_id(block: Any) -> str:
    if isinstance(block, dict):
        return str(block.get("id", ""))
    return str(getattr(block, "id", ""))


def _block_input(block: Any) -> dict[str, Any]:
    if isinstance(block, dict):
        v = block.get("input", {})
        return v if isinstance(v, dict) else {}
    v = getattr(block, "input", {})
    return v if isinstance(v, dict) else {}


def micro_compact(messages: list[dict[str, Any]]) -> None:
    tool_results: list[dict[str, Any]] = []
    for msg in messages:
        if msg.get("role") != "user":
            continue
        content = msg.get("content")
        if not isinstance(content, list):
            continue
        for part in content:
            if isinstance(part, dict) and part.get("type") == "tool_result":
                tool_results.append(part)

    if len(tool_results) <= KEEP_RECENT_TOOL_RESULTS:
        return

    tool_name_by_id: dict[str, str] = {}
    for msg in messages:
        if msg.get("role") != "assistant":
            continue
        content = msg.get("content")
        if not isinstance(content, list):
            continue
        for block in content:
            if _block_type(block) == "tool_use":
                tool_name_by_id[_block_id(block)] = _block_name(block)

    for result in tool_results[:-KEEP_RECENT_TOOL_RESULTS]:
        content = result.get("content")
        if not isinstance(content, str) or len(content) <= 100:
            continue
        tool_id = str(result.get("tool_use_id", ""))
        tool_name = tool_name_by_id.get(tool_id, "unknown")
        result["content"] = f"[Previous: used {tool_name}]"


def auto_compact(client: Any, model: str, messages: list[dict[str, Any]], focus: str = "") -> list[dict[str, Any]]:
    TRANSCRIPT_DIR.mkdir(exist_ok=True)
    ts = int(time.time())
    transcript_path = TRANSCRIPT_DIR / f"transcript_{ts}.jsonl"
    with transcript_path.open("w", encoding="utf-8") as f:
        for msg in messages:
            f.write(json.dumps(msg, ensure_ascii=False, default=str) + "\n")
    print(f"[transcript saved: {transcript_path}]")

    focus_prompt = f"\nExtra focus: {focus}\n" if focus else ""
    source = json.dumps(messages, ensure_ascii=False, default=str)[:90000]
    prompt = (
        "Summarize this conversation for continuity. Include: "
        "1) accomplished work, 2) current state, 3) pending risks/actions. "
        "Keep critical file paths and decisions."
        + focus_prompt
        + "\nConversation:\n"
        + source
    )

    response = client.messages.create(
        model=model,
        messages=[{"role": "user", "content": prompt}],
        max_tokens=2200,
    )
    summary = extract_text_blocks(response.content)
    if not summary:
        summary = "Summary unavailable; continue from transcript."

    return [
        {
            "role": "user",
            "content": f"[Conversation compressed. Transcript: {transcript_path}]\n\n{summary}",
        },
        {
            "role": "assistant",
            "content": "Understood. Context restored from summary. Continuing.",
        },
    ]


def main() -> None:
    client = create_client()
    model = __import__("os").getenv("MODEL_ID", "")
    if not model:
        raise RuntimeError("MODEL_ID is required")

    handlers = handlers_base()
    handlers["compact"] = lambda **kw: "Manual compression requested."
    tools = tool_specs_base() + [COMPACT_TOOL]

    history: list[dict[str, Any]] = []

    while True:
        try:
            q = input("\033[36ms06-fxsh >> \033[0m")
        except (EOFError, KeyboardInterrupt):
            break
        if q.strip().lower() in ("q", "quit", "exit", ""):
            break

        history.append({"role": "user", "content": q})

        while True:
            micro_compact(history)
            if estimate_tokens(history) > THRESHOLD:
                print("[auto_compact triggered]")
                history[:] = auto_compact(client, model, history)

            response = client.messages.create(
                model=model,
                system=SYSTEM,
                messages=history,
                tools=tools,
                max_tokens=8000,
            )
            history.append({"role": "assistant", "content": response.content})

            if response.stop_reason != "tool_use":
                text = extract_text_blocks(response.content)
                if text:
                    print(text)
                print()
                break

            results: list[dict[str, Any]] = []
            manual_compact = False
            compact_focus = ""

            for block in response.content:
                if _block_type(block) != "tool_use":
                    continue
                name = _block_name(block)
                inp = _block_input(block)
                if name == "compact":
                    manual_compact = True
                    compact_focus = str(inp.get("focus", ""))
                    output = "Compressing context..."
                else:
                    handler = handlers.get(name)
                    try:
                        output = handler(**inp) if handler else f"Unknown tool: {name}"
                    except Exception as e:
                        output = f"Error: {e}"
                print(f"> {name}: {str(output)[:200]}")
                results.append(
                    {
                        "type": "tool_result",
                        "tool_use_id": _block_id(block),
                        "content": str(output),
                    }
                )

            history.append({"role": "user", "content": results})

            if manual_compact:
                print("[manual compact]")
                history[:] = auto_compact(client, model, history, focus=compact_focus)


if __name__ == "__main__":
    main()
