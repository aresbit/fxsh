#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

cases="examples/hello.fxsh examples/regression_stage2.fxsh examples/match_guard_min.fxsh examples/match_boxed_ctor_guard.fxsh examples/match_literal.fxsh examples/match_string.fxsh examples/match_ctor_literal.fxsh examples/match_ctor_nested.fxsh examples/match_ctor_record_tuple_list.fxsh examples/match_boxed_ctor_nested.fxsh examples/match_record.fxsh examples/match_record_literal.fxsh examples/match_record_basic_types.fxsh examples/match_tuple.fxsh examples/match_tuple_literal.fxsh examples/match_tuple_basic_types.fxsh examples/match_call_tuple_strict_bind.fxsh examples/list_basic.fxsh examples/list_pattern_literal.fxsh examples/match_poly_option.fxsh examples/direct_lambda_call.fxsh examples/hof_named.fxsh examples/closure_make_adder.fxsh examples/closure_lexical_scope.fxsh examples/closure_codegen_multi.fxsh examples/closure_codegen_string.fxsh examples/closure_codegen_chain.fxsh examples/closure_codegen_alias.fxsh examples/closure_codegen_letin.fxsh examples/closure_codegen_letin_chain.fxsh examples/closure_codegen_letin_alias_chain.fxsh examples/record_codegen_letin.fxsh examples/record_codegen_letin_alias.fxsh examples/record_codegen_top_let.fxsh examples/record_codegen_param.fxsh examples/mono_id_int_string.fxsh examples/module_basic.fxsh examples/module_brace.fxsh examples/module_result_api.fxsh examples/module_adt.fxsh examples/comptime_type_ctor.fxsh examples/import_path.fxsh examples/app_arg_nested.fxsh examples/record_helper_native.fxsh examples/closure_curried_generic.fxsh examples/ctor_tuple_payload.fxsh examples/trait_basic.fxsh examples/ffi_abs.fxsh examples/ffi_atol.fxsh examples/ffi_puts_cint.fxsh examples/ffi_dlsym_default_puts.fxsh examples/tensor_basic.fxsh examples/ffi_ptr_memset.fxsh examples/ffi_callback0.fxsh examples/sys_basic.fxsh examples/sys_result_api.fxsh examples/result_poly_native.fxsh examples/dir_walk_basic.fxsh examples/text_bytes_fs.fxsh examples/json_stdlib_file.fxsh examples/stdlib_option_result.fxsh"

for f in $cases; do
  echo "[native-codegen] $f"
  "$BIN" --native-codegen "$f" >/dev/null 2>&1 || {
    echo "failed: native-codegen run error file=$f" >&2
    exit 1
  }
done

echo "native-codegen smoke passed"
