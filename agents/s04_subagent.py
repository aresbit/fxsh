#!/usr/bin/env python3
"""s04: subagent with fresh context via task tool."""

from common import create_client, handlers_base, run_agent_loop, tool_specs_base

PARENT_SYSTEM = """You are an fxsh coding agent.
Delegate exploration or scoped subtasks with the task tool when useful."""

CHILD_SYSTEM = """You are an fxsh subagent with fresh context.
Complete only the requested subtask and return a concise summary."""

TASK_TOOL = {
    "name": "task",
    "description": "Spawn subagent with clean context and return summary.",
    "input_schema": {
        "type": "object",
        "properties": {
            "prompt": {"type": "string"},
            "description": {"type": "string"},
        },
        "required": ["prompt"],
    },
}


def main() -> None:
    client = create_client()
    base_tools = tool_specs_base()
    base_handlers = handlers_base()

    def run_subagent(prompt: str) -> str:
        child_messages = [{"role": "user", "content": prompt}]
        return run_agent_loop(
            client=client,
            system=CHILD_SYSTEM,
            tools=base_tools,
            handlers=base_handlers,
            messages=child_messages,
            max_rounds=30,
        )

    handlers = dict(base_handlers)
    handlers["task"] = lambda **kw: run_subagent(kw["prompt"])
    tools = base_tools + [TASK_TOOL]

    history = []
    while True:
        try:
            q = input("\033[36ms04-fxsh >> \033[0m")
        except (EOFError, KeyboardInterrupt):
            break
        if q.strip().lower() in ("q", "quit", "exit", ""):
            break
        history.append({"role": "user", "content": q})
        text = run_agent_loop(
            client=client,
            system=PARENT_SYSTEM,
            tools=tools,
            handlers=handlers,
            messages=history,
        )
        if text:
            print(text)
        print()


if __name__ == "__main__":
    main()
