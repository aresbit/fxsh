#!/usr/bin/env python3
"""s01: one loop + one tool (fxsh_eval)."""

from common import create_client, run_agent_loop, run_fxsh_eval

TOOLS = [
    {
        "name": "fxsh_eval",
        "description": "Evaluate an fxsh expression.",
        "input_schema": {
            "type": "object",
            "properties": {"expr": {"type": "string"}},
            "required": ["expr"],
        },
    }
]

SYSTEM = """You are an fxsh coding agent.
Use fxsh_eval to evaluate expressions and inspect behavior.
Act via tools first, keep text concise."""


def main() -> None:
    client = create_client()
    handlers = {"fxsh_eval": lambda **kw: run_fxsh_eval(kw["expr"])}
    history = []
    while True:
        try:
            q = input("\033[36ms01-fxsh >> \033[0m")
        except (EOFError, KeyboardInterrupt):
            break
        if q.strip().lower() in ("q", "quit", "exit", ""):
            break
        history.append({"role": "user", "content": q})
        text = run_agent_loop(
            client=client,
            system=SYSTEM,
            tools=TOOLS,
            handlers=handlers,
            messages=history,
        )
        if text:
            print(text)
        print()


if __name__ == "__main__":
    main()
