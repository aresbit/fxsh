#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

cases="examples/hello.fxsh examples/pipeline.fxsh examples/factorial.fxsh examples/match.fxsh examples/match_guard_min.fxsh examples/match_boxed_ctor_guard.fxsh examples/match_literal.fxsh examples/match_string.fxsh examples/match_ctor_literal.fxsh examples/match_ctor_nested.fxsh examples/match_ctor_record_tuple_list.fxsh examples/match_boxed_ctor_nested.fxsh examples/match_record.fxsh examples/match_record_literal.fxsh examples/match_record_basic_types.fxsh examples/match_tuple.fxsh examples/match_tuple_literal.fxsh examples/match_tuple_basic_types.fxsh examples/list_basic.fxsh examples/list_pattern_literal.fxsh examples/tensor_basic.fxsh examples/transformer_shape_safe.fxsh stdlib/list.fxsh stdlib/system.fxsh stdlib/path.fxsh stdlib/process.fxsh stdlib/text.fxsh stdlib/bytes.fxsh stdlib/fs.fxsh stdlib/json.fxsh examples/adt.fxsh examples/regression_stage2.fxsh examples/closure_make_adder.fxsh examples/closure_lexical_scope.fxsh examples/closure_recursion.fxsh examples/closure_codegen_letin.fxsh examples/closure_codegen_letin_chain.fxsh examples/closure_codegen_letin_alias_chain.fxsh examples/type_annotation_ok.fxsh examples/type_annotation_letin_ok.fxsh examples/record_basic.fxsh examples/record_row_poly.fxsh examples/record_type_annotation_open.fxsh examples/record_helper_native.fxsh examples/closure_curried_generic.fxsh examples/module_basic.fxsh examples/module_brace.fxsh examples/module_result_api.fxsh examples/module_adt.fxsh examples/import_path.fxsh examples/app_arg_nested.fxsh examples/ctor_tuple_payload.fxsh examples/trait_basic.fxsh examples/sys_basic.fxsh examples/sys_result_api.fxsh examples/result_poly_native.fxsh examples/dir_walk_basic.fxsh examples/text_bytes_fs.fxsh examples/json_stdlib_file.fxsh"

for f in $cases; do
  echo "[consistency] $f"
  "$BIN" "$f" >/dev/null 2>&1
  rc_interp=$?

  "$BIN" --native "$f" >/dev/null 2>&1
  rc_native=$?

  if [ "$rc_interp" -ne "$rc_native" ]; then
    echo "mismatch: interp=$rc_interp native=$rc_native file=$f" >&2
    exit 1
  fi

done

echo "consistency smoke passed"
