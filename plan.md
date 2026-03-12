# fxsh 净室复现 `obra/superpowers` 实施计划

## 0. 结论（先回答你的问题）

可以做，但建议定义为“功能等价复现”而不是“逐行兼容复刻”。

基于当前仓库能力，`fxsh` 已具备复现主干所需能力：
- 文件系统与目录遍历：`read_file/write_file/list_dir/walk_dir/mkdir_p`（见 `include/fxsh.h` 与 `stdlib/fs.fxsh`）
- 进程执行与管道：`exec_* / capture_* / pipefail_* / glob / grep_lines`
- 模块化与多文件 CLI 实践：已有 `apps/fx-doctor` 参考

已知边界（需要在方案里规避）：
- 无原生 HTTP/Git API：通过 `exec_*` 调 `git/curl` 作为适配层
- 无原生 YAML 解析器：前期用“最小 frontmatter 解析器”或约束元数据格式
- 多文件 app 的 `native-codegen` 仍有稳定性风险：先走“解释器 + 启动器”发布路径

结论：**支持净室开发复现 superpowers 的核心能力**。

---

## 1. 净室开发规则（必须先落地）

1. 建立 `docs/cleanroom/`，仅保存以下材料：
   - 公开 README/INSTALL/命令说明的行为描述
   - 手工整理的“输入-输出”规范
   - 黑盒测试用例（不给实现细节）
2. 明确禁止项：
   - 不拷贝上游源码、脚本实现、模板文本
   - 不使用上游内部函数名/文件结构作为直接实现蓝图
3. 实施双文档流程：
   - `spec.md`：行为规格（由“观察者视角”编写）
   - `impl.md`：本仓库实现决策（由“实现者视角”编写）
4. 每个 PR 必须附“clean-room checklist”。

**交付物**：`docs/cleanroom/spec.md`、`docs/cleanroom/checklist.md`

---

## 2. 目标范围冻结（MVP）

定义复现目标为 superpowers 的“可用内核”：

1. 初始化命令：在目标目录生成/更新命令与技能目录骨架
2. 技能安装：支持从“本地目录 + git 仓库”安装技能到工作区
3. 命令安装：支持全局/项目级命令同步与覆盖策略
4. 变量替换：支持 `$ARGUMENTS` 等基础占位替换
5. 清单能力：`list`, `status`, `doctor`
6. 幂等更新：重复执行不会破坏已有安装

暂不纳入 MVP：
- 完整兼容全部上游脚本参数
- 高阶远程 API 集成
- 并发安装优化

**交付物**：`docs/cleanroom/feature-matrix.md`

---

## 3. 架构设计（fxsh 版本）

目录建议：

- `apps/fx-superpowers/src/main.fxsh`
- `apps/fx-superpowers/src/sp/cli.fxsh`
- `apps/fx-superpowers/src/sp/fsx.fxsh`
- `apps/fx-superpowers/src/sp/manifest.fxsh`
- `apps/fx-superpowers/src/sp/install.fxsh`
- `apps/fx-superpowers/src/sp/template.fxsh`
- `apps/fx-superpowers/scripts/fx-superpowers`（薄启动器）

设计原则：
- 纯函数核心（解析/规划） + 命令式边缘（文件写入/进程执行）
- 所有外部命令集中在 `sp/process.fxsh`，便于替换与测试
- 写入前先生成“计划对象”，支持 `--dry-run`

**交付物**：`docs/architecture.md` + 空模块骨架

---

## 4. 元数据与模板协议（先定协议再写代码）

1. 定义技能清单格式（建议 JSON，避免 YAML 复杂度）
   - `name`, `version`, `entry`, `description`, `commands[]`
2. 定义模板渲染协议
   - 仅支持 `{{ARGUMENTS}}`, `{{ROOT}}`, `{{DATE}}` 三类 token（MVP）
3. 定义安装冲突策略
   - `skip | overwrite | backup`

**交付物**：
- `docs/spec/manifest.schema.json`
- `docs/spec/template-rules.md`

---

## 5. 实现阶段 A：读写与发现层

1. 实现路径探测：项目级与用户级目录定位
2. 实现目录扫描：发现可安装技能与命令
3. 实现安全写入：
   - 原子写（临时文件 + rename）
   - 可选备份
4. 完成 `fx-superpowers list/status`

**验收**：
- 能正确枚举本地技能
- 重复扫描结果稳定

---

## 6. 实现阶段 B：安装器核心

1. `install skill <path|git-url>`
   - 本地路径直接复制
   - git URL 通过 `exec_*` 调 `git clone --depth=1`
2. `install command <name>`
   - 从模板生成目标命令文件
3. `init`
   - 生成默认目录结构与示例清单
4. `update`
   - 基于版本号或哈希判断是否更新

**验收**：
- 支持 `--dry-run`
- 覆盖/跳过/备份策略生效

---

## 7. 实现阶段 C：参数与模板渲染

1. 解析 CLI 参数（子命令 + flags）
2. 模板替换器：
   - 文本替换（严格 token 白名单）
   - 未知 token 报错
3. 在命令执行链中注入上下文：
   - 当前目录
   - 用户参数
   - 时间戳

**验收**：
- `render --arguments "x y"` 输出可预测
- 替换结果可回归测试

---

## 8. 测试体系（净室关键）

1. 黑盒集成测试（shell）
   - `tests/integration/fx_superpowers_init.sh`
   - `tests/integration/fx_superpowers_install.sh`
   - `tests/integration/fx_superpowers_update.sh`
2. 黄金文件测试
   - 关键模板渲染输出比对
3. 破坏性场景
   - 目标目录已存在
   - 权限不足
   - 非法 manifest

**验收**：CI 全绿，且每个命令至少一个失败路径测试。

---

## 9. 发布路径（先稳定后优化）

1. 第一阶段发布：
   - `scripts/fx-superpowers` + `./bin/fxsh apps/fx-superpowers/src/main.fxsh`
2. 第二阶段再评估 `--native-codegen`
3. 输出：
   - 使用文档
   - 迁移文档（与上游行为对照）

**验收**：在干净目录一键 `init -> install -> list -> update` 成功。

---

## 10. 里程碑与时间盒（建议）

1. M1（1-2 天）：净室规范 + feature matrix + 架构骨架
2. M2（2-3 天）：阶段 A + B（可安装、可更新）
3. M3（1-2 天）：阶段 C（模板与参数）
4. M4（1-2 天）：测试补齐 + 文档 + 首次发布

总计：**5-9 天可交付 MVP**（单人节奏）。

---

## 11. 风险与对策

1. YAML 兼容风险
   - 对策：MVP 改用 JSON manifest；必要时后续加 YAML 子集解析
2. 外部命令依赖风险（git/curl）
   - 对策：`doctor` 预检依赖，缺失时给可执行修复提示
3. native-codegen 稳定性风险
   - 对策：发布阶段固定解释器路径，native 仅作为实验开关
4. 行为偏差风险（与上游预期不同）
   - 对策：用 feature matrix 明确“已兼容/有差异/不支持”

---

## 12. 开工顺序（按这个顺序直接做）

1. 建 `docs/cleanroom/*` 与 `feature-matrix.md`
2. 建 `apps/fx-superpowers` 项目骨架
3. 先做 `list/status/doctor`（只读能力）
4. 再做 `init`（低风险写入）
5. 再做 `install/update`（核心）
6. 再做模板渲染与参数替换
7. 最后补测试和文档

> 完成以上 7 步后，即可得到一个可实际使用的 fxsh 版 superpowers MVP。
