#!/usr/bin/env python3
"""s09: persistent agent teams + JSONL inbox messaging."""

from __future__ import annotations

import json
import os
import threading
import time
from pathlib import Path
from typing import Any

from common import create_client, extract_text_blocks, handlers_base, tool_specs_base

WORKDIR = Path.cwd()
TEAM_DIR = WORKDIR / ".team"
INBOX_DIR = TEAM_DIR / "inbox"

SYSTEM = f"""You are a team lead agent at {WORKDIR}.
Spawn persistent teammates and coordinate by inbox messaging."""

VALID_MSG_TYPES = {
    "message",
    "broadcast",
    "shutdown_request",
    "shutdown_response",
    "plan_approval_response",
}


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
            ]
        )
        return tools

    def _teammate_exec(
        self, sender: str, name: str, inp: dict[str, Any], base_handlers: dict[str, Any]
    ) -> str:
        if name in base_handlers:
            return str(base_handlers[name](**inp))
        if name == "send_message":
            return BUS.send(sender, inp["to"], inp["content"], inp.get("msg_type", "message"))
        if name == "read_inbox":
            return json.dumps(BUS.read_inbox(sender), ensure_ascii=False, indent=2)
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
            f"Work in {WORKDIR}. Use send_message/read_inbox to coordinate."
        )
        messages: list[dict[str, Any]] = [{"role": "user", "content": prompt}]
        tools = self._teammate_tools()

        for _ in range(50):
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

            messages.append({"role": "user", "content": results})

        self._set_status(name, "idle")

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
        {
            "name": "list_teammates",
            "description": "List teammate roster and status.",
            "input_schema": {"type": "object", "properties": {}},
        },
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
            "description": "Read and drain lead inbox.",
            "input_schema": {"type": "object", "properties": {}},
        },
        {
            "name": "broadcast",
            "description": "Broadcast to all teammates.",
            "input_schema": {
                "type": "object",
                "properties": {"content": {"type": "string"}},
                "required": ["content"],
            },
        },
    ]

    history: list[dict[str, Any]] = []

    while True:
        try:
            q = input("\033[36ms09-fxsh >> \033[0m")
        except (EOFError, KeyboardInterrupt):
            break
        if q.strip().lower() in ("q", "quit", "exit", ""):
            break
        history.append({"role": "user", "content": q})

        while True:
            inbox = BUS.read_inbox("lead")
            if inbox:
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
