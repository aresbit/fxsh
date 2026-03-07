#!/usr/bin/env python3
"""s05: skill metadata in system + on-demand full content via load_skill."""

from pathlib import Path

from common import SkillLoader, create_client, handlers_base, run_agent_loop, tool_specs_base

SKILLS = SkillLoader(Path.cwd() / "skills")

SYSTEM = f"""You are an fxsh coding agent.
Use load_skill(name) before unfamiliar tasks.

Available skills:
{SKILLS.descriptions()}
"""

LOAD_SKILL_TOOL = {
    "name": "load_skill",
    "description": "Load full SKILL.md content by skill name.",
    "input_schema": {
        "type": "object",
        "properties": {"name": {"type": "string"}},
        "required": ["name"],
    },
}


def main() -> None:
    client = create_client()
    handlers = handlers_base()
    handlers["load_skill"] = lambda **kw: SKILLS.load(kw["name"])
    tools = tool_specs_base() + [LOAD_SKILL_TOOL]

    history = []
    while True:
        try:
            q = input("\033[36ms05-fxsh >> \033[0m")
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
        )
        if text:
            print(text)
        print()


if __name__ == "__main__":
    main()
