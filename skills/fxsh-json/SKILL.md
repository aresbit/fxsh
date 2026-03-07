---
name: fxsh-json
description: 使用 fxsh JSON 运行时（json_validate/json_compact/json_get/json_get_*）进行结构化数据处理。
---

# FXSH JSON

已提供能力：
- `json_validate : string -> bool`
- `json_compact : string -> string`
- `json_has : string -> string -> bool`
- `json_get : string -> string -> string`
- `json_get_string/int/float/bool`

路径语法：
- 对象字段：`a.b.c`
- 数组索引：`arr[0].x`

实践建议：
1. 先 `json_validate` 再读取字段
2. 对输入先做 `json_compact` 归一化
3. 数值/布尔读取前优先 `json_has` 判定
