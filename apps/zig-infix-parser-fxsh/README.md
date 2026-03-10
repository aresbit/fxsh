# zig-infix-parser (fxsh port)

这是对 `https://github.com/markisus/zig-infix-parser` 的 `fxsh` 复刻实验，用来测试语言能力边界。

当前版本实现了：

- token 化：变量、整数/数字字面量、`+ - * ( )`
- 中缀转后缀：shunting-yard
- AST 构建：`Ref/Lit/Add/Sub/Mul`
- 运行时求值：通过 `binding list` 绑定变量
- 内置回归：3 条表达式测试（含原仓库示例）

## 运行

在仓库根目录执行：

```sh
make fxsh
./bin/fxsh apps/zig-infix-parser-fxsh/main.fxsh
```

预期输出：

- `[PASS] parse1`
- `[PASS] parse2`
- `[PASS] parse3`
- `all tests passed`

注：当前 fxsh 类型系统的算术运算符以 `int` 为主，因此此复刻版本默认用整型样例数据。

## 与 Zig 原版差异

- Zig 原版核心亮点是 `CompileExpression` 在 **comptime** 产出一个可 `eval` 的类型。
- 这个 `fxsh` 版本目前是 **运行时编译 + 运行时求值**。
- 也就是说：算法复刻了，但“类型级 compile-time AST 生成”还不是 1:1 对标。
