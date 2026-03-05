#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

run_case() {
  file="$1"
  expected="$2"

  echo "[closure] $file => $expected"
  out=$("$BIN" "$file" 2>&1)
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "failed: interpreter run rc=$rc file=$file" >&2
    echo "$out" >&2
    exit 1
  fi

  echo "$out" | grep -F "Interpreter:" >/dev/null || {
    echo "failed: missing interpreter output file=$file" >&2
    echo "$out" >&2
    exit 1
  }
  echo "$out" | grep -F "  => $expected" >/dev/null || {
    echo "failed: expected result $expected file=$file" >&2
    echo "$out" >&2
    exit 1
  }
}

run_case "examples/closure_make_adder.fxsh" "42"
run_case "examples/closure_lexical_scope.fxsh" "15"
run_case "examples/closure_recursion.fxsh" "120"

echo "closure integration passed"
