#!/usr/bin/env python3
"""s10: team protocols (shutdown + plan approval request/response)."""

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

SYSTEM = f"""You are a team lead agent at {WORKDIR}.
Manage teammates using request/response protocols:
- shutdown_request -> shutdown_response
- plan_approval_response -> plan_approval review"""

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
                {
                    "name": "read_inbox",
                    "description": "Read and drain your inbox.",
                    "input_schema": {"type": "object", "properties": {}},
                },
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
        return f"Unknown tool: {name}"

    def _teammate_loop(self, name: str, role: str, prompt: str) -> None:
        client = create_client()
        model = os.getenv("MODEL_ID", "")
        if not model:
            self._set_status(name, "idle")
            return

        base_handlers = handlers_base()
        base_handlers = {k: v for k, v in base_handlers.items() if k in {"bash", "read_file", "write_file", "edit_file"}}

        sys_prompt = (
            f"You are '{name}', role: {role}, in team '{self.config.get('team_name','default')}'. "
            f"Respond to shutdown_request via shutdown_response; submit major plans via plan_approval first."
        )
        messages: list[dict[str, Any]] = [{"role": "user", "content": prompt}]
        tools = self._teammate_tools()

        should_shutdown = False
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
            for block in response.content:
                if _block_type(block) != "tool_use":
                    continue
                tname = _block_name(block)
                tinp = _block_input(block)
                try:
                    out = self._teammate_exec(name, tname, tinp, base_handlers)
                except Exception as e:
                    out = f"Error: {e}"
                print(f"  [{name}] {tname}: {str(out)[:120]}")
                results.append({"type": "tool_result", "tool_use_id": _block_id(block), "content": str(out)})
                if tname == "shutdown_response" and bool(tinp.get("approve")):
                    should_shutdown = True

            messages.append({"role": "user", "content": results})
            if should_shutdown:
                break

        self._set_status(name, "shutdown" if should_shutdown else "idle")

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
            "name": "spawn_teammate",
            "description": "Spawn persistent teammate in background thread.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "name": {"type": "string"},
                    "role": {"type": "string"},
                    "prompt": {"type": "string"},
                },
                "required": ["name", "role", "prompt"],
            },
        },
        {"name": "list_teammates", "description": "List teammate roster and status.", "input_schema": {"type": "object", "properties": {}}},
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
        {"name": "read_inbox", "description": "Read and drain lead inbox.", "input_schema": {"type": "object", "properties": {}}},
        {
            "name": "broadcast",
            "description": "Broadcast to all teammates.",
            "input_schema": {"type": "object", "properties": {"content": {"type": "string"}}, "required": ["content"]},
        },
        {
            "name": "shutdown_request",
            "description": "Request teammate graceful shutdown.",
            "input_schema": {"type": "object", "properties": {"teammate": {"type": "string"}}, "required": ["teammate"]},
        },
        {
            "name": "shutdown_response",
            "description": "Check shutdown request status by request_id.",
            "input_schema": {"type": "object", "properties": {"request_id": {"type": "string"}}, "required": ["request_id"]},
        },
        {
            "name": "plan_approval",
            "description": "Approve/reject teammate plan request.",
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
            q = input("\033[36ms10-fxsh >> \033[0m")
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
