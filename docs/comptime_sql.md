# Comptime SQL 能力说明

更新时间：2026-03-09

本文档描述 `fxsh` 当前编译期 SQL 能力，覆盖 `@sqliteSQL(...)` 和 `@sql(...)` 的语义、字段约束与常见错误。

## 1. `@sqliteSQL(type_expr, "table")`

用途：基于 record 类型在编译期生成 SQL 模板。

输入约束：
- `type_expr` 必须可在编译期求值为 `type`，且该类型必须是 record。
- 第二个参数必须是字符串表名。

返回值：record，包含三个字符串字段：
- `create`：`CREATE TABLE IF NOT EXISTS ...`
- `insert`：`INSERT INTO ... VALUES (?, ?, ...)`
- `select`：`SELECT ... FROM ...`

字段规则：
- `int` / `bool` -> `INTEGER`
- `float` -> `REAL`
- `string` -> `TEXT`
- 其他 -> `TEXT`
- 非 `option` 字段自动附加 `NOT NULL`
- 字段名为 `id` 且映射后为 `INTEGER` 时，自动附加 `PRIMARY KEY`

典型错误：
- `@sqliteSQL expects a record type`

## 2. `@sql(dsl)`

用途：在编译期将 DSL record 转为 SQL 字符串。

输入约束：
- 可传 `string`（直接透传）。
- 或传 `record`，且必须包含 `op: string`。

通用错误：
- `@sql expects dsl record/string`
- `@sql dsl requires op:string`
- `@sql unknown op: ...`

错误码约定：
- SQL DSL 错误统一前缀 `SQL_E_*`
- `@sqliteSQL` 错误统一前缀 `SQLITE_E_*`
- 具体消息格式：`ERROR_CODE: 原始错误说明`

参数占位符校验：
- `@sql({...})` 可选字段 `param_count: int`（非负）
- 若提供 `param_count`，会在编译期校验 SQL 中 `?` 的数量
- 不一致时报错：`SQL_E_PARAM_COUNT_MISMATCH`
- `param_count` 类型不合法时报错：`SQL_E_PARAM_COUNT_TYPE`

可选 schema 校验：
- `@sql({...})` / `@sqlCheck({...})` 可选字段 `schema: type`
- `schema` 必须是 record 类型（通常写成 `schema = @typeOf(record_value)`）
- 当 `schema` 存在时会做列一致性检查：
  - `select.columns`
  - `insert.columns`
  - `update.set` 左侧列名
- 列不存在时报错：`SQL_E_SCHEMA_UNKNOWN_COLUMN`
- `schema` 类型非法时报错：`SQL_E_SCHEMA_TYPE`

## 3. `@sqlCheck(dsl)`

用途：只做编译期 DSL 校验，不输出 SQL 字符串。

行为：
- 复用 `@sql(dsl)` 的全部字段约束与错误规则（含 `param_count` 校验）
- 校验通过返回 `bool`：`true`
- 校验失败直接编译报错

### 2.1 `op = "select"`

必填：
- `columns: [string]`
- `from: string`

可选：
- `distinct: bool`
- `joins: [string]`
- `where: [string]`
- `where_mode: string`（默认 `AND`）
- `group_by: [string]`
- `having: [string]`
- `having_mode: string`（默认 `AND`）
- `window: [string]`
- `order_by: string`
- `limit: int`
- `offset: int`
- `union: [string]`
- `union_all: [string]`

典型错误：
- `@sql select requires columns:[string], from:string`

### 2.2 `op = "insert"`

必填：
- `table: string`
- `columns: [string]`

可选：
- `mode: string`
- `rows: [[string]]`
- `values: [string]`
- `select: string`
- `on_conflict: string`
- `returning: string | [string]`

约束：
- `rows` / `values` / `select` 互斥。
- `rows` 与 `columns` 列数必须逐行一致。
- `values` 长度必须与 `columns` 一致。

典型错误：
- `@sql insert requires table:string, columns:[string]`
- `@sql insert rows/values/select are mutually exclusive`
- `@sql insert row size must equal columns size`
- `@sql insert values must be [string] and match columns size`

### 2.3 `op = "update"`

必填：
- `table: string`
- `set: [string]`

可选：
- `mode: string`
- `from: string`
- `where: [string]`
- `where_mode: string`（默认 `AND`）
- `order_by: string`
- `limit: int`
- `returning: string | [string]`

典型错误：
- `@sql update requires table:string, set:[string]`

### 2.4 `op = "delete"`

必填：
- `table: string`

可选：
- `where: [string]`
- `where_mode: string`（默认 `AND`）
- `order_by: string`
- `limit: int`
- `returning: string | [string]`

典型错误：
- `@sql delete requires table:string`

### 2.5 `op = "create_table"`

必填：
- `table: string`
- `columns: [string]`（当 `as_select` 缺失时）

可选：
- `if_not_exists: bool`（默认 true）
- `temporary: bool`
- `constraints: [string]`
- `without_rowid: bool`
- `strict: bool`
- `as_select: string`

典型错误：
- `@sql create_table requires table:string`
- `@sql create_table requires columns:[string] when as_select is absent`

### 2.6 `op = "drop_table"`

必填：
- `table: string`

可选：
- `if_exists: bool`（默认 true）

典型错误：
- `@sql drop_table requires table:string`

### 2.7 `op = "create_index"`

必填：
- `name: string`
- `table: string`
- `columns: [string]`

可选：
- `unique: bool`
- `if_not_exists: bool`（默认 true）
- `where: [string]`
- `where_mode: string`（默认 `AND`）

典型错误：
- `@sql create_index requires name:string, table:string, columns:[string]`

### 2.8 `op = "pragma"`

必填：
- `name: string`

可选：
- `value: string | int | bool`

典型错误：
- `@sql pragma requires name:string`
- `@sql pragma value must be string/int/bool`

### 2.9 `op = "explain"`

必填：
- `query: string`

可选：
- `query_plan: bool`

典型错误：
- `@sql explain requires query:string`

### 2.10 `op = "copy"`

必填：
- `table: string`
- `from_select: string`

典型错误：
- `@sql copy requires table:string, from_select:string`

### 2.11 `op = "upsert"`

必填：
- `insert: string`
- `on_conflict: string`

典型错误：
- `@sql upsert requires insert:string, on_conflict:string`

### 2.12 `op = "raw"`

必填：
- `text: string`

典型错误：
- `@sql raw requires text:string`
