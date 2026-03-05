#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

cases="examples/hello.fxsh examples/pipeline.fxsh examples/factorial.fxsh examples/match.fxsh examples/match_literal.fxsh examples/match_string.fxsh examples/adt.fxsh examples/regression_stage2.fxsh examples/closure_make_adder.fxsh examples/closure_lexical_scope.fxsh examples/closure_recursion.fxsh examples/closure_codegen_letin.fxsh examples/closure_codegen_letin_chain.fxsh examples/closure_codegen_letin_alias_chain.fxsh examples/type_annotation_ok.fxsh examples/type_annotation_letin_ok.fxsh examples/record_basic.fxsh examples/record_row_poly.fxsh examples/record_type_annotation_open.fxsh"

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
