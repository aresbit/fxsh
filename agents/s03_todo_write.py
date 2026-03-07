#!/usr/bin/env python3
"""s03: todo state + reminder injection."""

from common import TodoManager, create_client, handlers_base, run_agent_loop, tool_specs_base

TODO = TodoManager()

SYSTEM = """You are an fxsh coding agent.
For multi-step work, maintain todo state:
- mark one task in_progress
- finish with all completed
Use tools to act."""

TODO_TOOL = {
    "name": "todo",
    "description": "Update todo items for current task plan.",
    "input_schema": {
        "type": "object",
        "properties": {
            "items": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "id": {"type": "string"},
                        "text": {"type": "string"},
                        "status": {
                            "type": "string",
                            "enum": ["pending", "in_progress", "completed"],
                        },
                    },
                    "required": ["id", "text", "status"],
                },
            }
        },
        "required": ["items"],
    },
}


def main() -> None:
    client = create_client()
    handlers = handlers_base()
    handlers["todo"] = lambda **kw: TODO.update(kw["items"])
    tools = tool_specs_base() + [TODO_TOOL]

    history = []
    rounds_without_todo = 0

    def after_round(used_tools, results):
        nonlocal rounds_without_todo
        rounds_without_todo = 0 if "todo" in used_tools else rounds_without_todo + 1
        if rounds_without_todo >= 3:
            results.insert(
                0,
                {
                    "type": "text",
                    "text": "<reminder>Update your todo list for this task.</reminder>",
                },
            )

    while True:
        try:
            q = input("\033[36ms03-fxsh >> \033[0m")
        except (EOFError, KeyboardInterrupt):
            break
        if q.strip().lower() in ("q", "quit", "exit", ""):
            break
        history.append({"role": "user", "content": q})
        text = run_agent_loop(
            client=client,
            system=SYSTEM,
            tools=tools,
            handlers=handlers,
            messages=history,
            after_round=after_round,
        )
        if text:
            print(text)
        print()


if __name__ == "__main__":
    main()
