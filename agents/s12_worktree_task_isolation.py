#!/usr/bin/env python3
"""s12: worktree + task isolation with lifecycle events."""

from __future__ import annotations

import json
import os
import re
import subprocess
import time
from pathlib import Path
from typing import Any

from common import create_client, extract_text_blocks, handlers_base, tool_specs_base

WORKDIR = Path.cwd()


def detect_repo_root(cwd: Path) -> Path | None:
    try:
        r = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=10,
        )
        if r.returncode != 0:
            return None
        root = Path(r.stdout.strip())
        return root if root.exists() else None
    except Exception:
        return None


REPO_ROOT = detect_repo_root(WORKDIR) or WORKDIR

SYSTEM = f"""You are an fxsh coding agent at {WORKDIR}.
Use tasks to manage goals and worktrees to isolate execution directories.
For larger changes: create tasks, create/bind worktrees, run changes in lane, then keep/remove for closeout.
Use worktree_events for lifecycle visibility."""


class EventBus:
    def __init__(self, path: Path):
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if not self.path.exists():
            self.path.write_text("", encoding="utf-8")

    def emit(
        self,
        event: str,
        task: dict[str, Any] | None = None,
        worktree: dict[str, Any] | None = None,
        error: str | None = None,
    ) -> None:
        payload: dict[str, Any] = {
            "event": event,
            "ts": time.time(),
            "task": task or {},
            "worktree": worktree or {},
        }
        if error:
            payload["error"] = str(error)
        with self.path.open("a", encoding="utf-8") as f:
            f.write(json.dumps(payload, ensure_ascii=False) + "\n")

    def list_recent(self, limit: int = 20) -> str:
        n = max(1, min(int(limit or 20), 200))
        lines = self.path.read_text(encoding="utf-8").splitlines()
        out: list[dict[str, Any]] = []
        for line in lines[-n:]:
            try:
                out.append(json.loads(line))
            except Exception:
                out.append({"event": "parse_error", "raw": line})
        return json.dumps(out, ensure_ascii=False, indent=2)


class TaskManager:
    def __init__(self, tasks_dir: Path):
        self.dir = tasks_dir
        self.dir.mkdir(parents=True, exist_ok=True)
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
        return self.dir / f"task_{int(task_id)}.json"

    def exists(self, task_id: int) -> bool:
        return self._path(task_id).exists()

    def _load(self, task_id: int) -> dict[str, Any]:
        p = self._path(task_id)
        if not p.exists():
            raise ValueError(f"Task {task_id} not found")
        return json.loads(p.read_text(encoding="utf-8"))

    def _save(self, task: dict[str, Any]) -> None:
        self._path(int(task["id"])).write_text(
            json.dumps(task, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )

    def create(self, subject: str, description: str = "") -> str:
        task = {
            "id": self._next_id,
            "subject": str(subject).strip(),
            "description": str(description),
            "status": "pending",
            "owner": "",
            "worktree": "",
            "blockedBy": [],
            "created_at": time.time(),
            "updated_at": time.time(),
        }
        if not task["subject"]:
            raise ValueError("subject is required")
        self._save(task)
        self._next_id += 1
        return json.dumps(task, ensure_ascii=False, indent=2)

    def get(self, task_id: int) -> str:
        return json.dumps(self._load(task_id), ensure_ascii=False, indent=2)

    def update(self, task_id: int, status: str | None = None, owner: str | None = None) -> str:
        task = self._load(task_id)
        if status is not None:
            st = str(status)
            if st not in ("pending", "in_progress", "completed"):
                raise ValueError(f"Invalid status: {st}")
            task["status"] = st
        if owner is not None:
            task["owner"] = str(owner)
        task["updated_at"] = time.time()
        self._save(task)
        return json.dumps(task, ensure_ascii=False, indent=2)

    def bind_worktree(self, task_id: int, worktree: str, owner: str = "") -> str:
        task = self._load(task_id)
        task["worktree"] = str(worktree)
        if owner:
            task["owner"] = str(owner)
        if task.get("status") == "pending":
            task["status"] = "in_progress"
        task["updated_at"] = time.time()
        self._save(task)
        return json.dumps(task, ensure_ascii=False, indent=2)

    def unbind_worktree(self, task_id: int) -> str:
        task = self._load(task_id)
        task["worktree"] = ""
        task["updated_at"] = time.time()
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
            owner = f" owner={t.get('owner')}" if t.get("owner") else ""
            wt = f" wt={t.get('worktree')}" if t.get("worktree") else ""
            lines.append(f"{m} #{t.get('id')}: {t.get('subject', '')}{owner}{wt}")
        return "\n".join(lines)


class WorktreeManager:
    def __init__(self, repo_root: Path, tasks: TaskManager, events: EventBus):
        self.repo_root = repo_root
        self.tasks = tasks
        self.events = events
        self.dir = repo_root / ".worktrees"
        self.dir.mkdir(parents=True, exist_ok=True)
        self.index_path = self.dir / "index.json"
        if not self.index_path.exists():
            self.index_path.write_text(json.dumps({"worktrees": []}, indent=2), encoding="utf-8")
        self.git_available = self._is_git_repo()

    def _is_git_repo(self) -> bool:
        try:
            r = subprocess.run(
                ["git", "rev-parse", "--is-inside-work-tree"],
                cwd=self.repo_root,
                capture_output=True,
                text=True,
                timeout=10,
            )
            return r.returncode == 0
        except Exception:
            return False

    def _run_git(self, args: list[str]) -> str:
        if not self.git_available:
            raise RuntimeError("Not in a git repository. worktree tools require git.")
        r = subprocess.run(
            ["git", *args],
            cwd=self.repo_root,
            capture_output=True,
            text=True,
            timeout=120,
        )
        if r.returncode != 0:
            msg = (r.stdout + r.stderr).strip()
            raise RuntimeError(msg or f"git {' '.join(args)} failed")
        return (r.stdout + r.stderr).strip() or "(no output)"

    def _load_index(self) -> dict[str, Any]:
        return json.loads(self.index_path.read_text(encoding="utf-8"))

    def _save_index(self, data: dict[str, Any]) -> None:
        self.index_path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")

    def _find(self, name: str) -> dict[str, Any] | None:
        idx = self._load_index()
        for wt in idx.get("worktrees", []):
            if wt.get("name") == name:
                return wt
        return None

    def _validate_name(self, name: str) -> None:
        if not re.fullmatch(r"[A-Za-z0-9._-]{1,40}", name or ""):
            raise ValueError("Invalid worktree name. Use 1-40 chars: letters, numbers, ., _, -")

    def create(self, name: str, task_id: int | None = None, base_ref: str = "HEAD") -> str:
        nm = str(name)
        self._validate_name(nm)
        if self._find(nm):
            raise ValueError(f"Worktree '{nm}' already exists in index")
        if task_id is not None and not self.tasks.exists(int(task_id)):
            raise ValueError(f"Task {task_id} not found")

        path = self.dir / nm
        branch = f"wt/{nm}"
        task_obj = {"id": int(task_id)} if task_id is not None else {}
        self.events.emit(
            "worktree.create.before",
            task=task_obj,
            worktree={"name": nm, "base_ref": base_ref},
        )
        try:
            self._run_git(["worktree", "add", "-b", branch, str(path), str(base_ref)])
            entry = {
                "name": nm,
                "path": str(path),
                "branch": branch,
                "task_id": int(task_id) if task_id is not None else None,
                "status": "active",
                "created_at": time.time(),
            }
            idx = self._load_index()
            idx.setdefault("worktrees", []).append(entry)
            self._save_index(idx)

            if task_id is not None:
                self.tasks.bind_worktree(int(task_id), nm)

            self.events.emit(
                "worktree.create.after",
                task=task_obj,
                worktree={"name": nm, "path": str(path), "branch": branch, "status": "active"},
            )
            return json.dumps(entry, ensure_ascii=False, indent=2)
        except Exception as e:
            self.events.emit(
                "worktree.create.failed",
                task=task_obj,
                worktree={"name": nm, "base_ref": base_ref},
                error=str(e),
            )
            raise

    def list_all(self) -> str:
        idx = self._load_index()
        wts = idx.get("worktrees", [])
        if not wts:
            return "No worktrees in index."
        lines: list[str] = []
        for wt in wts:
            task = wt.get("task_id")
            suffix = f" task={task}" if task is not None else ""
            lines.append(
                f"[{wt.get('status', 'unknown')}] {wt.get('name')} -> "
                f"{wt.get('path')} ({wt.get('branch', '-')}){suffix}"
            )
        return "\n".join(lines)

    def status(self, name: str) -> str:
        wt = self._find(str(name))
        if not wt:
            return f"Error: Unknown worktree '{name}'"
        path = Path(str(wt.get("path", "")))
        if not path.exists():
            return f"Error: Worktree path missing: {path}"
        r = subprocess.run(
            ["git", "status", "--short", "--branch"],
            cwd=path,
            capture_output=True,
            text=True,
            timeout=60,
        )
        text = (r.stdout + r.stderr).strip()
        return text or "Clean worktree"

    def run(self, name: str, command: str) -> str:
        dangerous = ["rm -rf /", "sudo", "shutdown", "reboot", "> /dev/"]
        if any(d in command for d in dangerous):
            return "Error: dangerous command blocked"

        wt = self._find(str(name))
        if not wt:
            return f"Error: Unknown worktree '{name}'"
        path = Path(str(wt.get("path", "")))
        if not path.exists():
            return f"Error: Worktree path missing: {path}"

        try:
            r = subprocess.run(
                command,
                shell=True,
                cwd=path,
                capture_output=True,
                text=True,
                timeout=300,
            )
            out = (r.stdout + r.stderr).strip()
            return out[:50000] if out else "(no output)"
        except subprocess.TimeoutExpired:
            return "Error: timeout (300s)"

    def keep(self, name: str) -> str:
        nm = str(name)
        wt = self._find(nm)
        if not wt:
            return f"Error: Unknown worktree '{nm}'"

        idx = self._load_index()
        kept: dict[str, Any] | None = None
        for item in idx.get("worktrees", []):
            if item.get("name") == nm:
                item["status"] = "kept"
                item["kept_at"] = time.time()
                kept = item
                break
        self._save_index(idx)

        task_id = wt.get("task_id")
        task_obj = {"id": task_id} if task_id is not None else {}
        self.events.emit(
            "worktree.keep",
            task=task_obj,
            worktree={"name": nm, "path": wt.get("path"), "status": "kept"},
        )
        return json.dumps(kept, ensure_ascii=False, indent=2) if kept else f"Error: Unknown worktree '{nm}'"

    def remove(self, name: str, force: bool = False, complete_task: bool = False) -> str:
        nm = str(name)
        wt = self._find(nm)
        if not wt:
            return f"Error: Unknown worktree '{nm}'"

        task_id = wt.get("task_id")
        task_obj = {"id": task_id} if task_id is not None else {}
        self.events.emit(
            "worktree.remove.before",
            task=task_obj,
            worktree={"name": nm, "path": wt.get("path")},
        )
        try:
            args = ["worktree", "remove"]
            if force:
                args.append("--force")
            args.append(str(wt.get("path")))
            self._run_git(args)

            if complete_task and task_id is not None:
                before = json.loads(self.tasks.get(int(task_id)))
                self.tasks.update(int(task_id), status="completed")
                self.tasks.unbind_worktree(int(task_id))
                self.events.emit(
                    "task.completed",
                    task={
                        "id": int(task_id),
                        "subject": before.get("subject", ""),
                        "status": "completed",
                    },
                    worktree={"name": nm},
                )

            idx = self._load_index()
            for item in idx.get("worktrees", []):
                if item.get("name") == nm:
                    item["status"] = "removed"
                    item["removed_at"] = time.time()
            self._save_index(idx)

            self.events.emit(
                "worktree.remove.after",
                task=task_obj,
                worktree={"name": nm, "path": wt.get("path"), "status": "removed"},
            )
            return f"Removed worktree '{nm}'"
        except Exception as e:
            self.events.emit(
                "worktree.remove.failed",
                task=task_obj,
                worktree={"name": nm, "path": wt.get("path")},
                error=str(e),
            )
            raise


TASKS = TaskManager(REPO_ROOT / ".tasks")
EVENTS = EventBus(REPO_ROOT / ".worktrees" / "events.jsonl")
WORKTREES = WorktreeManager(REPO_ROOT, TASKS, EVENTS)


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
    handlers["task_list"] = lambda **kw: TASKS.list_all()
    handlers["task_get"] = lambda **kw: TASKS.get(int(kw["task_id"]))
    handlers["task_update"] = lambda **kw: TASKS.update(int(kw["task_id"]), kw.get("status"), kw.get("owner"))
    handlers["task_bind_worktree"] = (
        lambda **kw: TASKS.bind_worktree(int(kw["task_id"]), kw["worktree"], kw.get("owner", ""))
    )

    handlers["worktree_create"] = (
        lambda **kw: WORKTREES.create(kw["name"], kw.get("task_id"), kw.get("base_ref", "HEAD"))
    )
    handlers["worktree_list"] = lambda **kw: WORKTREES.list_all()
    handlers["worktree_status"] = lambda **kw: WORKTREES.status(kw["name"])
    handlers["worktree_run"] = lambda **kw: WORKTREES.run(kw["name"], kw["command"])
    handlers["worktree_keep"] = lambda **kw: WORKTREES.keep(kw["name"])
    handlers["worktree_remove"] = (
        lambda **kw: WORKTREES.remove(kw["name"], kw.get("force", False), kw.get("complete_task", False))
    )
    handlers["worktree_events"] = lambda **kw: EVENTS.list_recent(int(kw.get("limit", 20)))

    tools = tool_specs_base() + [
        {
            "name": "task_create",
            "description": "Create a new task on board.",
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
            "name": "task_list",
            "description": "List tasks with owner/worktree.",
            "input_schema": {"type": "object", "properties": {}},
        },
        {
            "name": "task_get",
            "description": "Get task details by id.",
            "input_schema": {
                "type": "object",
                "properties": {"task_id": {"type": "integer"}},
                "required": ["task_id"],
            },
        },
        {
            "name": "task_update",
            "description": "Update task status or owner.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "task_id": {"type": "integer"},
                    "status": {
                        "type": "string",
                        "enum": ["pending", "in_progress", "completed"],
                    },
                    "owner": {"type": "string"},
                },
                "required": ["task_id"],
            },
        },
        {
            "name": "task_bind_worktree",
            "description": "Bind task to worktree name.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "task_id": {"type": "integer"},
                    "worktree": {"type": "string"},
                    "owner": {"type": "string"},
                },
                "required": ["task_id", "worktree"],
            },
        },
        {
            "name": "worktree_create",
            "description": "Create git worktree and optional task binding.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "name": {"type": "string"},
                    "task_id": {"type": "integer"},
                    "base_ref": {"type": "string"},
                },
                "required": ["name"],
            },
        },
        {
            "name": "worktree_list",
            "description": "List indexed worktrees.",
            "input_schema": {"type": "object", "properties": {}},
        },
        {
            "name": "worktree_status",
            "description": "Show git status for a worktree.",
            "input_schema": {
                "type": "object",
                "properties": {"name": {"type": "string"}},
                "required": ["name"],
            },
        },
        {
            "name": "worktree_run",
            "description": "Run shell command in a worktree directory.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "name": {"type": "string"},
                    "command": {"type": "string"},
                },
                "required": ["name", "command"],
            },
        },
        {
            "name": "worktree_keep",
            "description": "Mark worktree as kept without removing it.",
            "input_schema": {
                "type": "object",
                "properties": {"name": {"type": "string"}},
                "required": ["name"],
            },
        },
        {
            "name": "worktree_remove",
            "description": "Remove worktree; optionally complete bound task.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "name": {"type": "string"},
                    "force": {"type": "boolean"},
                    "complete_task": {"type": "boolean"},
                },
                "required": ["name"],
            },
        },
        {
            "name": "worktree_events",
            "description": "List recent lifecycle events.",
            "input_schema": {
                "type": "object",
                "properties": {"limit": {"type": "integer"}},
            },
        },
    ]

    history: list[dict[str, Any]] = []
    print(f"Repo root for s12: {REPO_ROOT}")
    if not WORKTREES.git_available:
        print("Note: not in a git repo. worktree_* tools will return errors.")

    while True:
        try:
            q = input("\033[36ms12-fxsh >> \033[0m")
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

            results = []
            for block in response.content:
                if _block_type(block) != "tool_use":
                    continue
                name = _block_name(block)
                inp = _block_input(block)
                h = handlers.get(name)
                try:
                    out = h(**inp) if h else f"Unknown tool: {name}"
                except Exception as e:
                    out = f"Error: {e}"
                print(f"> {name}: {str(out)[:200]}")
                results.append({"type": "tool_result", "tool_use_id": _block_id(block), "content": str(out)})
            history.append({"role": "user", "content": results})


if __name__ == "__main__":
    main()
