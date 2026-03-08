# fxsh Comptime 特性支持情况报告

> 更新日期：2026-03-09

---

## 一、已支持的特性（正常工作）

### 1. 基础编译期常量
```fxsh
let comptime a = 1
let comptime b = a + 2
let comptime c = a * b - 1
```
- 整数运算：`+`, `-`, `*`, `/`
- 浮点数运算
- 布尔运算

### 2. 编译期字符串
```fxsh
let comptime s1 = "hello"
let comptime s2 = " world"
let comptime s3 = s1 ++ s2  # "hello world"
```
- 字符串连接 `++`

### 3. 编译期布尔
```fxsh
let comptime b = true
let comptime nb = not b
```
- `not`, `and`, `or`

### 4. 编译期 if-then-else
```fxsh
let comptime x = if cond then "yes" else "no"
```
- 支持在 comptime 表达式中使用 if-then-else

### 5. 编译期函数
```fxsh
let comptime double = fn x -> x * 2
let comptime d = double 5  # 10

let comptime add = fn x -> fn y -> x + y
let comptime add10 = add 10
let comptime result = add10 5  # 15
```
- 单参数函数
- 柯里化多参数函数
- 字符串处理函数
- 支持闭包

### 6. @typeOf 类型反射
```fxsh
let t = @typeOf(42)
let t2 = @typeOf({ id = 1, name = "test" })
```
- 支持获取任意表达式的类型

### 7. @typeName 类型名称
```fxsh
let t = @typeOf(42)
let comptime name = @typeName(t)  # "int"
```

### 8. @fieldsOf 字段反射
```fxsh
let r = { id = 1, name = "test" }
let t = @typeOf(r)
let comptime fields = @fieldsOf(t)  # ["id", "name"]
```

### 9. @hasField 字段存在检查
```fxsh
let r = { id = 1 }
let t = @typeOf(r)
let comptime has_id = @hasField(t, "id")    # true
let comptime has_xxx = @hasField(t, "xxx") # false
```

### 10. @isRecord / @isTuple 类型判断
```fxsh
let r = { x = 1 }
let t = @typeOf(r)
let comptime is_rec = @isRecord(t)  # true

let tup = (1, "a")
let t2 = @typeOf(tup)
let comptime is_tup = @isTuple(t2)  # true
```

### 11. @sizeOf / @alignOf
```fxsh
let t = @typeOf(42)
let comptime sz = @sizeOf(t)    # 8
let comptime al = @alignOf(t)   # 8
```

### 12. @compileLog 编译期日志
```fxsh
let _ = @compileLog("message")
# 输出: [comptime] "message"
```

### 13. @quote / @unquote / @splice AST 操作
```fxsh
let comptime Q = @quote(1 + 2)
let comptime U = @unquote(Q)    # 3
let comptime S = @splice(@quote(40 + 2))  # 42
```

### 14. List 字面量
```fxsh
let comptime xs = [1, 2, 3]
```

### 15. Record 字面量
```fxsh
let comptime r = { x = 1, y = 2 }
```

### 16. Tuple 字面量
```fxsh
let comptime t = (1, "hello")
```

---

## 二、不支持或有限的特性

### 1. match 表达式
```fxsh
# ❌ 不支持 - 编译错误
let comptime test = fn x ->
  match x with
  | 0 -> "zero"
  | _ -> "other"
  end
```
**问题**：comptime 内使用 match 会导致 "Comptime expansion error: unknown error"

### 2. List 递归操作
```fxsh
# ❌ 不支持 - 编译错误
let comptime length = fn xs ->
  if xs == [] then 0
  else 1 + length xs[1:]
```
**问题**：无法对 list 进行递归处理

### 3. List 索引访问
```fxsh
# ✅ 部分支持
let xs = [1, 2, 3]
let comptime first = xs[0]  # 工作
```
**注意**：仅支持直接索引，不支持动态索引

### 4. @List / @Option / @Result 类型构造器
```fxsh
# ⚠️ 可能有兼容性问题，需要进一步测试
let comptime ints = @List(@typeOf(1))
let comptime maybe = @Option(@typeOf("fx"))
```

### 5. @compileError / @panic
```fxsh
# ⚠️ 未充分测试
let _ = @compileError("error message")
let _ = @panic("panic message")
```

### 6. let-in 表达式内的 match
```fxsh
# ❌ 不支持
let comptime x = 
  let y = 1 in
  match y with
  | 1 -> "one"
  | _ -> "other"
  end
```

---

## 三、SPEC 文档描述但未充分测试的特性

| 特性 | SPEC 描述 | 实际状态 |
|------|----------|----------|
| `@List(T)` | 内置类型构造器 | 需测试 |
| `@Option(T)` | 内置类型构造器 | 需测试 |
| `@Result(T, E)` | 内置类型构造器 | 需测试 |
| `@Vector(T)` | 内置类型构造器 | 需测试 |
| `@box(T)` | 用户定义类型构造器 | 需测试 |
| `@compileError` | 编译期报错中止 | 需测试 |
| `@panic` | 编译期 panic | 需测试 |
| `@jsonSchema` | JSON schema 生成 | 需测试 |

---

## 四、已知限制

1. **comptime 函数内不支持 match**：需要改用 if-then-else
2. **不支持 list 递归**：无法实现通用的 list 处理函数
3. **不支持在 let-in 内使用 match**：需要展平表达式

---

## 五、建议修复优先级

### 高优先级
1. **comptime 内支持 match 表达式** - 这是核心缺失
2. **comptime 内支持 list 递归** - 实现通用的编译期数据结构处理

### 中优先级
3. **@List/@Option/@Result/@Vector 类型构造器** - 完善类型元编程能力
4. **@compileError/@panic 诊断功能** - 增强编译期错误报告

### 低优先级
5. **@jsonSchema** - 特定用途的扩展
