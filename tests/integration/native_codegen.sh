#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

cases="examples/hello.fxsh examples/regression_stage2.fxsh examples/closure_make_adder.fxsh examples/closure_lexical_scope.fxsh examples/closure_codegen_multi.fxsh examples/closure_codegen_string.fxsh examples/closure_codegen_chain.fxsh examples/closure_codegen_alias.fxsh examples/closure_codegen_letin.fxsh examples/closure_codegen_letin_chain.fxsh examples/closure_codegen_letin_alias_chain.fxsh examples/record_codegen_letin.fxsh examples/record_codegen_letin_alias.fxsh"

for f in $cases; do
  echo "[native-codegen] $f"
  "$BIN" --native-codegen "$f" >/dev/null 2>&1 || {
    echo "failed: native-codegen run error file=$f" >&2
    exit 1
  }
done

echo "native-codegen smoke passed"
