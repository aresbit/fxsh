# fx-superpowers Clean-Room Spec (MVP)

## 1. 目标

在不复制上游实现代码的前提下，基于公开行为信息实现一个 `fxsh` 版本的 superpowers 核心能力：
- 初始化工作区命令/技能目录
- 安装与更新技能
- 渲染基础模板变量
- 输出可诊断的状态与依赖检查结果

本规格只定义“可观察行为”，不定义内部实现细节。

## 2. Clean-Room 边界

允许输入（Allowed Inputs）：
- 上游公开文档（README、INSTALL、公开命令说明）
- 公开 CLI 使用示例与输出语义
- 手工编写的行为用例与测试数据

禁止输入（Disallowed Inputs）：
- 上游源代码文件内容（脚本/模块/模板）
- 上游私有实现注释、函数名、目录结构作为直接蓝图
- 复制粘贴上游模板文本

实现约束：
- 先写行为测试，再写实现
- 所有偏差在 `feature-matrix.md` 标注
- 每次新增命令必须附失败路径测试

## 3. 术语

- Workspace install: 安装到当前项目目录（建议 `./.codex/` 或项目约定目录）。
- User/global install: 安装到用户级目录（如 `$HOME/.codex/`）。
- Skill manifest: 描述技能元数据的清单文件（MVP 使用 JSON）。

## 4. CLI 行为规格（MVP）

### 4.1 `fx-superpowers init`

输入：
- 可选 `--root <path>`
- 可选 `--scope workspace|user`（默认 workspace）
- 可选 `--dry-run`

行为：
- 创建必要目录（已存在则不报错）
- 写入最小示例清单（若目标不存在）
- 输出执行摘要（创建/跳过的路径数）

失败条件：
- 目标路径无权限写入
- 参数非法

### 4.2 `fx-superpowers list`

输入：
- 可选 `--root <path>`
- 可选 `--json`

行为：
- 列出已发现的技能与命令
- `--json` 输出稳定 JSON 结构

失败条件：
- 根路径不存在或不可读

### 4.3 `fx-superpowers install skill <path|git-url>`

输入：
- 技能来源（本地路径或 git URL）
- 可选 `--scope`
- 可选 `--on-conflict skip|overwrite|backup`（默认 skip）
- 可选 `--dry-run`

行为：
- 验证来源可读
- 复制到目标目录
- 写入/更新索引
- 输出安装结果（installed/skipped/updated）

失败条件：
- 来源非法
- manifest 缺失或无效
- 依赖命令（git）不可用

### 4.4 `fx-superpowers update`

输入：
- 可选 `--scope`
- 可选 `--dry-run`

行为：
- 扫描已安装项
- 对可更新项执行更新
- 输出更新摘要

失败条件：
- 更新来源不可达
- 本地目标不可写

### 4.5 `fx-superpowers doctor`

输入：
- 可选 `--json`

行为：
- 检查运行必需依赖（最少：`git`, `sh`, `fxsh`）
- 检查路径权限与目录完整性
- 输出 `ok/warn/fail` 状态

## 5. 模板渲染规格（MVP）

支持 token：
- `{{ARGUMENTS}}`
- `{{ROOT}}`
- `{{DATE}}`（格式 `YYYY-MM-DD`）

规则：
- 未知 token 视为错误
- 替换是纯文本，不执行表达式
- 保持输入换行风格

## 6. Manifest 规格（MVP）

MVP 使用 JSON，必填字段：
- `name: string`
- `version: string`
- `entry: string`
- `description: string`

可选字段：
- `commands: string[]`
- `source: { type: "local" | "git", value: string }`

## 7. 非功能要求

- 幂等：重复执行 `init/install` 不应破坏已有数据
- 可追踪：每次写入可输出变更摘要
- 可测试：核心命令有黑盒脚本测试
- 可回退：冲突策略支持 `backup`

## 8. 验收标准

- 覆盖 `init/list/install/update/doctor` 五个命令主路径
- 每个命令至少 1 个失败路径测试
- `--dry-run` 不产生写入副作用
- 在空目录可完成 `init -> install -> list`
