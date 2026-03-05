#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

cases="examples/closure_make_adder.fxsh examples/closure_lexical_scope.fxsh examples/closure_recursion.fxsh"

for f in $cases; do
  echo "[closure-native] $f"
  "$BIN" "$f" >/dev/null 2>&1
  rc_interp=$?

  "$BIN" --native "$f" >/dev/null 2>&1
  rc_native=$?

  if [ "$rc_interp" -ne "$rc_native" ]; then
    echo "mismatch: interp=$rc_interp native=$rc_native file=$f" >&2
    exit 1
  fi
done

echo "closure native consistency passed"
