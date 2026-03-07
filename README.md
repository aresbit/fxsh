# fxsh - Functional Core Minimal Bash

一个函数式核心、类型安全的 shell 脚本语言，目标是让 OCaml 初学者都能轻松编写替代 bash 的脚本。

## 项目状态

### 已实现功能

- [x] **现代 C 项目框架**：使用 sp.h 单头文件库
- [x] **词法分析器**：完整的 fxsh 语言词法分析
- [x] **语法分析器**：递归下降解析器，支持表达式
- [x] **类型推导**：Hindley-Milner 风格类型系统
  - 统一变量与替换
  - 类型统一（Unification）
  - Occurs 检查
  - 支持多态类型 `'a -> int`
- [x] **编译期宏系统 (comptime)**：Zig 风格的编译期执行
  - 编译期值表示（CTValue）
  - 编译期求值引擎
  - 类型反射（TypeInfo）
  - 派生宏（derive Show, Eq）

### 语言特性

```fxsh
# 字面量
42          # int
3.14        # float
"hello"     # string
true        # bool

# 算术运算
1 + 2 * 3   # int

# 比较
x == y      # bool
x < y       # bool

# 逻辑运算
a and b     # bool
not c       # bool

# 函数 (lambda)
fn x -> x + 1
fn x y -> x + y

# let 绑定
let x = 10 in x + 5

# if 表达式
if x > 0 then x else -x

# 管道操作符
5 |> double |> add_one

# 编译期计算 (comptime)
let comptime MAX_SIZE = 1024 * 1024   # 编译期求值
let comptime DEBUG = true

# 编译期类型编程
let comptime make_vector = fn T -> {
  data = T,
  len = 0,
  cap = 0
}

# 编译期条件
let comptime mode = if DEBUG then "debug" else "release"

# C FFI (native-codegen)
let c_labs : int -> int = "c:labs"
let x = c_labs (-7)

# string ABI bridge: fxsh string <-> C const char*
let c_atol : string -> int = "c:atol"
let n = c_atol "12345"

# precise C integer ABI types for FFI
let c_puts : string -> c_int = "c:puts"
let rc : int = c_int_to_int (c_puts "hello")

# libuv example (needs FXSH_LDFLAGS='-luv')
let uv_version : unit -> int = "c:uv_version"
let v = uv_version ()

# pointer helpers (for opaque handles)
let p : unit ptr = c_malloc 64
let p2 : unit ptr = c_cast_ptr p

# callback pointer helper (top-level function -> opaque C callback pointer)
let on_tick = fn _ -> ()
let cbp : unit ptr = c_callback0 on_tick

# tensor MVP (2D float)
let a = tensor_from_list2 2 3 [1.0; 2.0; 3.0; 4.0; 5.0; 6.0]
let b = tensor_from_list2 3 2 [7.0; 8.0; 9.0; 10.0; 11.0; 12.0]
let c = tensor_dot a b
let v = tensor_get2 c 1 1

# JSON runtime helpers (compact + path get)
let doc = json_compact "{ \"agent\": { \"name\": \"fx\", \"ok\": true } }"
let name = json_get_string doc "agent.name"
let ok = json_get_bool doc "agent.ok"

# shell-like process helpers (stdin/stdout/stderr/pipe/glob/grep)
let code = exec_code "sh -c \"exit 3\""
let out = exec_stdout "printf \"a\\nb\\n\""
let err = exec_stderr "sh -c \"echo boom 1>&2\""
let via_stdin = exec_stdin "cat" "hello"
let via_stdin_code = exec_stdin_code "cat >/dev/null" "hello"
let via_stdin_err = exec_stdin_stderr "sh -c \"cat >/dev/null; echo boom 1>&2\"" "hello"
let piped = exec_pipe "printf \"x\\ny\\n\"" "grep y"
let piped_code = exec_pipe_code "printf \"x\\ny\\n\"" "grep y"
let piped_err = exec_pipe_stderr "printf \"x\\ny\\n\"" "sh -c \"cat >/dev/null; echo boom 1>&2\""
let cap = exec_capture "sh -c \"echo out; echo err 1>&2\""
let cap_code = capture_code cap
let cap_out = capture_stdout cap
let cap_err = capture_stderr cap
let cap_rel = capture_release cap
let cap_in = exec_stdin_capture "sh -c \"cat; echo e 1>&2\"" "in"
let cap_pipe = exec_pipe_capture "printf \"a\\nb\\n\"" "grep b"
let cap_pipefail = exec_pipefail_capture "sh -c \"echo L; exit 7\"" "cat >/dev/null"
let cap_pipefail3 = exec_pipefail3_capture "sh -c \"exit 5\"" "cat >/dev/null" "cat >/dev/null"
let p2 = Shell.pipe "printf \"a\\nb\\n\"" "grep b"
let p2_pf = Shell.pipefail "sh -c \"echo L; exit 7\"" "cat >/dev/null"
let p3_pf = Shell.pipefail3 "sh -c \"exit 5\"" "cat >/dev/null" "cat >/dev/null"
let matched = grep_lines "^b.*$" out   # regex (Thompson NFA): . * + ? | () ^ $
let paths = glob "examples/*.fxsh"

# comptime derive-json schema (MVP)
let user = { id = 1, name = "fx", ok = true }
let comptime USER_SCHEMA = @jsonSchema(@typeOf(user))

# typed-shape transformer sketch (static wrapper types)
type embed = Embed of tensor
type hidden = Hidden of tensor
let w_up = tensor_new2 4 8 0.1
let to_hidden = fn x -> match x with | Embed t -> Hidden (tensor_dot t w_up) end
```

## 构建

```bash
make              # 编译项目
make debug=1      # 调试模式编译
make test         # 运行单元测试
make clean        # 清理构建文件
```

## 使用

```bash
# 查看帮助
./bin/fxsh --help

# 查看版本
./bin/fxsh --version

# 求值表达式
./bin/fxsh -e "42"
./bin/fxsh -e "1 + 2"
./bin/fxsh -e 'fn x -> x + 1'

# 打印 tokens
./bin/fxsh -t -e "1 + 2"

# 执行文件
./bin/fxsh examples/factorial.fxsh
```

## Agent 机制迁移 (s01-s12)

项目已内置从 `learn-claude-code` 迁移的前 12 个 agent 机制，入口在 `agents/`：

```bash
pip install -r requirements-agent.txt
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

详细说明见 `agents/README.md`。

## 项目结构

```
fxsh/
├── src/
│   ├── main.c              # 入口点
│   ├── lexer/
│   │   └── lexer.c         # 词法分析器
│   ├── parser/
│   │   └── parser.c        # 语法分析器
│   ├── types/
│   │   └── types.c         # 类型系统与推导
│   └── comptime/
│       └── comptime.c      # 编译期求值引擎
├── include/
│   └── fxsh.h              # 公共头文件
├── lib/
│   └── sp.h                # spclib 单头文件库
├── examples/
│   ├── hello.fxsh          # Hello World 示例
│   ├── factorial.fxsh      # 阶乘示例
│   ├── pipeline.fxsh       # 管道操作示例
│   ├── comptime.fxsh       # 编译期求值示例
│   └── vector.fxsh         # 泛型向量示例
├── Makefile
└── README.md
```

## 设计哲学

### 摒弃 bash 的复杂性

| bash | fxsh |
|------|------|
| `x=10` (无空格!) | `let x = 10` |
| `$((x + 5))` | `x + 5` |
| `function foo() { }` | `let foo = fn x -> ...` |
| `if [ "$x" -eq 10 ]; then` | `if x == 10 then ... end` |
| `for i in $(seq 1 10); do` | `for i in 1..10 do ... end` |

### 类型安全

```fxsh
let id = fn x -> x          # 'a -> 'a (多态)
let compose = fn f g x -> f (g x)   # ('b -> 'c) -> ('a -> 'b) -> 'a -> 'c
```

## 当前进度

- [x] 编译期元编程 (`comptime`)
- [x] 模式匹配（ADT / tuple / record / list 主路径）
- [x] 行多态记录类型（基础）
- [x] 代码生成（C 后端，`--native-codegen`）
- [x] 递归函数支持（解释器 + native 主路径）
- [x] 类型注解语法（let 绑定基础）
- [~] 标准库收敛（已有 `list/process/json/path/system`，仍在统一 API）
- [x] shell 设计收敛：不引入语言层语法糖（无 `$()` / `run!` / `try!` / `cap!`）
- [ ] ANF IR / TCO / 内联优化通道

## 技术栈

- **语言**: C17 (GNU)
- **构建**: Make + Clang
- **依赖**: sp.h (单头文件标准库替代)
- **类型系统**: Hindley-Milner 推导

## 许可证

MIT License
