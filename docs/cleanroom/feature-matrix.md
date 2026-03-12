# fx-superpowers Feature Matrix (MVP)

状态说明：
- `Planned`: 已纳入计划，未实现
- `In Progress`: 开发中
- `Done`: 已实现并测试
- `Partial`: 部分实现，行为存在差异
- `Not Planned`: 本阶段不做

| Capability | Target Behavior | Status | Notes |
|---|---|---|---|
| init workspace | 初始化项目级目录与最小清单 | Done | tests/integration/fx_superpowers_init.sh |
| init user scope | 初始化用户级目录 | Done | 已支持 `--scope user` 路径策略 |
| list | 列出已安装技能/命令 | Done | 已支持文本与 JSON 输出 |
| status | 输出安装根目录与初始化状态 | Done | 已支持文本与 JSON 输出 |
| doctor | 检查依赖与目录健康 | Done | 已支持依赖+目录状态与 JSON 输出 |
| install from local path | 从本地路径安装技能 | Done | tests/integration/fx_superpowers_install.sh |
| install from git url | 从 git 仓库安装技能 | Done | tests/integration/fx_superpowers_install.sh |
| install command from local path | 从本地路径安装命令文件 | Done | tests/integration/fx_superpowers_install.sh |
| install command from git url | 从 git 仓库 `commands/` 安装命令 | Done | tests/integration/fx_superpowers_install.sh |
| update installed skills | 更新已安装技能 | Done | tests/integration/fx_superpowers_update.sh |
| update installed commands | 更新已安装命令 | Done | tests/integration/fx_superpowers_update.sh |
| conflict policy: skip | 冲突时跳过 | Done | tests/integration/fx_superpowers_install.sh |
| conflict policy: overwrite | 冲突时覆盖 | Done | tests/integration/fx_superpowers_install.sh |
| conflict policy: backup | 冲突时备份后覆盖 | Done | tests/integration/fx_superpowers_install.sh |
| template token ARGUMENTS | 支持 `{{ARGUMENTS}}` | Done | tests/integration/fx_superpowers_render.sh |
| template token ROOT | 支持 `{{ROOT}}` | Done | tests/integration/fx_superpowers_render.sh |
| template token DATE | 支持 `{{DATE}}` | Done | tests/integration/fx_superpowers_render.sh |
| YAML frontmatter full compatibility | 兼容复杂 YAML | Not Planned | MVP 改用 JSON manifest |
| native-codegen release mode | 原生编译发布路径 | Not Planned | 首发用解释器+启动器 |

## Compatibility Delta Tracking

| Upstream Surface | fx-superpowers Plan | Delta |
|---|---|---|
| install script workflow | `init + install` 子命令 | 等价工作流，不复制脚本文本 |
| skill installation | local/git 双来源 | 优先可维护性，协议可扩展 |
| argument templating | 白名单 token 替换 | 限制 token 集，先保稳定 |
| command suite sync | Partial | 已支持 command 安装与更新，仍缺完整远端索引同步策略 |

## Next Update Rule

每完成一个能力项：
1. 将状态改为 `Done` 或 `Partial`
2. 在 Notes 标注对应测试脚本路径
3. 若 `Partial`，必须写清差异与后续计划
