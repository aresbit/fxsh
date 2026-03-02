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

## 待实现功能

- [x] 编译期元编程 (`comptime`)
- [ ] 模式匹配 (`match ... with`)
- [ ] 行多态记录类型
- [ ] 标准库 (IO, Path, Process 模块)
- [ ] 代码生成 (C 后端)
- [ ] 递归函数支持
- [ ] 类型注解语法
- [ ] 完整的 comptime 函数调用

## 技术栈

- **语言**: C17 (GNU)
- **构建**: Make + Clang
- **依赖**: sp.h (单头文件标准库替代)
- **类型系统**: Hindley-Milner 推导

## 许可证

MIT License
