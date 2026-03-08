# TODO 文档中文翻译

> 翻译说明：本文档跟踪使用 `apps/fx-doctor` 工作负载构建和演示 `fxsh 1.0` 时发现的问题。

---

## 优先级 0（已完成）

- [x] **添加最小 CLI 参数支持**
  - 原因：当前应用需要一个 shell 启动器来将 argv 转换为环境变量
  - MVP 接口：
    - `argv0() -> string`
    - `argc() -> int`
    - `argv_at(i: int) -> string`
  - 涉及区域：main.c, interp.c, codegen.c, shell.c, fxsh.h
  - 验收标准：纯 fxsh CLI 可以在常见场景下直接读取参数
  - 状态：解释器、--native、--native-codegen 现在一致接收脚本 argv

- [x] **让 `fxsh <file.fxsh>` 默认静默执行**
  - 原因：解释器模式目前打印 "Type"、"Comptime evaluation"、"Interpreter"、"Success" 横幅，这对最终用户 CLI 应用是错误的
  - 期望行为：
    - 默认运行只打印程序输出
    - 调试横幅移至显式标志之后
  - 涉及区域：main.c
  - 验收标准：CLI 脚本可以直接运行而无需输出过滤
  - 状态：正常文件执行现在只打印程序输出，-e/--eval 仍然打印最终表达式结果

---

## 优先级 1（大部分已完成）

- [x] **修复 --codegen 只输出纯 C 代码**
  - 原因：当前 --codegen 输出被前端横幅污染
  - 涉及区域：main.c
  - 验收标准：`./bin/fxsh --codegen file.fxsh > out.c` 生成可直接编译的 C 代码
  - 状态：--codegen 现在直接以生成的 C 代码开始

- [ ] **稳定 native-codegen 中的多文件 CLI 工作负载**
  - 原因：大量使用 string/list 的应用代码在生成的 C 中仍然失败
  - 观察到的症状：
    - 辅助函数和 list/string 辅助函数的 C 类型不匹配
    - 运行时应用路径不得不回退到解释器 + 启动器过滤
  - 涉及区域：codegen.c, types.c
  - 验收标准：apps/fx-doctor 可以通过 native-codegen 构建和运行
  - 状态：apps/fx-doctor/src/main.fxsh 现在可以通过 ./bin/fxsh --native-codegen 运行

- [x] **修复 codegen 中顶层 unit 绑定**
  - 原因：`let _ = print ...` 仍然生成无效 C 代码如 `static void ___0;`
  - 涉及区域：codegen.c
  - 验收标准：返回 unit 的顶层副作用绑定在 --native-codegen 中能干净编译
  - 状态：已修复

- [x] **在 codegen/native-codegen 中执行裸顶层表达式**
  - 原因：尾部顶层表达式如 `print "ok"` 在生成的 C 中被忽略，而解释器执行了它
  - 涉及区域：codegen.c
  - 验收标准：解释器和 --native-codegen 按程序顺序执行相同的顶层表达式
  - 状态：已修复

- [x] **审计类型推断中与 record 相关的辅助函数**
  - 原因：使用 record 的通用辅助层在 fx-doctor 实现期间遇到推断边界
  - 观察到的症状：围绕 `{ code, stdout, stderr }` 风格 record 构建的辅助模块在更大组合中变得脆弱
  - 涉及区域：types.c, interp.c
  - 验收标准：中等规模的多模块 CLI 可以使用 record 结构化 shell 结果而不会遇到类型系统意外
  - 状态：native codegen 的 record 字段投影现在恢复投影字段类型而不是默认为 i64

- [x] **审计 tuple/constructor 中等元数数据流的 ergonomics**
  - 原因：此工作负载暴露了在使用更高元数的 tuple 类数据流时的实际限制或混乱行为
  - 涉及区域：parser.c, types.c, codegen.c
  - 验收标准：tuple/constructor 元数行为已文档化、测试，并在解释器和 codegen 中保持一致
  - 状态：构造函数 tuple 语法糖现在按声明的元数规范化

---

## 优先级 2（待完成）

- [ ] **澄清 stdlib 导入语义文档**
  - 原因：`import String` 容易被误解为系统库导入
  - 需要的文档更新：
    - 导入首先尝试 `stdlib/<lowercase>.fxsh`
    - 导入的模块是源码展开的，不是动态链接
  - 涉及区域：README.md, docs/half_hour_fxsh.md

- [ ] **在文档中添加第一个 app 示例部分**
  - 原因：apps/fx-doctor 现在是一个有意义的参考工作负载
  - 需要的文档：
    - app 布局
    - 启动器模式
    - env 支持的桥接模式
    - 何时优先使用纯 fxsh vs 薄 shell 启动器

- [ ] **为面向 app 的运行时模式添加回归覆盖**
  - 需要的测试：
    - 静默正常执行
    - 纯 --codegen 输出
    - argv 内置函数
    - 多文件 app 导入路径冒烟测试

- [x] **在 native-codegen 中特化泛型柯里化闭包**（已完成）
  - 原因：跨闭包阶段捕获值的柯里化泛型辅助函数将后续阶段参数类型擦除为 s64
  - 修复方式：
    - 当调用站点有完全具体参数时，将柯里化 lambda 链展平为 mono-spec 本地辅助函数
    - 让 gen_call 在回退到通用闭包存根之前优先选择这些 mono 辅助函数
  - 回归测试：examples/closure_curried_generic.fxsh

- [x] **放宽解析器对多行括号表达式的敏感性**（已完成）
  - 原因：examples/argv_basic.fxsh 中的多行 `print (a ++ (if ...))` 形式触发解析错误，直到被展平为一行
  - 涉及区域：parser.c
  - 验收标准：普通多行括号表达式与单行形式解析相同
  - 状态：二进制/逻辑/管道表达式解析现在一致跳过内部换行符

---

## 当前仓库中的变通方案

- `apps/fx-doctor` 使用稳定的混合路径：
  - shell 启动器用于 argv + 进程编排
  - `fxsh` 用于格式化/输出模式
- 这对当前演示是可接受的，但不应该成为长期的语言故事。

---

## 后续实际需要做的工作

### 1. 文档完善（Priority 2）

| 任务 | 优先级 | 说明 |
|------|--------|------|
| 澄清 stdlib 导入语义 | P2 | 更新 README.md 和 half_hour_fxsh.md |
| 添加 app 示例部分 | P2 | fx-doctor 作为参考，展示 app 布局、启动器模式、env 桥接 |
| 添加回归测试 | P2 | 静默执行、codegen 输出、argv、多文件导入 |

### 2. native-codegen 稳定性（Priority 1 未完成）

| 任务 | 优先级 | 说明 |
|------|--------|------|
| 多文件 CLI 工作负载 | P1 | 修复 string/list 在生成的 C 中的类型不匹配 |
| fx-doctor 完全原生化 | P1 | 移除 shell 启动器，完全通过 native-codegen 运行 |

### 3. 长期改进（路线图）

| 任务 | 状态 | 说明 |
|------|------|------|
| ANF IR | 计划中 | 引入 A-Normal Form 中间表示 |
| TCO | 部分支持 | native-codegen 已支持尾调用优化 |
| 内联优化 | 计划中 | 激进的中端优化 |
| 错误报告体验 | 持续改进 | 更完整的错误诊断 |

---

## 总结

**已完成**：大部分 Priority 0 和 Priority 1 已完成，语言核心功能稳定。

**待完成**：
1. **文档补充** - stdlib 导入语义、app 示例、回归测试
2. **native-codegen 最后一公里** - 修复 string/list 类型问题，让 fx-doctor 完全原生化
3. **长期优化** - ANF IR、TCO、内联

fx-doctor 当前的混合路径（shell 启动器 + fxsh）是可接受的临时方案，但长期目标应该是纯 fxsh 原生化。
