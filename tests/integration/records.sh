#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

ok_cases="examples/record_basic.fxsh examples/record_row_poly.fxsh examples/record_type_annotation_open.fxsh"
for f in $ok_cases; do
  echo "[records-ok] $f"
  "$BIN" "$f" >/dev/null 2>&1
  "$BIN" --native "$f" >/dev/null 2>&1
done

bad_cases="examples/record_missing_field.fxsh examples/record_type_annotation_closed_fail.fxsh"
for f in $bad_cases; do
  echo "[records-bad] $f"
  if "$BIN" "$f" >/dev/null 2>&1; then
    echo "expected type-check failure in interpreter path: $f" >&2
    exit 1
  fi
  if "$BIN" --native "$f" >/dev/null 2>&1; then
    echo "expected type-check failure in native path: $f" >&2
    exit 1
  fi
done

echo "records integration passed"
