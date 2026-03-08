# fxsh 语言规格文档 v2.0

> Functional Core Minimal Shell Script Language  
> 版本：1.0.0 | 更新日期：2026-03-09

---

## 1. 语言概述

### 1.1 设计目标

fxsh 是一个函数式核心、类型安全的 shell 脚本语言，目标是让具备函数式编程背景的开发者能用更可控的方式编写替代 bash 的脚本。

### 1.2 核心理念

| 理念 | 说明 |
|------|------|
| **函数式核心** | 所有值默认不可变，副作用通过显式接口处理 |
| **类型安全** | Hindley-Milner 类型推导，支持多态 |
| **脚本优先** | 通过标准库调用系统命令，语言层不引入专用 shell 语法 |
| **零运行时 GC** | Arena 分配，脚本结束 OS 回收 |
| **可读性优先** | 比 bash 更接近自然语言，无隐式魔法 |

### 1.3 定位

- **不是**：bash 兼容层、通用应用框架、隐式 typeclass 系统
- **是**：小型函数式语言 + 适合脚本场景的标准库和 builtin + 类型推导但不强制要求类型标注

---

## 2. 词法规范

### 2.1 基础词法单元

```
IDENT          ::= [a-z_][a-zA-Z0-9_']*      # 标识符
TYPE_IDENT     ::= [A-Z][a-zA-Z0-9_]*        # 类型标识符（以大写开头）
INT_LIT        ::= [0-9]+                    # 整数
FLOAT_LIT      ::= [0-9]+'.'[0-9]+           # 浮点数
STRING_LIT     ::= '"' (escape | ~["])* '"'  # 字符串
COMMENT        ::= '#' ~[\n]*                 # 注释
TYPE_VAR       ::= "'" IDENT                 # 类型变量 ('a, 'b)
```

### 2.2 关键字

```
let, rec, comptime, fn, if, then, else, match, with, end
type, module, import, trait, impl, do
true, false, not, and, or
```

### 2.3 运算符

```
# 算术
+  -  *  /  %

# 比较
==  !=  <  >  <=  >=

# 逻辑
and  or  not

# 字符串
++  # 字符串连接

# 管道
|>  # 函数组合

# 其他
::   # list cons
=    # 绑定
->   # 函数箭头
:    # 类型注解
.    # 字段访问
|    # ADT 分支
```

### 2.4 字符串字面量扩展（v1.0 新增）

```fxsh
# 普通字符串
"hello world"

# 多行字符串
let sql = """
  SELECT * FROM users
  WHERE id = 42
"""

# 原始字符串（不转义）
let path = r"C:\Users\foo\bar"

# 字节字符串
let bytes = b"\x00\x01\x02"

# 字符串插值
let msg = f"Hello {name}, age = {age}"
```

---

## 3. 语法规范

### 3.1 程序结构

```bnf
program     ::= decl* EOF

decl        ::= let_decl
              | type_decl
              | module_decl
              | import_decl
              | trait_decl
              | impl_decl
              | expr NEWLINE
```

### 3.2 声明

```bnf
# 变量/函数声明
let_decl    ::= "let" "rec"? "comptime"? 
                 IDENT param* (":" type)? "=" expr

# 类型声明（ADT 或 Record）
type_decl   ::= "type" type_params? IDENT "=" type_body
type_params ::= "'" IDENT+
type_body   ::= variant ("|" variant)*       # ADT
              | "{" field (";" field)* "}"   # Record

# 模块声明
module_decl ::= "module" TYPE_IDENT "=" "{" decl* "}"

# 导入声明
import_decl ::= "import" TYPE_IDENT ("." TYPE_IDENT)*

# trait 声明（显式接口命名空间）
trait_decl  ::= "trait" IDENT "=" "{" let_decl* "}"

# impl 声明（显式实现）
impl_decl   ::= "impl" IDENT "for" IDENT "=" "{" let_decl* "}"
```

### 3.3 表达式

```bnf
expr        ::= pipe_expr

pipe_expr   ::= logical ("|>" logical)*

logical     ::= comparison (("and" | "or") comparison)*

comparison  ::= additive (("==" | "!=" | "<" | ">" | "<=" | ">=") additive)?

additive    ::= multiplicative (("+"|"-"|"++") multiplicative)*

multiplicative ::= unary (("*"|"/"|"%") unary)*

unary       ::= ("-" | "not") unary
              | app_expr

app_expr    ::= postfix postfix*

postfix     ::= primary ("." IDENT)*
              | primary "(" args ")"

primary     ::= literal
              | IDENT | TYPE_IDENT
              | "(" expr ("," expr)* ")"         # 元组
              | "[" (expr (";" expr)*)? "]"      # 列表
              | "{" field_init (";" field_init)* "}"           # 记录
              | "{" expr "with" field_init (";" field_init)* "}"  # 记录更新
              | "fn" param+ "->" expr
              | "let" let_binding+ "in" expr
              | "if" expr "then" expr ("else" expr)?
              | "match" expr "with" ("|" pattern "->" expr)+
              | "do" "{" do_stmt* "}"
              | "@" IDENT "(" args ")"           # comptime 操作符
```

### 3.4 模式

```bnf
pattern     ::= "_"                         # 通配符
              | IDENT                       # 变量绑定
              | TYPE_IDENT pattern*         # 构造函数
              | literal                     # 字面量
              | "(" pattern ("," pattern)* ")"  # 元组模式
              | "[" "]"                      # 空列表
              | pattern "::" pattern        # list cons
              | "{" (IDENT "=" pattern)* "}"  # 记录模式
```

### 3.5 类型语法

```bnf
type        ::= type_atom ("->" type)?

type_atom   ::= "'" IDENT                   # 类型变量
              | IDENT                       # 类型构造子
              | type_atom IDENT             # 类型应用：'a list
              | "(" type ")"
              | "(" type ("*" type)+ ")"     # 元组类型
              | "{" (IDENT ":" type ";")* "}"  # 记录类型

# 特殊类型
'a ptr        # 指针类型
c_int         # C int 类型（FFI）
c_uint        # C unsigned int
c_long        # C long
```

---

## 4. 类型系统

### 4.1 基础类型

| 类型 | 说明 | 示例 |
|------|------|------|
| `int` | 64 位整数 | `42`, `-3` |
| `float` | 双精度浮点 | `3.14`, `-0.5` |
| `bool` | 布尔值 | `true`, `false` |
| `string` | 字符串 | `"hello"` |
| `unit` | 单元类型 | `()` |
| `'a ptr` | 指针类型 |  |
| `tensor` | 2D 浮点张量 |  |

### 4.2 复合类型

```fxsh
# 元组
(1, "hello", true)  : (int, string, bool)

# 列表
[1; 2; 3]           : int list

# 记录
{ name = "Alice"; age = 30 }  : { name: string; age: int }

# 函数
fn x -> x + 1       : int -> int
fn x y -> (x, y)    : 'a -> 'b -> ('a, 'b)
```

### 4.3 代数数据类型（ADT）

```fxsh
# 基础 ADT
type option = None | Some of int

# 泛型 ADT
type 'a option = None | Some of 'a
type 'a list = Nil | Cons of 'a * 'a list
type 'a result = Ok of 'a | Err of string
```

### 4.4 行多态记录

```fxsh
# 闭口记录
let p = { name = "Alice"; age = 30 }  : { name: string; age: int }

# 开口记录（行多态）
let get_name = fn r -> r.name   # { name: 'a | 'r } -> 'a

# 记录更新
let older = { p with age = 31 }
```

### 4.5 类型推导

- **Hindley-Milner 风格**：支持 let 多态
- **统一变量与替换**：维护类型变量映射
- **Occurs 检查**：防止无限类型
- **多态类型**：`'a -> 'a`, `('a -> 'b) -> 'a -> 'b`

### 4.6 类型注解

```fxsh
let x: int = 42
let f: int -> int = fn x -> x + 1

# 记录字段类型注解
let r = { name: string = "test"; value: int = 0 }
```

---

## 5. 语言特性

### 5.1 变量绑定

```fxsh
# 简单绑定
let x = 10

# 带类型注解
let y: int = 20

# 递归绑定
let rec fact = fn n -> if n <= 1 then 1 else n * fact (n - 1)

# let-in 表达式
let result = let a = 3 in let b = 4 in a + b
```

### 5.2 函数

```fxsh
# 匿名函数
fn x -> x + 1
fn x y -> x + y

# 柯里化
let add = fn x -> fn y -> x + y
let result = add 1 2  # 等价于 ((add 1) 2)

# 函数应用（空格分隔）
let n = add 1 2
```

### 5.3 条件表达式

```fxsh
let abs = fn x ->
  if x < 0 then -x else x

# if 表达式必须有 else 分支
if x > 0 then "positive"
else if x < 0 then "negative"
else "zero"
```

### 5.4 模式匹配

```fxsh
# ADT 匹配
let get_or = fn v fallback ->
  match v with
  | Some x -> x
  | None -> fallback
  end

# 字面量匹配
let label = fn n ->
  match n with
  | 0 -> "zero"
  | 1 -> "one"
  | _ -> "many"
  end

# 列表模式
let head_or = fn xs fallback ->
  match xs with
  | x :: _ -> x
  | [] -> fallback
  end

# 元组模式
let swap = fn p ->
  match p with
  | (a, b) -> (b, a)
  end

# 记录模式
let name = fn u ->
  match u with
  | { name = n } -> n
  end

# 带守卫
let classify = fn n ->
  match n with
  | x when x < 0 -> "negative"
  | 0 -> "zero"
  | _ -> "positive"
  end
```

### 5.5 管道操作

```fxsh
let double = fn x -> x * 2
let inc = fn x -> x + 1

let result = 5 |> double |> inc  # 7
```

### 5.6 模块系统

```fxsh
# 模块定义（花括号形式）
module Math = {
  let pi = 3.14159
  let sq = fn x -> x * x
}

# 模块导入
import Math
let area = Math.pi * Math.sq 5

# 点路径导入
import Pkg.Result
let status: Pkg.Result.flag = Pkg.Result.default

# 模块解析顺序：
# 1. a/b.fxsh
# 2. a.b.fxsh
```

### 5.7 Trait / Impl（显式接口）

```fxsh
# trait 定义（显式接口命名空间）
trait Eq = {
  let equal: Self -> Self -> bool
}

# impl 实现
impl Eq for int = {
  let equal x y = x == y
}

# 显式调用
let same = Eq.int.equal 1 1
```

**重要**：
- trait/impl 是显式 namespace 调用，不是隐式 typeclass 分派
- 必须通过 `TraitName.Type.method` 形式调用

### 5.8 Comptime（编译期求值）

```fxsh
# 编译期常量
let comptime MAX_SIZE = 1024 * 1024
let comptime DEBUG = true

# 编译期函数
let comptime make_tag = fn name -> "tag:" ++ name
let comptime LABEL = make_tag "build"

# 编译期条件
let comptime mode = if DEBUG then "debug" else "release"
```

### 5.9 类型反射

```fxsh
# 类型信息获取
let t = @typeOf({ id = 1, name = "fx" })

# 字段反射
let comptime fields = @fieldsOf(t)
let comptime has_id = @hasField(t, "id")

# 类型名称
let comptime type_name = @typeName(t)

# 类型判断
let comptime is_record = @isRecord(t)
let comptime is_tuple = @isTuple(t)

# 大小/对齐
let comptime size = @sizeOf(t)
let comptime align = @alignOf(t)
```

### 5.10 类型构造器

```fxsh
# 内置类型构造器
let comptime ints = @List(@typeOf(1))           # int list
let comptime maybe = @Option(@typeOf("fx"))     # string option
let comptime reply = @Result(@typeOf(1), @typeOf("err"))  # int string result
let comptime vec = @Vector(@typeOf(1))          # int vector

# 用户定义类型构造器
type 'a box = Box of 'a
let comptime boxed = @box(@typeOf(1))           # int box
```

### 5.11 Comptime 诊断

```fxsh
# 编译期日志
let _ = @compileLog("Processing...")

# 编译期错误
let _ = @compileError("Type mismatch!")

# 编译期 panic
let _ = @panic("Impossible!")
```

### 5.12 Comptime AST 操作

```fxsh
# 引用表达式
let quoted = @quote(1 + 2)

# 解引用 AST
let computed = @unquote(quoted)

# 拼接 AST
let expr = @splice(quoted)
```

### 5.13 Do 块（语法糖）

```fxsh
# Result 风格
let main = do {
  let! x = get_result ();
  let! y = another_op x;
  return y + 1
}

# Option 风格
let find_active = do {
  let? user = get_user ();
  let? profile = get_profile user.id;
  return profile.status
}
```

---

## 6. 标准库

### 6.1 内置函数

| 函数 | 类型 | 说明 |
|------|------|------|
| `print` | `string -> unit` | 打印到 stdout |
| `getenv` | `string -> string` | 获取环境变量 |
| `file_exists` | `string -> bool` | 检查文件是否存在 |
| `read_file` | `string -> string` | 读取文件内容 |
| `write_file` | `string -> string -> bool` | 写入文件 |
| `exec` | `string -> int` | 执行命令 |

### 6.2 Shell 运行时

```fxsh
# 执行命令
exec_code "sh -c \"exit 3\""           # 返回 exit code
exec_stdout "printf \"hello\\n\""       # 返回 stdout
exec_stderr "sh -c \"echo err 1>&2\""  # 返回 stderr

# 带 stdin 的执行
exec_stdin "cat" "hello"                # 通过 stdin 输入执行

# 管道
exec_pipe "printf \"x\\ny\\n\"" "grep y"  # 管道执行

# 捕获完整结果
exec_capture "sh -c \"echo out; echo err 1>&2\""
capture_code id      # 获取 exit code
capture_stdout id    # 获取 stdout
capture_stderr id    # 获取 stderr

# 高级捕获
exec_pipefail_capture "cmd1" "cmd2"    # pipefail 语义

# 文件操作
glob "examples/*.fxsh"                  # glob 匹配
grep_lines "^b.*$" "abc"               # 正则匹配
```

### 6.3 Shell 模块（stdlib/shell.fxsh）

```fxsh
import Shell

# 高级 API
let result = Shell.run "echo hello"
let code = Shell.code result
let out = Shell.stdout result
let err = Shell.stderr result

# 管道
let p2 = Shell.pipe "printf \"a\\nb\\n\"" "grep b"
let p3 = Shell.pipeline3 "cmd1" "cmd2" "cmd3"
```

### 6.4 JSON 运行时

```fxsh
# 解析/压缩
json_compact "{ \"key\": \"value\" }"

# 路径获取
json_get_string doc "agent.name"
json_get_bool doc "agent.ok"
json_get_int doc "count"
```

### 6.5 Tensor 运行时

```fxsh
# 创建张量
tensor_new2 2 3 0.0              # 2x3 零矩阵
tensor_from_list2 2 3 [1.0; 2.0; 3.0; 4.0; 5.0; 6.0]

# 操作
tensor_shape2 t                  # 返回形状 (rows, cols)
tensor_get2 t 1 1               # 获取元素
tensor_set2 t 1 1 9.0           # 设置元素（返回新张量）
tensor_add a b                  # 矩阵加法
tensor_dot a b                  # 矩阵乘法
```

### 6.6 标准库模块

```
stdlib/
├── prelude.fxsh    # 基础函数 (id, compose, pipe, flip...)
├── list.fxsh      # List.map, List.filter, List.fold...
├── option.fxsh   # Option.map, Option.unwrap_or...
├── result.fxsh   # Result.map, Result.bind...
├── string.fxsh   # String.split, String.join...
├── io.fxsh       # print, read_line...
├── os.fxsh       # 系统操作
├── fs.fxsh       # 文件系统
├── path.fxsh     # 路径操作
├── process.fxsh  # 进程操作
├── json.fxsh     # JSON 操作
├── regex.fxsh    # 正则表达式
├── shell.fxsh    # Shell 高级 API
└── system.fxsh   # 系统函数
```

---

## 7. FFI（C 互操作）

### 7.1 基础 FFI

```fxsh
# C 函数绑定
let c_puts : string -> c_int = "c:puts"
let c_labs : int -> int = "c:labs"
let c_atol : string -> int = "c:atol"

# 使用
c_puts "hello"
c_atol "12345"
```

### 7.2 C 整数类型

```fxsh
# 精确 C ABI 类型
c_int      # int
c_uint     # unsigned int
c_long     # long
c_ulong    # unsigned long
c_size     # size_t
c_ssize    # ssize_t

# 类型转换
int_to_c_int
c_int_to_int
int_to_c_uint
c_uint_to_int
```

### 7.3 指针操作

```fxsh
# 指针分配/释放
c_malloc 64           # 分配
c_free ptr            # 释放
c_null                # 空指针
c_cast_ptr p          # 类型转换

# 回调（顶层函数 -> C 回调指针）
let on_tick = fn _ -> ()
let cbp = c_callback0 on_tick
```

### 7.4 链接外部库

```bash
# 通过环境变量指定链接标志
FXSH_LDFLAGS='-luv' ./bin/fxsh --native-codegen example.fxsh
FXSH_CFLAGS='-I/opt/include' ./bin/fxsh example.fxsh
```

---

## 8. 执行模式

### 8.1 解释器模式（默认）

```bash
./bin/fxsh script.fxsh
```

### 8.2 Native 模式

```bash
./bin/fxsh --native script.fxsh
```

- 编译为 C 后通过 clang 执行
- 使用闭包安全的运行时 runner

### 8.3 Native Codegen 模式

```bash
./bin/fxsh --native-codegen script.fxsh
```

- 直接生成 C 代码
- 支持 FFI
- 支持更完整的代码生成

### 8.4 表达式求值

```bash
./bin/fxsh -e "1 + 2"
./bin/fxsh -e 'fn x -> x + 1'
```

### 8.5 调试选项

```bash
./bin/fxsh -t script.fxsh   # 打印 tokens
./bin/fxsh -h               # 查看帮助
./bin/fxsh --version        # 查看版本
```

---

## 9. 运算符优先级

| 优先级 | 运算符 | 结合性 |
|--------|--------|--------|
| 9 | 函数调用 `f x` | 左 |
| 8 | `.` 字段访问 | 左 |
| 7 | `-` (一元), `not` | 右 |
| 6 | `*`, `/`, `%` | 左 |
| 5 | `+`, `-`, `++` | 左 |
| 4 | `::` | 右 |
| 3 | `==`, `!=`, `<`, `>`, `<=`, `>=` | 无 |
| 2 | `and`, `or` | 左 |
| 1 | `\|>` | 左 |
| 0 | `->`, `=`, `if/then/else`, `let/in` | 右 |

---

## 10. 架构设计

### 10.1 编译 Pipeline

```
源码 (.fxsh)
    │
    ▼
┌──────────┐
│  Lexer   │  → Token[]
└──────────┘
    │
    ▼
┌──────────┐
│  Parser  │  → AST (Arena 分配)
└──────────┘
    │
    ▼
┌──────────────┐
│  Type Infer  │  → Typed AST + TypeEnv
│  (H-M + ADT) │
└──────────────┘
    │
    ▼
┌──────────────┐
│  Comptime    │  → 折叠 comptime 节点
│  Evaluator   │
└──────────────┘
    │
    ├─────────────────┐
    ▼                 ▼
┌──────────┐    ┌──────────┐
│Interpreter│    │ Codegen  │  → C 代码
└──────────┘    └──────────┘
```

### 10.2 内存管理

- **Arena 分配**：编译期所有 AST 节点、类型、token 通过 Arena 分配
- **零 GC 运行时**：脚本结束后 OS 回收全部内存
- **无 GC 停顿**：适合脚本场景

### 10.3 代码规模

| 组件 | 规模 |
|------|------|
| 核心实现 (src/ + include/) | ~19,166 行 |
| 总计（含测试/示例/文档） | ~23,022 行 |

---

## 11. 设计边界（1.0）

### 11.1 已明确不包含

| 特性 | 说明 |
|------|------|
| 隐式 typeclass | trait/impl 必须显式调用 |
| `$()` shell 语法 | 统一走 `exec_*` API |
| `run!` 宏 | 同上 |
| `try!` 宏 | 使用 `let!` do 块 |
| `cap!` 宏 | 使用 `exec_capture` |
| 复杂优化 | 优先语义清晰 |

### 11.2 仍在演进

| 特性 | 状态 |
|------|------|
| ANF IR | 计划中 |
| TCO | 部分支持（native-codegen） |
| 内联优化 | 计划中 |
| 更完整的错误报告 | 持续改进 |

---

## 12. 示例集合

### 12.1 基础示例

```fxsh
# Hello World
print "Hello, World!"

# 函数
let add = fn x y -> x + y
let result = add 1 2

# 列表
let xs = [1; 2; 3; 4; 5]
let doubled = List.map (fn x -> x * 2) xs

# 模式匹配
let head = fn xs ->
  match xs with
  | x :: _ -> Some x
  | [] -> None
  end
```

### 12.2 完整脚本示例

```fxsh
import Shell

# 定义类型
type status = Ok | Err of string

# 函数
let run_check = fn cmd ->
  let result = Shell.run cmd in
  if Shell.code result == 0 then
    Ok (Shell.stdout result)
  else
    Err (Shell.stderr result)
  end

# 使用
let check = run_check "ls -la"
match check with
| Ok output -> print output
| Err err -> eprint err
end
```

---

## 13. 附录

### 13.1 构建命令

```bash
make              # 编译项目
make debug=1      # 调试模式
make test         # 运行测试
make clean        # 清理
```

### 13.2 与 bash 对比

| bash | fxsh |
|------|------|
| `x=10` (无空格!) | `let x = 10` |
| `$((x + 5))` | `x + 5` |
| `function foo() { }` | `let foo = fn x -> ...` |
| `if [ "$x" -eq 10 ]; then` | `if x == 10 then ... end` |
| `for i in $(seq 1 10); do` | `for i in 1..10 do ... end` |

### 13.3 联系方式

- 项目主页：https://github.com/aresbit/fxsh
- 问题反馈：https://github.com/aresbit/fxsh/issues
