# 半小时学会 fxsh

这份指南面向两类人：

- 会写 shell，但受不了 bash 的可维护性
- 会一点函数式语言，想用更稳的脚本语言处理文件、进程和数据

目标不是讲全语言，而是让你在 30 分钟内写出能用的 fxsh 脚本。

## 1. 先建立心智模型

把 fxsh 看成：

- 一个小型函数式语言
- 加上一套适合脚本场景的标准库和 builtin
- 有类型推导，但不要求你到处写类型

它不是：

- bash 兼容层
- 通用框架语言
- 隐式魔法很多的 typeclass 系统

最重要的三条：

1. `let` 绑定值
2. `fn` 定义函数
3. 表达式总会产生值

## 2. 第一个脚本

```fxsh
let add = fn x y -> x + y
let result = add 20 22
```

运行：

```sh
./bin/fxsh demo.fxsh
```

看版本：

```sh
./bin/fxsh --version
```

## 3. 基本语法

### 字面量

```fxsh
42
3.14
"hello"
true
()
```

### let 绑定

```fxsh
let name = "fxsh"
let answer = 42
```

### 函数

```fxsh
let inc = fn x -> x + 1
let add = fn x y -> x + y
```

调用是空格，不是逗号：

```fxsh
let n = add 1 2
```

### if 表达式

```fxsh
let abs = fn x ->
  if x < 0 then -x else x
```

### 管道

```fxsh
let double = fn x -> x * 2
let inc = fn x -> x + 1
let out = 5 |> double |> inc
```

## 4. 数据结构

### tuple

```fxsh
let pair = (1, "ok")
```

### list

```fxsh
let xs = [1; 2; 3]
```

### record

```fxsh
let user = { name = "alice", age = 30 }
let age = user.age
let older = { user with age = 31 }
```

## 5. 模式匹配

`match` 是 fxsh 的核心控制能力之一。

```fxsh
let label = fn n ->
  match n with
  | 0 -> "zero"
  | 1 -> "one"
  | _ -> "many"
  end
```

### ADT

```fxsh
type option = None | Some of int

let get_or = fn v fallback ->
  match v with
  | Some x -> x
  | None -> fallback
  end
```

### list 模式

```fxsh
let head_or = fn xs fallback ->
  match xs with
  | x :: _ -> x
  | _ -> fallback
  end
```

### record 模式

```fxsh
let user_name = fn u ->
  match u with
  | { name = n } -> n
  end
```

## 6. 模块与导入

本地文件和标准库都走 `import`。

```fxsh
import Pkg.Result

let status: Pkg.Result.flag = Pkg.Result.default
let code = Pkg.Result.to_int status
```

点路径导入 `import A.B` 会解析为：

- 优先 `a/b.fxsh`
- 回退 `a.b.fxsh`

模块引用也支持点路径：

```fxsh
Pkg.Result.default
Pkg.Result.On
Pkg.Result.flag
```

## 7. trait / impl

1.0 的 `trait` 是显式接口 namespace，不是隐式 typeclass。

```fxsh
trait Eq = {
  let equal: Self -> Self -> bool
}

impl Eq for int = {
  let equal x y = x == y
}

let same = Eq.int.equal 1 1
```

要点：

- 你显式调用 `Eq.int.equal`
- 不会自动按类型帮你挑实现
- 这对脚本语言更简单，也更可调试

## 8. comptime

`comptime` 用来做编译期常量、反射和简单元编程。

```fxsh
let comptime MAX = 1024 * 1024
let comptime DEBUG = true
```

### 类型反射

```fxsh
let t = @typeOf({ id = 1, name = "fx" })
let comptime fields = @fieldsOf(t)
let comptime has_id = @hasField(t, "id")
let comptime type_name = @typeName(t)
```

### 类型构造器

```fxsh
let comptime ints = @List(@typeOf(1))
let comptime maybe_name = @Option(@typeOf("fx"))
let comptime reply = @Result(@typeOf(1), @typeOf("err"))
```

## 9. FFI

`native-codegen` 路径可以直接绑定 C 符号。

```fxsh
let c_puts : string -> c_int = "c:puts"
let rc : int = c_int_to_int (c_puts "hello")
```

运行：

```sh
./bin/fxsh --native-codegen ffi_puts.fxsh
```

## 10. shell 能力

fxsh 不打算增加 bash 风格语法糖。进程能力统一走库层 API。

这意味着：

- 语言保持小
- shell 能力由 runtime / stdlib 提供
- parser 和类型系统不会因为 shell 语法继续膨胀

如果你看到 `exec_*`、`capture_*`、`glob` 这类 API，这是刻意设计。

## 11. 三条实践建议

### 1. 脚本先写成小函数

```fxsh
let parse_port = fn s -> ...
let validate = fn cfg -> ...
let run = fn cfg -> ...
```

### 2. 能用 ADT 就别用魔法字符串

差：

```fxsh
let mode = "ok"
```

好：

```fxsh
type mode = Ok | Fail
let mode = Ok
```

### 3. 把 comptime 用在“生成信息”，别用成第二套语言

适合：

- 常量
- schema
- 类型反射
- 小规模派生

不适合：

- 大段业务逻辑搬到编译期

## 12. 先看哪些例子

按顺序看这些文件：

1. `examples/hello.fxsh`
2. `examples/list_basic.fxsh`
3. `examples/match.fxsh`
4. `examples/record_basic.fxsh`
5. `examples/module_basic.fxsh`
6. `examples/import_path.fxsh`
7. `examples/comptime_type_ctor.fxsh`
8. `examples/trait_basic.fxsh`

## 13. 1.0 该记住的限制

- `trait/impl` 是显式调用，不是隐式分派
- shell 统一走库 API，不引入 `$()` 之类语言糖
- 优先命名类型、简单模块、稳定语义
- 复杂优化不是 1.0 的重点

这不是缺陷，而是 1.0 的边界控制。

## 14. 一句话总结

如果你会 OCaml/ML 风格表达式，又想写比 bash 更稳的脚本，`fxsh` 的正确打开方式是：

- 用 `let/fn/match/record/ADT`
- 用 `module/import` 组织脚本
- 用 `comptime` 做少量编译期工作
- 用显式 `trait` namespace 和 FFI，而不是追求过度自动化
