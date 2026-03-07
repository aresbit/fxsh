---
name: agent-fxsh
description: fxsh 项目协作工作流，包括构建、运行、代码生成与解释器/原生后端一致性检查。
---

# Agent FXSH

适用场景：
- 修改 `src/*` 后快速确认编译是否通过
- 需要比较解释器与 native-codegen 路径
- 编写/更新 `examples/*.fxsh` 与 `stdlib/*.fxsh`

建议步骤：
1. `make -j2` 先确认可编译
2. 新增示例时优先放入 `examples/`，保持最小可复现
3. 涉及 builtin 时同步检查：
   - `src/types/types.c` (类型签名)
   - `src/interp/interp.c` (解释器行为)
   - `src/codegen/codegen.c` (native-codegen 行为)
4. 需要原生执行时使用：`./bin/fxsh --native-codegen <file>`
