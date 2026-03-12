# fxsh SQLite FFI Guide

更新时间：2026-03-12

本文档说明如何在 `fxsh` 中复用 `libsqlite3`，重点覆盖 `native-codegen` + FFI 的可用路径。

## 1. 运行前提

- 需要 `--native-codegen`
- 链接 sqlite：

```sh
FXSH_LDFLAGS='-lsqlite3' ./bin/fxsh --native-codegen your_file.fxsh
```

## 2. 建议导入

推荐直接导入声明模块：

```fxsh
import Sqlite
```

对应文件：`stdlib/sqlite.fxsh`

该模块提供：
- `sqlite3_open/close/prepare_v2/step/finalize`
- `sqlite3_bind_int/sqlite3_bind_text`
- `sqlite3_column_int/sqlite3_column_text`
- 常量：`sqlite_ok` / `sqlite_row` / `sqlite_done`
- 轻量辅助：`slot_alloc/slot_load/open_into/prepare_into/...`

## 3. 文本绑定安全（关键）

`sqlite3_bind_text` 的最后一个参数是析构器回调。若传 `NULL`，SQLite 可能引用临时内存。

`fxsh` 提供了 builtin：

```fxsh
c_sqlite_transient ()
```

它在 native-codegen 下映射为 `(void*)-1`（等价 `SQLITE_TRANSIENT`），可强制 SQLite 拷贝字符串数据。

示例：

```fxsh
let rc_b2 : int = c_int_to_int (
  Sqlite.sqlite3_bind_text stmt (int_to_c_int 2) "alice" (int_to_c_int (-1)) (c_sqlite_transient ())
)
```

## 4. 参考示例

- `examples/ffi_sqlite_open_close.fxsh`
- `examples/ffi_cdef_sqlite_open_close.fxsh`
- `examples/sqlite_query_min.fxsh`
- `examples/sqlite_crud_template.fxsh`
- `examples/sqlite_error_path.fxsh`

## 5. 列文本生命周期

`sqlite3_column_text` 返回的指针只在当前 `row` 有效，且通常在 `sqlite3_step/finalize` 后失效。  
因此应在 `finalize` 前消费该字符串（打印、比较、复制等）。

## 6. 回归测试

```sh
make test-sqlite-native
```
