#!/usr/bin/env python3
"""s02: tool dispatch map; loop unchanged."""

from common import create_client, handlers_base, run_agent_loop, tool_specs_base

SYSTEM = """You are an fxsh coding agent.
Use tools to read/edit files and run fxsh.
Prefer tool actions over long explanations."""


def main() -> None:
    client = create_client()
    history = []
    while True:
        try:
            q = input("\033[36ms02-fxsh >> \033[0m")
        except (EOFError, KeyboardInterrupt):
            break
        if q.strip().lower() in ("q", "quit", "exit", ""):
            break
        history.append({"role": "user", "content": q})
        text = run_agent_loop(
            client=client,
            system=SYSTEM,
            tools=tool_specs_base(),
            handlers=handlers_base(),
            messages=history,
        )
        if text:
            print(text)
        print()


if __name__ == "__main__":
    main()
