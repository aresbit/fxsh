#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

OUT=$(mktemp)
ERR=$(mktemp)
trap 'rm -f "$OUT" "$ERR"' EXIT

run_case() {
  file="$1"
  expect="$2"
  if ! FXSH_LDFLAGS='-lsqlite3' "$BIN" --native-codegen "$file" >"$OUT" 2>"$ERR"; then
    if grep -qi "cannot find -lsqlite3" "$ERR" || grep -qi "library not found.*sqlite3" "$ERR"; then
      echo "[sqlite-native] skipped: libsqlite3 not found"
      exit 0
    fi
    echo "[sqlite-native] failed: native-codegen execution error file=$file" >&2
    cat "$ERR" >&2
    exit 1
  fi

  if ! grep -q "$expect" "$OUT"; then
    echo "[sqlite-native] failed: expected '$expect' in output file=$file" >&2
    cat "$OUT" >&2
    exit 1
  fi
}

run_case "examples/sqlite_query_min.fxsh" "sqlite query ok"
run_case "examples/sqlite_query_min.fxsh" "name=alice"
run_case "examples/sqlite_error_path.fxsh" "sqlite error path ok"
run_case "examples/sqlite_crud_template.fxsh" "crud template ok: widget"

echo "sqlite native-codegen integration passed"
