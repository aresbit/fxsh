#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

echo "[comptime] examples/comptime.fxsh"
out1=$("$BIN" examples/comptime.fxsh 2>/dev/null)
echo "$out1" | grep -q "comptime BUFFER_SIZE = 65536"
echo "$out1" | grep -q "comptime DEBUG = true"
echo "$out1" | grep -q "comptime LABEL = \"tag:build\""
echo "$out1" | grep -q "comptime MAX_RETRIES = 5"

echo "[comptime] examples/comptime_reflection.fxsh"
out2=$("$BIN" examples/comptime_reflection.fxsh 2>/dev/null)
echo "$out2" | grep -q "comptime HasAge = true"
echo "$out2" | grep -q "comptime HasId = false"
echo "$out2" | grep -q "comptime SizeInt = 8"
echo "$out2" | grep -q "comptime AlignBool = 1"

echo "[comptime] examples/comptime_quote.fxsh"
out3=$("$BIN" examples/comptime_quote.fxsh 2>/dev/null)
echo "$out3" | grep -q "comptime Q = <ast>"
echo "$out3" | grep -q "comptime U = 3"
echo "$out3" | grep -q "comptime S = 42"

echo "[comptime] examples/comptime_block.fxsh"
out4=$("$BIN" examples/comptime_block.fxsh 2>/dev/null)
echo "$out4" | grep -q "comptime A = 3"
echo "$out4" | grep -q "comptime B = 42"

echo "[comptime] examples/comptime_compile_error.fxsh"
if "$BIN" examples/comptime_compile_error.fxsh >/dev/null 2>&1; then
  echo "expected compileError to fail" >&2
  exit 1
fi

echo "[comptime] examples/comptime_panic.fxsh"
if "$BIN" examples/comptime_panic.fxsh >/dev/null 2>&1; then
  echo "expected panic to fail" >&2
  exit 1
fi

echo "[comptime] examples/comptime_specialize.fxsh"
out5=$("$BIN" examples/comptime_specialize.fxsh 2>/dev/null)
echo "$out5" | grep -q "comptime CHECK = 42"
echo "$out5" | grep -q "comptime CHECK2 = 123"

echo "comptime integration passed"
