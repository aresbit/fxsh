#!/usr/bin/env python3
"""s11: autonomous teammates (idle polling + auto-claim + identity reinjection)."""

from __future__ import annotations

import json
import os
import threading
import time
import uuid
from pathlib import Path
from typing import Any

from common import create_client, extract_text_blocks, handlers_base, tool_specs_base

WORKDIR = Path.cwd()
TEAM_DIR = WORKDIR / ".team"
INBOX_DIR = TEAM_DIR / "inbox"
TASKS_DIR = WORKDIR / ".tasks"

POLL_INTERVAL = 5
IDLE_TIMEOUT = 60

SYSTEM = f"""You are a team lead agent at {WORKDIR}.
Teammates are autonomous: they can idle, poll, and auto-claim unassigned tasks."""

VALID_MSG_TYPES = {
    "message",
    "broadcast",
    "shutdown_request",
    "shutdown_response",
    "plan_approval_response",
}

shutdown_requests: dict[str, dict[str, Any]] = {}
plan_requests: dict[str, dict[str, Any]] = {}
_tracker_lock = threading.Lock()
_claim_lock = threading.Lock()


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
        task = {
            "id": self._next_id,
            "subject": str(subject).strip(),
            "description": str(description),
            "status": "pending",
            "blockedBy": [],
            "blocks": [],
            "owner": "",
        }
        if not task["subject"]:
            raise ValueError("subject is required")
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
                    bt = self._load(bid)
                except ValueError:
                    continue
                if tid not in bt.get("blockedBy", []):
                    bt["blockedBy"] = sorted(bt.get("blockedBy", []) + [tid])
                    self._save(bt)

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
            owner = t.get("owner")
            suffix = ""
            if blocked:
                suffix += f" blockedBy={blocked}"
            if owner:
                suffix += f" owner={owner}"
            lines.append(f"{m} #{t.get('id')}: {t.get('subject', '')}{(' (' + suffix.strip() + ')') if suffix else ''}")
        return "\n".join(lines)


TASKS = TaskManager(TASKS_DIR)


def scan_unclaimed_tasks() -> list[dict[str, Any]]:
    TASKS_DIR.mkdir(exist_ok=True)
    out: list[dict[str, Any]] = []
    for f in sorted(TASKS_DIR.glob("task_*.json"), key=lambda p: p.name):
        try:
            t = json.loads(f.read_text(encoding="utf-8"))
        except Exception:
            continue
        if t.get("status") == "pending" and not t.get("owner") and not t.get("blockedBy"):
            out.append(t)
    return out


def claim_task(task_id: int, owner: str) -> str:
    with _claim_lock:
        p = TASKS_DIR / f"task_{int(task_id)}.json"
        if not p.exists():
            return f"Error: task {task_id} not found"
        t = json.loads(p.read_text(encoding="utf-8"))
        if t.get("owner") and t.get("owner") != owner:
            return f"Error: task {task_id} already owned by {t.get('owner')}"
        t["owner"] = owner
        if t.get("status") == "pending":
            t["status"] = "in_progress"
        p.write_text(json.dumps(t, ensure_ascii=False, indent=2), encoding="utf-8")
    return f"Claimed task #{task_id} for {owner}"


class MessageBus:
    def __init__(self, inbox_dir: Path):
        self.dir = inbox_dir
        self.dir.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()

    def send(
        self,
        sender: str,
        to: str,
        content: str,
        msg_type: str = "message",
        extra: dict[str, Any] | None = None,
    ) -> str:
        if msg_type not in VALID_MSG_TYPES:
            return f"Error: invalid msg_type '{msg_type}'"
        msg = {
            "type": msg_type,
            "from": sender,
            "content": content,
            "timestamp": time.time(),
        }
        if extra:
            msg.update(extra)
        p = self.dir / f"{to}.jsonl"
        with self._lock:
            with p.open("a", encoding="utf-8") as f:
                f.write(json.dumps(msg, ensure_ascii=False) + "\n")
        return f"Sent {msg_type} to {to}"

    def read_inbox(self, name: str) -> list[dict[str, Any]]:
        p = self.dir / f"{name}.jsonl"
        if not p.exists():
            return []
        with self._lock:
            lines = p.read_text(encoding="utf-8").splitlines()
            p.write_text("", encoding="utf-8")
        out: list[dict[str, Any]] = []
        for line in lines:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except Exception:
                continue
        return out

    def broadcast(self, sender: str, content: str, members: list[str]) -> str:
        cnt = 0
        for m in members:
            if m == sender:
                continue
            self.send(sender, m, content, "broadcast")
            cnt += 1
        return f"Broadcast to {cnt} teammates"


BUS = MessageBus(INBOX_DIR)


def _identity_block(name: str, role: str, team_name: str) -> dict[str, str]:
    return {
        "role": "user",
        "content": f"<identity>You are '{name}', role: {role}, team: {team_name}. Continue your work.</identity>",
    }


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


def _handle_shutdown_request(teammate: str) -> str:
    req_id = str(uuid.uuid4())[:8]
    with _tracker_lock:
        shutdown_requests[req_id] = {"target": teammate, "status": "pending"}
    BUS.send("lead", teammate, "Please shutdown gracefully.", "shutdown_request", {"request_id": req_id})
    return f"Shutdown request {req_id} sent to {teammate}"


def _check_shutdown_status(request_id: str) -> str:
    with _tracker_lock:
        req = shutdown_requests.get(request_id)
    return json.dumps(req if req else {"error": "not found"}, ensure_ascii=False)


def _handle_plan_review(request_id: str, approve: bool, feedback: str = "") -> str:
    with _tracker_lock:
        req = plan_requests.get(request_id)
        if not req:
            return f"Error: unknown plan request {request_id}"
        req["status"] = "approved" if approve else "rejected"
        target = req["from"]
    BUS.send(
        "lead",
        target,
        feedback,
        "plan_approval_response",
        {"request_id": request_id, "approve": approve, "feedback": feedback},
    )
    return f"Plan {request_id} {'approved' if approve else 'rejected'} for {target}"


def _process_lead_inbox(inbox: list[dict[str, Any]]) -> None:
    for msg in inbox:
        mtype = str(msg.get("type", ""))
        req_id = str(msg.get("request_id", ""))
        if mtype == "shutdown_response" and req_id:
            with _tracker_lock:
                req = shutdown_requests.get(req_id)
                if req:
                    req["status"] = "approved" if msg.get("approve") else "rejected"
        if mtype == "plan_approval_response" and req_id:
            with _tracker_lock:
                req = plan_requests.get(req_id)
                if not req:
                    plan_requests[req_id] = {
                        "from": msg.get("from", "unknown"),
                        "plan": msg.get("plan", msg.get("content", "")),
                        "status": "pending",
                    }


class TeammateManager:
    def __init__(self, team_dir: Path):
        self.dir = team_dir
        self.dir.mkdir(exist_ok=True)
        self.config_path = self.dir / "config.json"
        self.config = self._load_config()
        self.threads: dict[str, threading.Thread] = {}
        self._lock = threading.Lock()

    def _load_config(self) -> dict[str, Any]:
        if self.config_path.exists():
            try:
                return json.loads(self.config_path.read_text(encoding="utf-8"))
            except Exception:
                pass
        return {"team_name": "default", "members": []}

    def _save_config(self) -> None:
        with self._lock:
            self.config_path.write_text(json.dumps(self.config, ensure_ascii=False, indent=2), encoding="utf-8")

    def _find_member(self, name: str) -> dict[str, Any] | None:
        for m in self.config["members"]:
            if m.get("name") == name:
                return m
        return None

    def _set_status(self, name: str, status: str) -> None:
        m = self._find_member(name)
        if m:
            m["status"] = status
            self._save_config()

    def spawn(self, name: str, role: str, prompt: str) -> str:
        name = str(name).strip()
        role = str(role).strip() or "worker"
        if not name:
            return "Error: name required"

        m = self._find_member(name)
        if m:
            if m.get("status") not in ("idle", "shutdown"):
                return f"Error: {name} is {m.get('status')}"
            m["role"] = role
            m["status"] = "working"
        else:
            self.config["members"].append({"name": name, "role": role, "status": "working"})
        self._save_config()

        th = threading.Thread(target=self._teammate_loop, args=(name, role, prompt), daemon=True)
        self.threads[name] = th
        th.start()
        return f"Spawned {name} ({role})"

    def _teammate_tools(self) -> list[dict[str, Any]]:
        tools = [
            t
            for t in tool_specs_base()
            if t["name"] in {"bash", "read_file", "write_file", "edit_file"}
        ]
        tools.extend(
            [
                {
                    "name": "send_message",
                    "description": "Send message to teammate inbox.",
                    "input_schema": {
                        "type": "object",
                        "properties": {
                            "to": {"type": "string"},
                            "content": {"type": "string"},
                            "msg_type": {"type": "string", "enum": sorted(list(VALID_MSG_TYPES))},
                        },
                        "required": ["to", "content"],
                    },
                },
                {"name": "read_inbox", "description": "Read and drain your inbox.", "input_schema": {"type": "object", "properties": {}}},
                {
                    "name": "shutdown_response",
                    "description": "Respond to shutdown request.",
                    "input_schema": {
                        "type": "object",
                        "properties": {
                            "request_id": {"type": "string"},
                            "approve": {"type": "boolean"},
                            "reason": {"type": "string"},
                        },
                        "required": ["request_id", "approve"],
                    },
                },
                {
                    "name": "plan_approval",
                    "description": "Submit plan for lead approval.",
                    "input_schema": {
                        "type": "object",
                        "properties": {"plan": {"type": "string"}},
                        "required": ["plan"],
                    },
                },
                {
                    "name": "idle",
                    "description": "Enter idle polling phase.",
                    "input_schema": {"type": "object", "properties": {}},
                },
                {
                    "name": "claim_task",
                    "description": "Claim task by id.",
                    "input_schema": {
                        "type": "object",
                        "properties": {"task_id": {"type": "integer"}},
                        "required": ["task_id"],
                    },
                },
            ]
        )
        return tools

    def _teammate_exec(self, sender: str, name: str, inp: dict[str, Any], base_handlers: dict[str, Any]) -> str:
        if name in base_handlers:
            return str(base_handlers[name](**inp))
        if name == "send_message":
            return BUS.send(sender, inp["to"], inp["content"], inp.get("msg_type", "message"))
        if name == "read_inbox":
            return json.dumps(BUS.read_inbox(sender), ensure_ascii=False, indent=2)
        if name == "shutdown_response":
            req_id = str(inp["request_id"])
            approve = bool(inp["approve"])
            with _tracker_lock:
                req = shutdown_requests.get(req_id)
                if req:
                    req["status"] = "approved" if approve else "rejected"
            BUS.send(sender, "lead", inp.get("reason", ""), "shutdown_response", {"request_id": req_id, "approve": approve})
            return f"Shutdown {'approved' if approve else 'rejected'}"
        if name == "plan_approval":
            plan = str(inp.get("plan", "")).strip()
            if not plan:
                return "Error: plan required"
            req_id = str(uuid.uuid4())[:8]
            with _tracker_lock:
                plan_requests[req_id] = {"from": sender, "plan": plan, "status": "pending"}
            BUS.send(sender, "lead", plan, "plan_approval_response", {"request_id": req_id, "plan": plan})
            return f"Plan submitted request_id={req_id}"
        if name == "claim_task":
            return claim_task(int(inp["task_id"]), sender)
        return f"Unknown tool: {name}"

    def _teammate_loop(self, name: str, role: str, prompt: str) -> None:
        client = create_client()
        model = os.getenv("MODEL_ID", "")
        if not model:
            self._set_status(name, "idle")
            return

        base_handlers = handlers_base()
        base_handlers = {k: v for k, v in base_handlers.items() if k in {"bash", "read_file", "write_file", "edit_file"}}

        team_name = self.config.get("team_name", "default")
        sys_prompt = (
            f"You are '{name}', role: {role}, team: {team_name}. "
            f"Use idle when no immediate work. You may auto-claim tasks."
        )
        messages: list[dict[str, Any]] = [{"role": "user", "content": prompt}]
        tools = self._teammate_tools()

        while True:
            # WORK PHASE
            for _ in range(60):
                inbox = BUS.read_inbox(name)
                if inbox:
                    messages.append({"role": "user", "content": f"<inbox>{json.dumps(inbox, ensure_ascii=False)}</inbox>"})
                    messages.append({"role": "assistant", "content": "Noted inbox messages."})

                response = client.messages.create(
                    model=model,
                    system=sys_prompt,
                    messages=messages,
                    tools=tools,
                    max_tokens=4000,
                )
                messages.append({"role": "assistant", "content": response.content})

                if response.stop_reason != "tool_use":
                    break

                results = []
                idle_requested = False
                shutdown_now = False
                for block in response.content:
                    if _block_type(block) != "tool_use":
                        continue
                    tname = _block_name(block)
                    tinp = _block_input(block)
                    if tname == "idle":
                        out = "Entering idle polling phase."
                        idle_requested = True
                    else:
                        try:
                            out = self._teammate_exec(name, tname, tinp, base_handlers)
                        except Exception as e:
                            out = f"Error: {e}"
                    print(f"  [{name}] {tname}: {str(out)[:120]}")
                    results.append({"type": "tool_result", "tool_use_id": _block_id(block), "content": str(out)})
                    if tname == "shutdown_response" and bool(tinp.get("approve")):
                        shutdown_now = True

                messages.append({"role": "user", "content": results})
                if shutdown_now:
                    self._set_status(name, "shutdown")
                    return
                if idle_requested:
                    break

            # IDLE PHASE
            self._set_status(name, "idle")
            resumed = False
            polls = max(1, IDLE_TIMEOUT // max(POLL_INTERVAL, 1))
            for _ in range(polls):
                time.sleep(POLL_INTERVAL)

                inbox = BUS.read_inbox(name)
                if inbox:
                    for msg in inbox:
                        if msg.get("type") == "shutdown_request":
                            self._set_status(name, "shutdown")
                            return
                    messages.append({"role": "user", "content": f"<inbox>{json.dumps(inbox, ensure_ascii=False)}</inbox>"})
                    messages.append({"role": "assistant", "content": "Inbox received. Resuming work."})
                    resumed = True
                    break

                unclaimed = scan_unclaimed_tasks()
                if unclaimed:
                    task = unclaimed[0]
                    claim_task(int(task["id"]), name)
                    if len(messages) <= 3:
                        messages.insert(0, _identity_block(name, role, team_name))
                        messages.insert(1, {"role": "assistant", "content": f"I am {name}. Continuing."})
                    messages.append(
                        {
                            "role": "user",
                            "content": f"<auto-claimed>Task #{task['id']}: {task.get('subject','')}\n{task.get('description','')}</auto-claimed>",
                        }
                    )
                    messages.append({"role": "assistant", "content": f"Claimed task #{task['id']}. Working on it."})
                    resumed = True
                    break

            if not resumed:
                self._set_status(name, "shutdown")
                return
            self._set_status(name, "working")

    def list_all(self) -> str:
        ms = self.config.get("members", [])
        if not ms:
            return "No teammates."
        lines = [f"Team: {self.config.get('team_name','default')}"]
        for m in ms:
            lines.append(f"  {m.get('name')} ({m.get('role')}): {m.get('status')}")
        return "\n".join(lines)

    def member_names(self) -> list[str]:
        return [str(m.get("name")) for m in self.config.get("members", []) if m.get("name")]


TEAM = TeammateManager(TEAM_DIR)


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

    handlers["spawn_teammate"] = lambda **kw: TEAM.spawn(kw["name"], kw["role"], kw["prompt"])
    handlers["list_teammates"] = lambda **kw: TEAM.list_all()
    handlers["send_message"] = lambda **kw: BUS.send("lead", kw["to"], kw["content"], kw.get("msg_type", "message"))
    handlers["read_inbox"] = lambda **kw: json.dumps(BUS.read_inbox("lead"), ensure_ascii=False, indent=2)
    handlers["broadcast"] = lambda **kw: BUS.broadcast("lead", kw["content"], TEAM.member_names())

    handlers["shutdown_request"] = lambda **kw: _handle_shutdown_request(kw["teammate"])
    handlers["shutdown_response"] = lambda **kw: _check_shutdown_status(kw["request_id"])
    handlers["plan_approval"] = lambda **kw: _handle_plan_review(kw["request_id"], kw["approve"], kw.get("feedback", ""))

    tools = tool_specs_base() + [
        {
            "name": "task_create",
            "description": "Create persisted task.",
            "input_schema": {
                "type": "object",
                "properties": {"subject": {"type": "string"}, "description": {"type": "string"}},
                "required": ["subject"],
            },
        },
        {
            "name": "task_update",
            "description": "Update task status or deps.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "task_id": {"type": "integer"},
                    "status": {"type": "string", "enum": ["pending", "in_progress", "completed"]},
                    "addBlockedBy": {"type": "array", "items": {"type": "integer"}},
                    "addBlocks": {"type": "array", "items": {"type": "integer"}},
                },
                "required": ["task_id"],
            },
        },
        {"name": "task_list", "description": "List task board.", "input_schema": {"type": "object", "properties": {}}},
        {
            "name": "task_get",
            "description": "Get task json by id.",
            "input_schema": {"type": "object", "properties": {"task_id": {"type": "integer"}}, "required": ["task_id"]},
        },
        {
            "name": "spawn_teammate",
            "description": "Spawn autonomous teammate.",
            "input_schema": {
                "type": "object",
                "properties": {"name": {"type": "string"}, "role": {"type": "string"}, "prompt": {"type": "string"}},
                "required": ["name", "role", "prompt"],
            },
        },
        {"name": "list_teammates", "description": "List teammate roster/status.", "input_schema": {"type": "object", "properties": {}}},
        {
            "name": "send_message",
            "description": "Send message to teammate.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "to": {"type": "string"},
                    "content": {"type": "string"},
                    "msg_type": {"type": "string", "enum": sorted(list(VALID_MSG_TYPES))},
                },
                "required": ["to", "content"],
            },
        },
        {"name": "read_inbox", "description": "Read/drain lead inbox.", "input_schema": {"type": "object", "properties": {}}},
        {
            "name": "broadcast",
            "description": "Broadcast message to all teammates.",
            "input_schema": {"type": "object", "properties": {"content": {"type": "string"}}, "required": ["content"]},
        },
        {
            "name": "shutdown_request",
            "description": "Request teammate shutdown.",
            "input_schema": {"type": "object", "properties": {"teammate": {"type": "string"}}, "required": ["teammate"]},
        },
        {
            "name": "shutdown_response",
            "description": "Check shutdown request status.",
            "input_schema": {"type": "object", "properties": {"request_id": {"type": "string"}}, "required": ["request_id"]},
        },
        {
            "name": "plan_approval",
            "description": "Approve/reject plan request.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "request_id": {"type": "string"},
                    "approve": {"type": "boolean"},
                    "feedback": {"type": "string"},
                },
                "required": ["request_id", "approve"],
            },
        },
    ]

    history: list[dict[str, Any]] = []

    while True:
        try:
            q = input("\033[36ms11-fxsh >> \033[0m")
        except (EOFError, KeyboardInterrupt):
            break
        if q.strip().lower() in ("q", "quit", "exit", ""):
            break
        history.append({"role": "user", "content": q})

        while True:
            inbox = BUS.read_inbox("lead")
            if inbox:
                _process_lead_inbox(inbox)
                history.append({"role": "user", "content": f"<inbox>{json.dumps(inbox, ensure_ascii=False)}</inbox>"})
                history.append({"role": "assistant", "content": "Noted inbox messages."})

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
                n = _block_name(block)
                inp = _block_input(block)
                h = handlers.get(n)
                try:
                    out = h(**inp) if h else f"Unknown tool: {n}"
                except Exception as e:
                    out = f"Error: {e}"
                print(f"> {n}: {str(out)[:200]}")
                results.append({"type": "tool_result", "tool_use_id": _block_id(block), "content": str(out)})
            history.append({"role": "user", "content": results})


if __name__ == "__main__":
    main()
