#!/usr/bin/env python3
"""s07: persisted task system with dependency graph."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from common import create_client, extract_text_blocks, handlers_base, tool_specs_base

WORKDIR = Path.cwd()
TASKS_DIR = WORKDIR / ".tasks"

SYSTEM = f"""You are an fxsh coding agent at {WORKDIR}.
Use task tools to plan and track multi-step work.
Respect dependencies before starting blocked tasks."""


class TaskManager:
    def __init__(self, tasks_dir: Path):
        self.dir = tasks_dir
        self.dir.mkdir(exist_ok=True)
        self._next_id = self._max_id() + 1

    def _max_id(self) -> int:
        ids: list[int] = []
        for f in self.dir.glob("task_*.json"):
            try:
                ids.append(int(f.stem.split("_", 1)[1]))
            except Exception:
                continue
        return max(ids) if ids else 0

    def _path(self, task_id: int) -> Path:
        return self.dir / f"task_{task_id}.json"

    def _load(self, task_id: int) -> dict[str, Any]:
        p = self._path(task_id)
        if not p.exists():
            raise ValueError(f"Task {task_id} not found")
        return json.loads(p.read_text(encoding="utf-8"))

    def _save(self, task: dict[str, Any]) -> None:
        self._path(int(task["id"])).write_text(json.dumps(task, ensure_ascii=False, indent=2), encoding="utf-8")

    def create(self, subject: str, description: str = "") -> str:
        subject = str(subject).strip()
        if not subject:
            raise ValueError("subject is required")
        task = {
            "id": self._next_id,
            "subject": subject,
            "description": str(description),
            "status": "pending",
            "blockedBy": [],
            "blocks": [],
            "owner": "",
        }
        self._save(task)
        self._next_id += 1
        return json.dumps(task, ensure_ascii=False, indent=2)

    def get(self, task_id: int) -> str:
        return json.dumps(self._load(int(task_id)), ensure_ascii=False, indent=2)

    def _clear_dependency(self, completed_id: int) -> None:
        for f in self.dir.glob("task_*.json"):
            task = json.loads(f.read_text(encoding="utf-8"))
            blocked = task.get("blockedBy", [])
            if completed_id in blocked:
                task["blockedBy"] = [x for x in blocked if x != completed_id]
                self._save(task)

    def update(
        self,
        task_id: int,
        status: str | None = None,
        addBlockedBy: list[int] | None = None,
        addBlocks: list[int] | None = None,
    ) -> str:
        tid = int(task_id)
        task = self._load(tid)

        if status is not None:
            status = str(status)
            if status not in ("pending", "in_progress", "completed"):
                raise ValueError(f"Invalid status: {status}")
            task["status"] = status
            if status == "completed":
                self._clear_dependency(tid)

        if addBlockedBy:
            vals = sorted(set(int(x) for x in addBlockedBy if int(x) != tid))
            task["blockedBy"] = sorted(set(task.get("blockedBy", []) + vals))

        if addBlocks:
            vals = sorted(set(int(x) for x in addBlocks if int(x) != tid))
            task["blocks"] = sorted(set(task.get("blocks", []) + vals))
            for bid in vals:
                try:
                    blocked_task = self._load(bid)
                except ValueError:
                    continue
                if tid not in blocked_task.get("blockedBy", []):
                    blocked_task["blockedBy"] = sorted(blocked_task.get("blockedBy", []) + [tid])
                    self._save(blocked_task)

        self._save(task)
        return json.dumps(task, ensure_ascii=False, indent=2)

    def list_all(self) -> str:
        tasks: list[dict[str, Any]] = []
        for f in sorted(self.dir.glob("task_*.json"), key=lambda p: p.name):
            try:
                tasks.append(json.loads(f.read_text(encoding="utf-8")))
            except Exception:
                continue
        if not tasks:
            return "No tasks."

        mark = {"pending": "[ ]", "in_progress": "[>]", "completed": "[x]"}
        lines: list[str] = []
        for t in tasks:
            m = mark.get(str(t.get("status", "pending")), "[?]")
            blocked = t.get("blockedBy", [])
            suffix = f" (blocked by: {blocked})" if blocked else ""
            lines.append(f"{m} #{t.get('id')}: {t.get('subject', '')}{suffix}")
        return "\n".join(lines)


TASKS = TaskManager(TASKS_DIR)

TASK_TOOLS = [
    {
        "name": "task_create",
        "description": "Create a persisted task.",
        "input_schema": {
            "type": "object",
            "properties": {
                "subject": {"type": "string"},
                "description": {"type": "string"},
            },
            "required": ["subject"],
        },
    },
    {
        "name": "task_update",
        "description": "Update task status or dependencies.",
        "input_schema": {
            "type": "object",
            "properties": {
                "task_id": {"type": "integer"},
                "status": {
                    "type": "string",
                    "enum": ["pending", "in_progress", "completed"],
                },
                "addBlockedBy": {"type": "array", "items": {"type": "integer"}},
                "addBlocks": {"type": "array", "items": {"type": "integer"}},
            },
            "required": ["task_id"],
        },
    },
    {
        "name": "task_list",
        "description": "List all tasks with status summary.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "task_get",
        "description": "Get full task JSON by id.",
        "input_schema": {
            "type": "object",
            "properties": {"task_id": {"type": "integer"}},
            "required": ["task_id"],
        },
    },
]


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


def main() -> None:
    client = create_client()
    model = os.getenv("MODEL_ID", "")
    if not model:
        raise RuntimeError("MODEL_ID is required")

    handlers = handlers_base()
    handlers["task_create"] = lambda **kw: TASKS.create(kw["subject"], kw.get("description", ""))
    handlers["task_update"] = (
        lambda **kw: TASKS.update(
            kw["task_id"], kw.get("status"), kw.get("addBlockedBy"), kw.get("addBlocks")
        )
    )
    handlers["task_list"] = lambda **kw: TASKS.list_all()
    handlers["task_get"] = lambda **kw: TASKS.get(kw["task_id"])

    tools = tool_specs_base() + TASK_TOOLS
    history: list[dict[str, Any]] = []

    while True:
        try:
            q = input("\033[36ms07-fxsh >> \033[0m")
        except (EOFError, KeyboardInterrupt):
            break
        if q.strip().lower() in ("q", "quit", "exit", ""):
            break

        history.append({"role": "user", "content": q})

        while True:
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
            for block in response.content:
                if _block_type(block) != "tool_use":
                    continue
                name = _block_name(block)
                inp = _block_input(block)
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


if __name__ == "__main__":
    main()
