# fx-superpowers Architecture (MVP)

## 1. 目标

`fx-superpowers` 是一个基于 `fxsh` 的命令行工具，目标是复现 superpowers 的核心工作流：
- 初始化目录
- 安装技能
- 更新技能
- 列表与健康检查

## 2. 分层

1. CLI 边界层（`scripts/fx-superpowers`）
- 参数解析
- 环境变量注入
- 统一错误码

2. 应用入口层（`apps/fx-superpowers/src/main.fxsh`）
- 路由子命令
- 组织输出格式（text/json）

3. 领域模块层（`apps/fx-superpowers/src/fxsuperpowers/*.fxsh`）
- `Cli`：CLI 语义与输出拼装
- `Fsx`：路径、目录、原子写辅助
- `Manifest`：manifest 读写与校验
- `Install`：安装计划与冲突策略
- `Template`：token 渲染
- `Doctor`：依赖与环境检查

4. 测试层（后续）
- 黑盒 shell 集成测试
- 黄金文件测试

## 3. 关键约束

- 保持 clean-room：实现仅依据行为规格
- 外部依赖通过 `Process` 模块集中调用
- 所有写操作优先支持 `--dry-run`
- 冲突策略统一由 `Install` 模块执行

## 4. 发布策略

- 第一阶段：`scripts/fx-superpowers` + 解释器运行
- 第二阶段：评估 `--native-codegen` 可用性
