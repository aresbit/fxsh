# Comptime SQLite SQL Generator

`fxsh` 的编译时 SQL 生成示例项目：根据 record 类型，在编译期生成 SQLite SQL。

## 演示目标

- 从 `@typeOf(record)` 自动生成：
  - `CREATE TABLE`
  - `INSERT`
  - `SELECT`
- 用 `@sql(...)` DSL 生成 `select/insert/update/delete/raw` 任意查询模板
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
  op = "select" | "insert" | "update" | "delete" | "raw",
  ...
})
```

DSL 字段：

- `select`: `columns:[string]`, `from:string`, 可选 `where:[string]`, `order_by:string`, `limit:int`
- `insert`: `table:string`, `columns:[string]`
- `update`: `table:string`, `set:[string]`, 可选 `where:[string]`
- `delete`: `table:string`, 可选 `where:[string]`
- `raw`: `text:string`

## 类型映射（SQLite）

- `int` / `bool` -> `INTEGER`
- `float` -> `REAL`
- `string` -> `TEXT`
- 其他类型 -> `TEXT`

额外规则：

- 非 `option` 字段默认 `NOT NULL`
- 字段名为 `id` 且类型为 `INTEGER` 时自动加 `PRIMARY KEY`
