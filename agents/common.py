#!/usr/bin/env python3
"""Shared runtime for fxsh agent stages s01-s05."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path
from typing import Any, Callable

from anthropic import Anthropic
from dotenv import load_dotenv

load_dotenv(override=True)

WORKDIR = Path.cwd()
MODEL = os.getenv("MODEL_ID", "")


def create_client() -> Anthropic:
    if os.getenv("ANTHROPIC_BASE_URL"):
        os.environ.pop("ANTHROPIC_AUTH_TOKEN", None)
    return Anthropic(base_url=os.getenv("ANTHROPIC_BASE_URL"))


def safe_path(p: str) -> Path:
    path = (WORKDIR / p).resolve()
    if not path.is_relative_to(WORKDIR):
        raise ValueError(f"Path escapes workspace: {p}")
    return path


def run_shell(command: str) -> str:
    dangerous = ["rm -rf /", "sudo", "shutdown", "reboot", "> /dev/"]
    if any(d in command for d in dangerous):
        return "Error: dangerous command blocked"
    try:
        r = subprocess.run(
            command,
            shell=True,
            cwd=WORKDIR,
            capture_output=True,
            text=True,
            timeout=120,
        )
        out = (r.stdout + r.stderr).strip()
        return out[:50000] if out else "(no output)"
    except subprocess.TimeoutExpired:
        return "Error: timeout (120s)"


def run_read(path: str, limit: int | None = None) -> str:
    try:
        text = run_fxsh_bridge("read_file", {"FXSH_TOOL_PATH": path})
        if text.startswith("Error:"):
            return text
        lines = text.splitlines()
        if limit and limit < len(lines):
            lines = lines[:limit] + [f"... ({len(lines) - limit} more lines)"]
        return "\n".join(lines)[:50000]
    except Exception as e:
        return f"Error: {e}"


def run_write(path: str, content: str) -> str:
    try:
        fp = safe_path(path)
        fp.parent.mkdir(parents=True, exist_ok=True)
        out = run_fxsh_bridge("write_file", {"FXSH_TOOL_PATH": path, "FXSH_TOOL_CONTENT": content})
        if out.strip() == "ok":
            return f"Wrote {len(content)} bytes to {path} (via fxsh)"
        return f"Error: write failed via fxsh ({out[:200]})"
    except Exception as e:
        return f"Error: {e}"


def run_file_exists(path: str) -> str:
    try:
        out = run_fxsh_bridge("file_exists", {"FXSH_TOOL_PATH": path}).strip().lower()
        if out in ("true", "false"):
            return out
        return f"Error: unexpected exists output: {out}"
    except Exception as e:
        return f"Error: {e}"


def run_edit(path: str, old_text: str, new_text: str) -> str:
    try:
        out = run_fxsh_bridge(
            "edit_file",
            {"FXSH_TOOL_PATH": path, "FXSH_TOOL_OLD": old_text, "FXSH_TOOL_NEW": new_text},
        ).strip()
        if out == "ok":
            return f"Edited {path} (via fxsh)"
        return f"Error: edit failed via fxsh ({out[:200]})"
    except Exception as e:
        return f"Error: {e}"


def run_fxsh_eval(expr: str) -> str:
    try:
        r = subprocess.run(
            ["./bin/fxsh", "-e", expr],
            cwd=WORKDIR,
            capture_output=True,
            text=True,
            timeout=120,
        )
        out = _extract_fxsh_payload((r.stdout + r.stderr).strip())
        return out[:50000] if out else "(no output)"
    except Exception as e:
        return f"Error: {e}"


def run_fxsh_file(path: str) -> str:
    try:
        p = safe_path(path)
        rel = str(p.relative_to(WORKDIR))
        r = subprocess.run(
            ["./bin/fxsh", rel],
            cwd=WORKDIR,
            capture_output=True,
            text=True,
            timeout=120,
        )
        out = _extract_fxsh_payload((r.stdout + r.stderr).strip())
        return out[:50000] if out else "(no output)"
    except Exception as e:
        return f"Error: {e}"


def run_fxsh_bridge(op: str, extra_env: dict[str, str]) -> str:
    tool_file = WORKDIR / "tools" / "agent_bridge.fxsh"
    if not tool_file.exists():
        return "Error: tools/agent_bridge.fxsh not found"
    env = os.environ.copy()
    env["FXSH_TOOL_OP"] = op
    for k, v in extra_env.items():
        env[k] = v
    try:
        r = subprocess.run(
            ["./bin/fxsh", str(tool_file.relative_to(WORKDIR))],
            cwd=WORKDIR,
            env=env,
            capture_output=True,
            text=True,
            timeout=120,
        )
        out = _extract_fxsh_payload((r.stdout + r.stderr).strip())
        return out[:50000] if out else "(no output)"
    except Exception as e:
        return f"Error: {e}"


def _extract_fxsh_payload(out: str) -> str:
    marker = "Comptime evaluation:"
    if marker not in out:
        return out.strip()
    tail = out.split(marker, 1)[1]
    if "\n\nInterpreter:" in tail:
        tail = tail.split("\n\nInterpreter:", 1)[0]
    return tail.strip()


def tool_specs_base() -> list[dict[str, Any]]:
    return [
        {
            "name": "bash",
            "description": "Run a shell command.",
            "input_schema": {
                "type": "object",
                "properties": {"command": {"type": "string"}},
                "required": ["command"],
            },
        },
        {
            "name": "read_file",
            "description": "Read file contents.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "limit": {"type": "integer"},
                },
                "required": ["path"],
            },
        },
        {
            "name": "write_file",
            "description": "Write content to file.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "content": {"type": "string"},
                },
                "required": ["path", "content"],
            },
        },
        {
            "name": "file_exists",
            "description": "Check file existence.",
            "input_schema": {
                "type": "object",
                "properties": {"path": {"type": "string"}},
                "required": ["path"],
            },
        },
        {
            "name": "edit_file",
            "description": "Replace exact text in file.",
            "input_schema": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "old_text": {"type": "string"},
                    "new_text": {"type": "string"},
                },
                "required": ["path", "old_text", "new_text"],
            },
        },
        {
            "name": "fxsh_eval",
            "description": "Evaluate an fxsh expression.",
            "input_schema": {
                "type": "object",
                "properties": {"expr": {"type": "string"}},
                "required": ["expr"],
            },
        },
        {
            "name": "fxsh_run",
            "description": "Run an fxsh source file.",
            "input_schema": {
                "type": "object",
                "properties": {"path": {"type": "string"}},
                "required": ["path"],
            },
        },
    ]


def handlers_base() -> dict[str, Callable[..., str]]:
    return {
        "bash": lambda **kw: run_shell(kw["command"]),
        "read_file": lambda **kw: run_read(kw["path"], kw.get("limit")),
        "write_file": lambda **kw: run_write(kw["path"], kw["content"]),
        "edit_file": lambda **kw: run_edit(kw["path"], kw["old_text"], kw["new_text"]),
        "file_exists": lambda **kw: run_file_exists(kw["path"]),
        "fxsh_eval": lambda **kw: run_fxsh_eval(kw["expr"]),
        "fxsh_run": lambda **kw: run_fxsh_file(kw["path"]),
    }


def extract_text_blocks(content: Any) -> str:
    out: list[str] = []
    if not isinstance(content, list):
        return ""
    for b in content:
        if getattr(b, "type", None) == "text":
            out.append(getattr(b, "text", ""))
    return "\n".join(x for x in out if x)


class TodoManager:
    def __init__(self) -> None:
        self.items: list[dict[str, str]] = []

    def update(self, items: list[dict[str, Any]]) -> str:
        if len(items) > 20:
            raise ValueError("Max 20 todos allowed")
        normalized: list[dict[str, str]] = []
        in_progress = 0
        for i, item in enumerate(items):
            item_id = str(item.get("id", str(i + 1)))
            text = str(item.get("text", "")).strip()
            status = str(item.get("status", "pending")).strip().lower()
            if not text:
                raise ValueError(f"Item #{item_id}: text required")
            if status not in ("pending", "in_progress", "completed"):
                raise ValueError(f"Item #{item_id}: invalid status {status}")
            if status == "in_progress":
                in_progress += 1
            normalized.append({"id": item_id, "text": text, "status": status})
        if in_progress > 1:
            raise ValueError("Only one todo can be in_progress")
        self.items = normalized
        return self.render()

    def render(self) -> str:
        if not self.items:
            return "No todos."
        mark = {"pending": "[ ]", "in_progress": "[>]", "completed": "[x]"}
        lines = [f"{mark[x['status']]} #{x['id']}: {x['text']}" for x in self.items]
        done = sum(1 for x in self.items if x["status"] == "completed")
        lines.append(f"\n({done}/{len(self.items)} completed)")
        return "\n".join(lines)


class SkillLoader:
    def __init__(self, skills_dir: Path):
        self.skills_dir = skills_dir
        self.skills: dict[str, dict[str, str]] = {}
        self._scan()

    def _scan(self) -> None:
        if not self.skills_dir.exists():
            return
        for fp in sorted(self.skills_dir.rglob("SKILL.md")):
            text = fp.read_text()
            meta, body = self._parse_frontmatter(text)
            name = meta.get("name", fp.parent.name)
            self.skills[name] = {"meta": meta, "body": body, "path": str(fp)}

    @staticmethod
    def _parse_frontmatter(text: str) -> tuple[dict[str, str], str]:
        m = re.match(r"^---\n(.*?)\n---\n(.*)", text, re.DOTALL)
        if not m:
            return {}, text
        meta: dict[str, str] = {}
        for line in m.group(1).strip().splitlines():
            if ":" in line:
                k, v = line.split(":", 1)
                meta[k.strip()] = v.strip()
        return meta, m.group(2).strip()

    def descriptions(self) -> str:
        if not self.skills:
            return "(no skills available)"
        rows = []
        for name, s in self.skills.items():
            desc = s["meta"].get("description", "No description")
            rows.append(f"- {name}: {desc}")
        return "\n".join(rows)

    def load(self, name: str) -> str:
        x = self.skills.get(name)
        if not x:
            return f"Error: unknown skill '{name}'. Available: {', '.join(self.skills.keys())}"
        return f"<skill name=\"{name}\">\n{x['body']}\n</skill>"


def run_agent_loop(
    *,
    client: Anthropic,
    system: str,
    tools: list[dict[str, Any]],
    handlers: dict[str, Callable[..., str]],
    messages: list[dict[str, Any]],
    max_rounds: int = 60,
    after_round: Callable[[list[str], list[dict[str, Any]]], None] | None = None,
) -> str:
    if not MODEL:
        raise RuntimeError("MODEL_ID is required in environment")

    last_text = ""
    for _ in range(max_rounds):
        response = client.messages.create(
            model=MODEL,
            system=system,
            messages=messages,
            tools=tools,
            max_tokens=8000,
        )
        messages.append({"role": "assistant", "content": response.content})
        last_text = extract_text_blocks(response.content)

        if response.stop_reason != "tool_use":
            return last_text

        used_tools: list[str] = []
        results: list[dict[str, Any]] = []
        for block in response.content:
            if getattr(block, "type", None) != "tool_use":
                continue
            used_tools.append(block.name)
            handler = handlers.get(block.name)
            try:
                output = handler(**block.input) if handler else f"Unknown tool: {block.name}"
            except Exception as e:
                output = f"Error: {e}"
            print(f"> {block.name}: {str(output)[:200]}")
            results.append(
                {
                    "type": "tool_result",
                    "tool_use_id": block.id,
                    "content": str(output),
                }
            )

        if after_round:
            after_round(used_tools, results)
        messages.append({"role": "user", "content": results})

    return "Stopped: max rounds reached"
