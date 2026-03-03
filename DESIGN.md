# fxsh — 详细设计文档 v0.2

> Functional Core Minimal Shell Script Language  
> 目标：让 OCaml 初学者能用函数式风格写替代 bash 的脚本

---

## 目录

1. [语言规范](#1-语言规范)
2. [架构总览](#2-架构总览)
3. [Arena GC 设计](#3-arena-gc-设计)
4. [词法分析](#4-词法分析)
5. [语法规范（BNF）](#5-语法规范bnf)
6. [类型系统](#6-类型系统)
7. [IR 中间表示](#7-ir-中间表示)
8. [代码生成](#8-代码生成)
9. [标准库](#9-标准库)
10. [当前 Bug 修复清单](#10-当前-bug-修复清单)
11. [实现路线图](#11-实现路线图)

---

## 1. 语言规范

### 1.1 设计原则

| 原则 | 说明 |
|------|------|
| **函数式核心** | 所有值默认不可变，副作用显式标注 |
| **类型安全** | Hindley-Milner 推导，无隐式类型转换 |
| **脚本友好** | 直接调用系统命令，管道操作一等公民 |
| **零 GC 开销** | Arena 分配，脚本结束 OS 回收，无 GC 停顿 |
| **可读性优先** | 比 bash 更接近自然语言 |

### 1.2 完整语言特性

```fxsh
# ── 字面量 ──────────────────────────────────
42          # int
3.14        # float
"hello"     # string
true        # bool
()          # unit
[1; 2; 3]   # list  (分号分隔，区别于 tuple 的逗号)
(1, "a")    # tuple

# ── 变量绑定 ────────────────────────────────
let x = 10
let y : int = x + 5

# ── 函数 ────────────────────────────────────
let add = fn x y -> x + y
let rec fact = fn n -> if n <= 1 then 1 else n * fact (n - 1)

# ── let-in 表达式 ───────────────────────────
let result = let a = 3 in let b = 4 in a + b

# ── 条件 ────────────────────────────────────
let abs = fn x -> if x >= 0 then x else -x

# ── 管道 ────────────────────────────────────
[1; 2; 3] |> List.map (fn x -> x * 2) |> List.filter (fn x -> x > 2)

# ── 模式匹配 ────────────────────────────────
let head = fn xs ->
  match xs with
  | [] -> None
  | x :: _ -> Some x

# ── ADT ─────────────────────────────────────
type 'a option = None | Some of 'a
type 'a list   = Nil  | Cons of 'a * 'a list
type 'a tree   = Leaf | Node of 'a tree * 'a * 'a tree

# ── 记录类型 ────────────────────────────────
type person = { name : string; age : int }
let alice = { name = "Alice"; age = 30 }
let older = { alice with age = alice.age + 1 }

# ── 模块 ────────────────────────────────────
module Math = {
  let pi = 3.14159
  let sq = fn x -> x * x
}
let area = Math.pi * Math.sq 5

# ── IO Monad (副作用隔离) ───────────────────
let main = do {
  print "Hello";
  let! line = read_line ();
  print ("You said: " ++ line)
}

# ── Shell 集成 ──────────────────────────────
let files = $(ls -la)          # 捕获命令输出为 string list
let _ = run! "git commit -m 'fix'"  # 执行命令，失败抛异常
let ok = try! "ping -c1 8.8.8.8"   # 执行命令，返回 bool

# ── comptime ────────────────────────────────
let comptime MAX = 1024 * 1024
let comptime platform = @os ()    # "linux" | "darwin" | "windows"
```

### 1.3 运算符优先级（从高到低）

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

## 2. 架构总览

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
    ▼
┌──────────────┐
│  IR Lower    │  → fxsh IR (ANF 范式)
└──────────────┘
    │
    ▼
┌──────────────┐
│  C Codegen   │  → .c 文件
└──────────────┘
    │
    ▼
┌──────────────┐
│  clang/gcc   │  → 可执行文件
└──────────────┘
```

### 2.1 编译 Pipeline 数据流

```
fxsh_arena_t *arena          // 全局 Arena，贯穿整个编译期
    │
    ├── fxsh_lexer_t          // 复用 source buffer，token 存 arena
    ├── fxsh_parser_t         // AST 节点全部 arena_alloc
    ├── fxsh_type_env_t       // 类型环境存 arena
    ├── fxsh_comptime_ctx_t   // comptime 值存 arena
    └── fxsh_codegen_ctx_t    // 输出 buffer 用独立 arena
```

---

## 3. Arena GC 设计

### 3.1 设计目标

脚本场景的内存特征：
- 编译期：分配大量小对象（AST 节点、类型、token）
- 运行时：脚本跑完即退出，OS 回收全部内存
- 无需精确 GC，不需要 mark-sweep/reference counting

### 3.2 分段嵌套 Arena

```c
// 单个 Arena 段（Segment）
typedef struct fxsh_arena_seg {
    struct fxsh_arena_seg *next;   // 链表下一段
    size_t                 cap;    // 本段总容量
    size_t                 used;   // 已用字节
    char                   data[]; // 柔性数组，数据紧跟其后
} fxsh_arena_seg_t;

// Arena 控制块
typedef struct fxsh_arena {
    fxsh_arena_seg_t *head;        // 当前活跃段（最新分配的）
    fxsh_arena_seg_t *full;        // 已满段链表（备用回溯）
    struct fxsh_arena *parent;     // 嵌套父 Arena（Scope Stack）
    size_t             seg_size;   // 默认段大小（64KB）
    size_t             total;      // 总分配字节统计
} fxsh_arena_t;
```

### 3.3 嵌套语义（Scope Stack）

```
main_arena          ← 全局，整个编译期存活
  ├── parse_arena   ← Parser 用，parse 完即 pop（但 AST 提升到 main）
  ├── type_arena    ← Type inference 临时类型变量
  │     └── unify_arena  ← 单次 unify 用，失败直接丢弃
  └── codegen_arena ← 代码生成输出 buffer
```

```c
// 使用示例
fxsh_arena_t *a = arena_create(NULL, 64 * 1024);   // 64KB 段
fxsh_arena_t *child = arena_create(a, 4 * 1024);   // 子 arena 4KB 段

void *p = arena_alloc(a, sizeof(MyStruct));
arena_alloc_zero(child, 128);

arena_destroy(child);  // 释放 child 的所有段
// 注意：如果 child 中有指针指向 main，需要确保 main 存活更长
```

### 3.4 Arena API

```c
// ── 生命周期 ────────────────────────────────────────────────
fxsh_arena_t *arena_create(fxsh_arena_t *parent, size_t seg_size);
void          arena_destroy(fxsh_arena_t *a);
void          arena_reset(fxsh_arena_t *a);   // 不释放段，只重置 used

// ── 分配 ────────────────────────────────────────────────────
void *arena_alloc(fxsh_arena_t *a, size_t size);
void *arena_alloc_zero(fxsh_arena_t *a, size_t size);
void *arena_alloc_align(fxsh_arena_t *a, size_t size, size_t align);
char *arena_strdup(fxsh_arena_t *a, const char *s);
char *arena_strndup(fxsh_arena_t *a, const char *s, size_t n);

// ── 标记/回滚（用于 unification 失败回滚）────────────────────
typedef struct { fxsh_arena_seg_t *seg; size_t used; } arena_mark_t;
arena_mark_t arena_mark(fxsh_arena_t *a);
void         arena_restore(fxsh_arena_t *a, arena_mark_t mark);

// ── 统计 ─────────────────────────────────────────────────────
size_t arena_used(fxsh_arena_t *a);
size_t arena_total_segs(fxsh_arena_t *a);
```

### 3.5 关键实现细节

```c
// 对齐分配（默认 8 字节对齐）
#define ARENA_ALIGN 8
#define ARENA_ALIGN_UP(n) (((n) + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1))

void *arena_alloc(fxsh_arena_t *a, size_t size) {
    size = ARENA_ALIGN_UP(size);

    // 尝试在当前段分配
    if (a->head && a->head->used + size <= a->head->cap) {
        void *ptr = a->head->data + a->head->used;
        a->head->used += size;
        a->total += size;
        return ptr;
    }

    // 当前段放不下，分配新段
    size_t seg_cap = size > a->seg_size ? size : a->seg_size;
    fxsh_arena_seg_t *seg = malloc(sizeof(fxsh_arena_seg_t) + seg_cap);
    if (!seg) return NULL;

    seg->cap  = seg_cap;
    seg->used = size;
    seg->next = a->head;  // 新段插到链表头
    a->head   = seg;
    a->total += size;
    return seg->data;
}
```

### 3.6 与现有 sp.h 的集成

当前代码用 `sp_alloc`/`sp_free`，需要将 Arena 透传给所有分配器：

```c
// 全局线程局部 Arena（简单方案）
static _Thread_local fxsh_arena_t *g_current_arena = NULL;

#define arena_push(a) fxsh_arena_t *_prev_arena = g_current_arena; g_current_arena = (a)
#define arena_pop()   g_current_arena = _prev_arena

// 替换 sp_alloc
#define fxsh_alloc(size)   arena_alloc(g_current_arena, size)
#define fxsh_alloc0(size)  arena_alloc_zero(g_current_arena, size)
```

---

## 4. 词法分析

### 4.1 当前已实现（✓）

- 整数、浮点数字面量
- 字符串（含转义序列）
- 标识符、关键字、类型标识符（大写开头）
- 所有运算符
- 注释（`#`）
- 类型变量（`'a`）

### 4.2 待实现

```
# 多行字符串
let sql = """
  SELECT *
  FROM users
  WHERE id = 42
"""

# 字符串插值
let msg = f"Hello {name}, you are {age} years old"

# 字节字符串
let bytes = b"\x00\x01\x02"

# 原始字符串
let path = r"C:\Users\foo\bar"
```

### 4.3 修复：列号计算

当前 `make_loc` 中列号计算正确，但 `advance_line` 后 `cursor` 应先移动再重置 `line_start`：

```c
// 修复前（BUG：先重置导致首字符列号错误）
static inline void advance_line(fxsh_lexer_t *lexer) {
    lexer->line++;
    lexer->line_start = lexer->cursor;  // cursor 尚未移过 '\n'
}

// 修复后
// 在 fxsh_lexer_next 中处理 '\n' 时：
if (c == '\n') {
    fxsh_loc_t loc = make_loc(lexer);
    advance(lexer);        // 先移过 '\n'
    lexer->line++;
    lexer->line_start = lexer->cursor;  // 再重置行首
    ...
}
```

---

## 5. 语法规范（BNF）

```bnf
program     ::= decl* EOF

decl        ::= let_decl
              | type_decl
              | module_decl
              | import_decl
              | expr NEWLINE

let_decl    ::= "let" "rec"? "comptime"? IDENT (":" type)? "=" expr
              | "let" "rec"? IDENT param+ (":" type)? "=" expr  (* 函数糖 *)

type_decl   ::= "type" type_params? IDENT "=" type_body
type_params ::= "'" IDENT+
type_body   ::= variant ("|" variant)*   (* ADT *)
              | "{" field (";" field)* "}"  (* 记录类型 *)
variant     ::= IDENT ("of" type ("*" type)*)?
field       ::= IDENT ":" type

module_decl ::= "module" TYPE_IDENT "=" "{" decl* "}"
import_decl ::= "import" TYPE_IDENT ("." TYPE_IDENT)*

expr        ::= pipe_expr

pipe_expr   ::= logical ("|>" logical)*

logical     ::= comparison (("and" | "or") comparison)*

comparison  ::= additive (("==" | "!=" | "<" | ">" | "<=" | ">=") additive)?

additive    ::= multiplicative (("+"|"-"|"++") multiplicative)*

multiplicative ::= unary (("*"|"/"|"%") unary)*

unary       ::= ("-" | "not") unary
              | app_expr

app_expr    ::= postfix postfix*    (* 函数应用，左结合 *)

postfix     ::= primary ("." IDENT)*
              | primary "(" args ")"

primary     ::= INT_LIT | FLOAT_LIT | STRING_LIT | "true" | "false" | "()"
              | IDENT | TYPE_IDENT
              | "(" expr ("," expr)* ")"   (* 括号 or 元组 *)
              | "[" (expr (";" expr)*)? "]" (* 列表 *)
              | "{" field_init (";" field_init)* "}"  (* 记录 *)
              | "{" expr "with" field_init (";" field_init)* "}"  (* 记录更新 *)
              | "fn" param+ "->" expr
              | "let" (let_binding)+ "in" expr
              | "if" expr "then" expr ("else" expr)?
              | "match" expr "with" ("|" pattern "->" expr)+
              | "do" "{" do_stmt+ "}"
              | "@" IDENT "(" args ")"     (* comptime operator *)
              | "$(" shell_cmd ")"         (* shell 命令捕获 *)

param       ::= IDENT | "(" IDENT ":" type ")" | "_"

pattern     ::= "_"
              | IDENT
              | TYPE_IDENT pattern*
              | INT_LIT | FLOAT_LIT | STRING_LIT | "true" | "false"
              | "(" pattern ("," pattern)* ")"
              | "[" "]"
              | pattern "::" pattern
              | "{" IDENT (";" IDENT)* "}"

type        ::= type_atom ("->" type)?
type_atom   ::= "'" IDENT           (* 类型变量 *)
              | IDENT               (* 类型构造子 *)
              | type_atom IDENT     (* 类型应用：'a list *)
              | "(" type ")"
              | "(" type ("*" type)+ ")"  (* 元组类型 *)
              | "{" (IDENT ":" type ";")* "}"  (* 记录类型 *)

do_stmt     ::= "let!" IDENT "=" expr ";"   (* monadic bind *)
              | expr ";"
```

---

## 6. 类型系统

### 6.1 类型规则补全

#### 记录类型（行多态）

```
Γ ⊢ e₁ : {l : τ₁, ρ}    Γ ⊢ e₂ : τ₂
─────────────────────────────────────
Γ ⊢ {e₁ with l = e₂} : {l : τ₂, ρ}
```

#### ADT 构造器

```
Γ(C) = τ₁ → ... → τₙ → T α⃗
Γ ⊢ eᵢ : τᵢ[α⃗ ↦ σ⃗]  (i=1..n)
───────────────────────────────
Γ ⊢ C e₁ ... eₙ : T σ⃗
```

#### 模式匹配完整性

编译器需要检查：
1. **完整性（Exhaustiveness）**：所有情况被覆盖
2. **可达性（Reachability）**：没有无法触达的分支

简化版检查算法（基于 Maranget 2007 useful clause matrix decomposition）：

```ocaml
(* 伪代码 - 检查 match 完整性 *)
let check_exhaustive ty arms =
  let missing = compute_missing ty arms in
  if missing <> [] then
    warn "Non-exhaustive match, missing: %s" (show_patterns missing)
```

### 6.2 类型环境数据结构

当前代码用 `sp_ht(sp_str_t, fxsh_scheme_t)`，存在的问题：
- 哈希表 key 是 `sp_str_t`（含指针），哈希函数需要按内容哈希
- 现在 `constr_env_bind` 调用了不存在的 `sp_ht_ensure`/`sp_ht_set_fns`

**修复方案**：改为简单的链表环境（函数式风格，天然支持 shadowing）：

```c
typedef struct fxsh_type_env_node {
    sp_str_t                  name;
    fxsh_scheme_t            *scheme;
    struct fxsh_type_env_node *next;
} fxsh_type_env_node_t;

typedef fxsh_type_env_node_t *fxsh_type_env_t;

static fxsh_type_env_t type_env_extend(fxsh_arena_t *a,
                                        fxsh_type_env_t env,
                                        sp_str_t name,
                                        fxsh_scheme_t *scheme) {
    fxsh_type_env_node_t *node = arena_alloc(a, sizeof(*node));
    node->name   = name;
    node->scheme = scheme;
    node->next   = env;
    return node;
}

static fxsh_scheme_t *type_env_lookup(fxsh_type_env_t env, sp_str_t name) {
    for (; env; env = env->next)
        if (sp_str_equal(env->name, name))
            return env->scheme;
    return NULL;
}
```

### 6.3 Substitution 性能优化

当前 `compose(s1, s2)` 每次都创建新数组，O(n²) 累积。
对于脚本场景（类型变量不多）可接受，但建议改为**持久化映射**：

```c
// 用链式 subst，避免复制
typedef struct fxsh_subst_node {
    fxsh_type_var_t         var;
    fxsh_type_t            *type;
    struct fxsh_subst_node *next;
} fxsh_subst_node_t;

typedef fxsh_subst_node_t *fxsh_subst_t;
```

---

## 7. IR 中间表示

引入 ANF（A-Normal Form）IR，简化代码生成：

### 7.1 ANF 定义

```c
// ANF: 所有函数调用参数必须是原子值
// 复杂表达式被展开为 let 链

typedef enum {
    IR_LIT_INT, IR_LIT_FLOAT, IR_LIT_BOOL, IR_LIT_STRING, IR_LIT_UNIT,
    IR_VAR,
    IR_LET,        // let x = val in cont
    IR_IF,         // if aval then expr else expr
    IR_CALL,       // f a1 a2 ... (所有参数是原子值)
    IR_LAMBDA,     // fn x -> expr
    IR_MATCH,      // match val with arms
    IR_CONSTR,     // Constructor 应用
    IR_FIELD,      // 字段访问
    IR_RECORD,     // 记录构造
    IR_PRIM,       // 原始操作 (+, -, *, ...)
} fxsh_ir_kind_t;

typedef struct fxsh_ir fxsh_ir_t;

struct fxsh_ir {
    fxsh_ir_kind_t kind;
    fxsh_type_t   *type;    // 类型信息（来自类型推导）
    union {
        s64          lit_int;
        f64          lit_float;
        bool         lit_bool;
        sp_str_t     lit_string;
        sp_str_t     var;
        struct { sp_str_t name; fxsh_ir_t *val; fxsh_ir_t *cont; } let;
        struct { fxsh_ir_t *cond; fxsh_ir_t *then_; fxsh_ir_t *else_; } if_;
        struct { sp_str_t func; fxsh_ir_t **args; u32 argc; } call;
        struct { sp_str_t param; fxsh_ir_t *body; } lambda;
        struct { fxsh_ir_t *val; fxsh_ir_match_arm_t *arms; u32 n_arms; } match;
        struct { sp_str_t constr; fxsh_ir_t **args; u32 argc; } constr;
        struct { fxsh_ir_t *obj; sp_str_t field; } field;
        struct { fxsh_ir_field_t *fields; u32 n_fields; } record;
        struct { fxsh_token_kind_t op; fxsh_ir_t *left; fxsh_ir_t *right; } prim;
    } data;
};
```

### 7.2 AST → ANF 转换示例

```fxsh
# 源码
let z = f (g x) (h y)
```

```c
// ANF
let tmp1 = g x in
let tmp2 = h y in
let z    = f tmp1 tmp2 in ...
```

---

## 8. 代码生成

### 8.1 类型映射

| fxsh 类型 | C 类型 |
|-----------|--------|
| `int` | `int64_t` |
| `float` | `double` |
| `bool` | `bool` |
| `string` | `fxsh_str_t` (ptr + len) |
| `unit` | `void` / `int` 0 |
| `'a list` | `fxsh_list_t *` (tagged union linked list) |
| `(a, b)` | `fxsh_tuple_t *` |
| ADT | `fxsh_<name>_t` (tag + union) |
| `'a -> 'b` | 闭包结构体 `fxsh_closure_t *` |

### 8.2 闭包表示

```c
// 闭包（用于 lambda 和 curried functions）
typedef struct {
    void  *fn_ptr;      // 函数指针
    void **captures;    // 捕获变量数组
    u32    n_captures;  // 捕获数量
    u32    arity;       // 参数数量
} fxsh_closure_t;

// 闭包调用约定
// 每个 lambda 生成两个函数：
// 1. 直接版：fxsh_fn_<name>(<params>, fxsh_closure_t *env)
// 2. 通用版：void* fxsh_apply(fxsh_closure_t *cl, void *arg)
```

### 8.3 当前 codegen.c 的 Bug：`embed_raw`

```c
// BUG (codegen.c line ~268)
embed_raw(ctx, "_t;\n\n");   // ← embed_raw 不存在！

// FIX
emit_raw(ctx, "_t;\n\n");
```

### 8.4 模式匹配代码生成

使用决策树算法（比简单的链式 if-else 更高效）：

```c
// 对于 ADT 匹配，生成 switch on tag
switch (val.tag) {
    case fxsh_tag_option_None:
        /* None 分支 */
        break;
    case fxsh_tag_option_Some:
        int64_t x = val.data.Some._0;
        /* Some x 分支 */
        break;
}
```

---

## 9. 标准库

### 9.1 核心模块

```
stdlib/
├── List.fxsh       # map, filter, fold, zip, flatten...
├── Option.fxsh     # map, bind, get_or, is_some...
├── Result.fxsh     # map, bind, map_err, unwrap...
├── String.fxsh     # split, join, trim, contains, replace...
├── Int.fxsh        # to_string, parse, min, max, abs...
├── Float.fxsh      # to_string, parse, floor, ceil, round...
├── IO.fxsh         # print, println, read_line, read_file...
├── Path.fxsh       # join, dirname, basename, exists, is_file...
├── Process.fxsh    # run, capture, env_get, env_set, exit...
└── Sys.fxsh        # os, arch, hostname, cwd, args...
```

### 9.2 Shell 集成 API

```fxsh
# Process 模块
let run!   : string -> unit        # 运行，失败抛异常
let try!   : string -> bool        # 运行，返回成功/失败
let cap!   : string -> string      # 捕获 stdout
let cap_lines! : string -> string list  # 捕获 stdout 按行

# IO 模块
let print   : string -> unit
let println : string -> unit
let eprint  : string -> unit       # stderr
let read_line : unit -> string
let read_file : string -> string
let write_file : string -> string -> unit
```

---

## 10. 当前 Bug 修复清单

### 10.1 codegen.c

| 位置 | Bug | 修复 |
|------|-----|------|
| `gen_type_def_struct` ~L268 | `embed_raw` → 应为 `emit_raw` | 直接替换 |
| `gen_decl_fn` | 所有参数硬编码为 `s64`，忽略类型信息 | 从 TypeEnv 查询 |
| `gen_lambda` | 直接返回 NULL，未生成闭包 | 需实现闭包提升 |
| `gen_match` | 未实现 | 实现 tag switch |

### 10.2 types.c

| 位置 | Bug | 修复 |
|------|-----|------|
| `constr_env_bind` | 调用不存在的 `sp_ht_ensure`/`sp_ht_set_fns` | 改用链表环境 |
| `free_vars_in_env` | 函数体为空，导致泛化错误 | 实现遍历 |
| `infer_pattern` `AST_PAT_LIT` | kind 已是 PAT_LIT，无法区分字面量类型 | 在 literal 存 kind 字段 |
| `type_env_bind` | 修改传入 env 指针（不安全） | 改为返回新 env |

### 10.3 comptime.c

| 位置 | Bug | 修复 |
|------|-----|------|
| `eval_let_in` | 检查 `AST_DECL_LET` 但 let_in bindings 实际是 `AST_LET` | 统一节点类型 |
| `fxsh_ct_list` | 参数 `fxsh_ct_value_t **items` 实为 SP_NULLPTR，未初始化 | 传 NULL 前检查 |

### 10.4 parser.c

| 位置 | Bug | 修复 |
|------|-----|------|
| `parse_type_def` | type 参数解析（`'a`）检查 `data[0] == '\''` 但 lexer 已去掉引号 | 修复引号处理或 token kind |
| `parse_let_decl` | 只支持 `IDENT`，不支持函数定义糖 `let f x y = ...` | 添加多参数解析 |
| `parse_primary` `TOK_TYPE_IDENT` | 构造器参数解析逻辑不停止条件有缺漏 | 精化停止条件 |

### 10.5 lexer.c

| 位置 | Bug | 修复 |
|------|-----|------|
| `advance_line` | 在 newline 移动前重置 `line_start` | 先 advance 再重置 |
| 类型变量 `'a` | 当遇到 `'let`、`'in` 等时返回 IDENT 而非关键字变量 | 检查 `'` 后是否是字母序列 |

---

## 11. 实现路线图

### Phase 1 — 修复稳定（当前）

- [x] Lexer 基础实现
- [x] Parser 递归下降
- [x] H-M 类型推导框架
- [x] comptime 求值基础
- [x] C codegen 框架
- [ ] **Arena GC 替换 sp_alloc**
- [ ] **修复上述所有 Bug**
- [ ] **链表型 TypeEnv 替换 HashMap**

### Phase 2 — 功能完整

- [ ] 闭包捕获分析 + 代码生成
- [ ] 完整模式匹配（含决策树）
- [ ] 记录类型 + row polymorphism
- [ ] 完整错误报告（带颜色和位置）
- [ ] List/Option/Result 标准库

### Phase 3 — Shell 集成

- [ ] `$()` 命令捕获语法
- [ ] `run!`/`try!`/`cap!` IO 原语
- [ ] 字符串插值 `f"...{expr}..."`
- [ ] 管道到系统命令 `|> sh "grep foo"`

### Phase 4 — 优化

- [ ] ANF IR 层
- [ ] 尾调用优化
- [ ] 内联展开
- [ ] comptime 完整支持（含泛型类型编程）
