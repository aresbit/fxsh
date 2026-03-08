#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

TMP_DIR="/tmp/fxsh_ct_${$}"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

echo "[comptime] basic"
cat > "$TMP_DIR/basic.fxsh" <<'EOF'
let comptime x = 1 + 2
let comptime s = "hello" ++ " world"
let comptime b = not false
let comptime i = if b then x else 0
let comptime f = fn n -> n * 2
let comptime add = fn a -> fn c -> a + c
let comptime _L1 = @compileLog(x)
let comptime _L2 = @compileLog(s)
let comptime _L3 = @compileLog(i)
let comptime _L4 = @compileLog(f 21)
let comptime _L5 = @compileLog((add 1) 2)
EOF
"$BIN" "$TMP_DIR/basic.fxsh" >/dev/null 2>"$TMP_DIR/basic.err"
grep -q '\[comptime\] 3' "$TMP_DIR/basic.err"
grep -q '\[comptime\] "hello world"' "$TMP_DIR/basic.err"
grep -q '\[comptime\] 42' "$TMP_DIR/basic.err"

echo "[comptime] match + list recursion"
cat > "$TMP_DIR/match_list_rec.fxsh" <<'EOF'
let comptime rec sum = fn xs ->
  match xs with
  | [] -> 0
  | x :: rest -> x + sum rest
  end

let comptime m = match [1, 2, 3] with
| [a, b, c] -> a + b + c
| _ -> 0
end

let comptime _L1 = @compileLog(sum [1, 2, 3, 4])
let comptime _L2 = @compileLog(m)
EOF
"$BIN" "$TMP_DIR/match_list_rec.fxsh" >/dev/null 2>"$TMP_DIR/match_list_rec.err"
grep -q '\[comptime\] 10' "$TMP_DIR/match_list_rec.err"
grep -q '\[comptime\] 6' "$TMP_DIR/match_list_rec.err"

echo "[comptime] type ctors"
cat > "$TMP_DIR/type_ctors.fxsh" <<'EOF'
let comptime t1 = @List(@typeOf(1))
let comptime t2 = @Option(@typeOf("x"))
let comptime t3 = @Result(@typeOf(1), @typeOf("e"))
let comptime _L1 = @compileLog(@typeName(t1))
let comptime _L2 = @compileLog(@typeName(t2))
let comptime _L3 = @compileLog(@typeName(t3))
EOF
"$BIN" "$TMP_DIR/type_ctors.fxsh" >/dev/null 2>"$TMP_DIR/type_ctors.err"
grep -q '\[comptime\] "int list"' "$TMP_DIR/type_ctors.err"
grep -q '\[comptime\] "string option"' "$TMP_DIR/type_ctors.err"
grep -q '\[comptime\] "int string result"' "$TMP_DIR/type_ctors.err"

echo "[comptime] diagnostics"
cat > "$TMP_DIR/compile_error.fxsh" <<'EOF'
let comptime BAD = @compileError("boom")
EOF
if "$BIN" "$TMP_DIR/compile_error.fxsh" >/dev/null 2>"$TMP_DIR/compile_error.err"; then
  echo "expected compileError to fail" >&2
  exit 1
fi
grep -q 'compileError: "boom"' "$TMP_DIR/compile_error.err"

cat > "$TMP_DIR/panic.fxsh" <<'EOF'
let comptime BAD = @panic("ct panic")
EOF
if "$BIN" "$TMP_DIR/panic.fxsh" >/dev/null 2>"$TMP_DIR/panic.err"; then
  echo "expected panic to fail" >&2
  exit 1
fi
grep -q 'panic: "ct panic"' "$TMP_DIR/panic.err"

echo "[comptime] json schema"
cat > "$TMP_DIR/json_schema.fxsh" <<'EOF'
let comptime schema = @jsonSchema(@typeOf({ id = 1, ok = true }))
let comptime _L = @compileLog(schema)
EOF
"$BIN" "$TMP_DIR/json_schema.fxsh" >/dev/null 2>"$TMP_DIR/json_schema.err"
grep -q '"type":"object"' "$TMP_DIR/json_schema.err"
grep -q '"required":\["id","ok"\]' "$TMP_DIR/json_schema.err"

echo "comptime integration passed"
