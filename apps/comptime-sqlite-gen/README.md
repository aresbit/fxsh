# Comptime SQLite SQL Generator

`fxsh` 的编译时 SQL 生成示例项目：根据 record 类型生成 SQL，并支持 `@sql(...)` DSL 生成复杂 SQLite 查询。

## 演示目标

- 从 `@typeOf(record)` 自动生成：
  - `CREATE TABLE`
  - `INSERT`
  - `SELECT`
- 用 `@sql(...)` DSL 生成高覆盖 SQLite 语句模板
- SQL 在编译期生成，运行期直接使用字符串常量。

## 运行

在仓库根目录执行：

```sh
make fxsh
./bin/fxsh apps/comptime-sqlite-gen/main.fxsh
```

## API

```fxsh
@sqliteSQL(type_expr, "table_name")
```

返回 record：

- `create`: `CREATE TABLE IF NOT EXISTS ...`
- `insert`: `INSERT INTO ... VALUES (?, ?, ...)`
- `select`: `SELECT ... FROM ...`

```fxsh
@sql({
  op = "select" | "insert" | "update" | "delete" |
       "create_table" | "drop_table" | "create_index" |
       "pragma" | "explain" | "copy" | "upsert" | "raw",
  ...
})
```

DSL 字段：

- `select`: `distinct:bool`, `columns:[string]`, `from:string`, 可选 `joins:[string]`, `where:[string]`, `group_by:[string]`, `having:[string]`, `window:[string]`, `order_by:string`, `limit:int`, `offset:int`, `union:[string]`, `union_all:[string]`
- `insert`: `mode:string`, `table:string`, `columns:[string]`, 可选 `values:[string]`/`rows:[[string]]`/`select:string`, `on_conflict:string`, `returning:string|[string]`
- `update`: `mode:string`, `table:string`, `set:[string]`, 可选 `from:string`, `where:[string]`, `order_by:string`, `limit:int`, `returning:string|[string]`
- `delete`: `table:string`, 可选 `where:[string]`, `order_by:string`, `limit:int`, `returning:string|[string]`
- `create_table`: `table:string`, 可选 `if_not_exists:bool`, `temporary:bool`, `columns:[string]`, `constraints:[string]`, `without_rowid:bool`, `strict:bool`, `as_select:string`
- `drop_table`: `table:string`, 可选 `if_exists:bool`
- `create_index`: `name:string`, `table:string`, `columns:[string]`, 可选 `unique:bool`, `if_not_exists:bool`, `where:[string]`
- `pragma`: `name:string`, 可选 `value:string|int|bool`
- `explain`: `query:string`, 可选 `query_plan:bool`
- `copy`: `table:string`, `from_select:string`
- `upsert`: `insert:string`, `on_conflict:string`
- `raw`: `text:string`

## 类型映射（SQLite）

- `int` / `bool` -> `INTEGER`
- `float` -> `REAL`
- `string` -> `TEXT`
- 其他类型 -> `TEXT`

额外规则：

- 非 `option` 字段默认 `NOT NULL`
- 字段名为 `id` 且类型为 `INTEGER` 时自动加 `PRIMARY KEY`
