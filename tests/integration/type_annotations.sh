#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

ok_cases="examples/type_annotation_ok.fxsh examples/type_annotation_letin_ok.fxsh"
for f in $ok_cases; do
  echo "[type-annotations-ok] $f"
  "$BIN" "$f" >/dev/null 2>&1
  "$BIN" --native "$f" >/dev/null 2>&1
done

bad_cases="examples/type_annotation_mismatch.fxsh"
for f in $bad_cases; do
  echo "[type-annotations-bad] $f"
  if "$BIN" "$f" >/dev/null 2>&1; then
    echo "expected type-check failure in interpreter path: $f" >&2
    exit 1
  fi
  if "$BIN" --native "$f" >/dev/null 2>&1; then
    echo "expected type-check failure in native path: $f" >&2
    exit 1
  fi
done

echo "type annotation integration passed"
