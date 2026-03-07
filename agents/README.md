# fxsh Agent Stages (s01-s12)

这是从 `learn-claude-code` 迁移到当前 `fxsh` 项目的前 12 个机制：

- `s01_agent_loop.py`: 最小 agent loop（单工具 `fxsh_eval`）
- `s02_tool_use.py`: 工具分发（bash/read/write/edit/fxsh_eval/fxsh_run）
- `s03_todo_write.py`: TodoWrite + nag reminder
- `s04_subagent.py`: 子 agent（`task` 工具，隔离上下文）
- `s05_skill_loading.py`: Skills 按需加载（`load_skill`）
- `s06_context_compact.py`: 三层上下文压缩（micro/auto/manual compact）
- `s07_task_system.py`: 持久化任务图（`.tasks/*.json`，支持依赖关系）
- `s08_background_tasks.py`: 后台任务（异步命令 + 结果通知注入）
- `s09_agent_teams.py`: Agent Teams（队友生成、点对点消息、广播）
- `s10_team_protocols.py`: Team Protocols（计划审批与关闭握手协议）
- `s11_autonomous_agents.py`: Autonomous Agents（空闲轮询、自动认领任务、持续协作）
- `s12_worktree_task_isolation.py`: Worktree + Task Isolation（按任务绑定独立目录执行，附带生命周期事件流）

## 当前 fxsh 重写范围

- 已使用 fxsh 执行：`read_file`、`write_file`、`file_exists`、`edit_file`（通过 `tools/agent_bridge.fxsh`）
- 暂保留 Python 执行：`bash`（原因：当前 fxsh 侧 `exec` 仅返回退出码，不返回 stdout/stderr）

## 环境

1. 安装依赖

```bash
pip install -r requirements-agent.txt
```

2. 配置环境变量（兼容 Anthropic API / 代理网关）

```bash
export MODEL_ID=claude-3-7-sonnet-latest
export ANTHROPIC_API_KEY=...
# 可选
export ANTHROPIC_BASE_URL=...
```

## 运行

```bash
python agents/s01_agent_loop.py
python agents/s02_tool_use.py
python agents/s03_todo_write.py
python agents/s04_subagent.py
python agents/s05_skill_loading.py
python agents/s06_context_compact.py
python agents/s07_task_system.py
python agents/s08_background_tasks.py
python agents/s09_agent_teams.py
python agents/s10_team_protocols.py
python agents/s11_autonomous_agents.py
python agents/s12_worktree_task_isolation.py
```
