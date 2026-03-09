#!/usr/bin/env sh
set -eu

BIN="${1:-./bin/fxsh}"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

TMP_DIR="/tmp/fxsh_ct_${$}"
mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

expect_fail_contains() {
  case_name="$1"
  expected="$2"
  if "$BIN" "$TMP_DIR/$case_name.fxsh" >/dev/null 2>"$TMP_DIR/$case_name.err"; then
    echo "expected $case_name to fail" >&2
    exit 1
  fi
  grep -Fq "$expected" "$TMP_DIR/$case_name.err"
}

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

echo "[comptime] sqlite sql"
out_sql=$("$BIN" examples/comptime_sqlite_sql.fxsh 2>/dev/null)
echo "$out_sql" | grep -q 'CREATE TABLE IF NOT EXISTS "users"'
echo "$out_sql" | grep -q '"id" INTEGER NOT NULL PRIMARY KEY'
echo "$out_sql" | grep -q 'INSERT INTO "users" ("id", "email", "balance", "active") VALUES (?, ?, ?, ?);'
echo "$out_sql" | grep -q 'SELECT "id", "email", "balance", "active" FROM "users";'

echo "[comptime] sql dsl"
out_sql_dsl=$("$BIN" examples/comptime_sql_dsl.fxsh 2>/dev/null)
echo "$out_sql_dsl" | grep -q 'SELECT DISTINCT'
echo "$out_sql_dsl" | grep -q 'LEFT JOIN invoices i ON i.user_id = u.id'
echo "$out_sql_dsl" | grep -q 'GROUP BY u.id, u.email'
echo "$out_sql_dsl" | grep -q 'HAVING count(i.id) >= ?'
echo "$out_sql_dsl" | grep -q 'OFFSET 5;'
echo "$out_sql_dsl" | grep -q 'INSERT OR REPLACE INTO "users"'
echo "$out_sql_dsl" | grep -q 'ON CONFLICT (id) DO UPDATE'
echo "$out_sql_dsl" | grep -q 'RETURNING "id", "email";'
echo "$out_sql_dsl" | grep -q 'UPDATE OR ABORT "users" SET email = ?, active = ? WHERE id = ? RETURNING id;'
echo "$out_sql_dsl" | grep -q 'DELETE FROM "users" WHERE id = ? LIMIT 1;'
echo "$out_sql_dsl" | grep -q 'CREATE TABLE IF NOT EXISTS "audit_logs"'
echo "$out_sql_dsl" | grep -q 'CREATE INDEX IF NOT EXISTS "idx_audit_user_time"'
echo "$out_sql_dsl" | grep -q 'PRAGMA journal_mode = WAL;'
echo "$out_sql_dsl" | grep -q 'EXPLAIN QUERY PLAN SELECT'
echo "$out_sql_dsl" | grep -q 'VACUUM;'

echo "[comptime] sql check"
cat > "$TMP_DIR/sql_check_ok.fxsh" <<'EOF'
let comptime ok = @sqlCheck({
  op = "select",
  columns = ["id", "email"],
  from = "users",
  where = ["id = ?", "email = ?"],
  param_count = 2
})
let comptime _L = @compileLog(ok)
EOF
"$BIN" "$TMP_DIR/sql_check_ok.fxsh" >/dev/null 2>"$TMP_DIR/sql_check_ok.err"
grep -q '\[comptime\] true' "$TMP_DIR/sql_check_ok.err"

echo "[comptime] sql schema check"
cat > "$TMP_DIR/sql_schema_ok.fxsh" <<'EOF'
let user = { id = 1, email = "a@b", active = true }
let comptime ok = @sqlCheck({
  op = "update",
  table = "users",
  set = ["email = ?", "active = ?"],
  where = ["id = ?"],
  schema = @typeOf(user),
  param_count = 3
})
let comptime _L = @compileLog(ok)
EOF
"$BIN" "$TMP_DIR/sql_schema_ok.fxsh" >/dev/null 2>"$TMP_DIR/sql_schema_ok.err"
grep -q '\[comptime\] true' "$TMP_DIR/sql_schema_ok.err"

echo "[comptime] sql dsl errors"
cat > "$TMP_DIR/sql_missing_op.fxsh" <<'EOF'
let comptime bad = @sql({ table = "users" })
EOF
expect_fail_contains "sql_missing_op" "SQL_E_DSL_OP_REQUIRED:"

cat > "$TMP_DIR/sql_unknown_op.fxsh" <<'EOF'
let comptime bad = @sql({ op = "wat" })
EOF
expect_fail_contains "sql_unknown_op" "SQL_E_UNKNOWN_OP:"

cat > "$TMP_DIR/sql_select_missing_required.fxsh" <<'EOF'
let comptime bad = @sql({ op = "select", from = "users" })
EOF
expect_fail_contains "sql_select_missing_required" "SQL_E_SELECT_REQUIRED:"

cat > "$TMP_DIR/sql_insert_mutually_exclusive.fxsh" <<'EOF'
let comptime bad = @sql({
  op = "insert",
  table = "users",
  columns = ["id"],
  rows = [["1"]],
  values = ["1"]
})
EOF
expect_fail_contains "sql_insert_mutually_exclusive" "SQL_E_INSERT_MUTUALLY_EXCLUSIVE:"

cat > "$TMP_DIR/sql_insert_row_size_mismatch.fxsh" <<'EOF'
let comptime bad = @sql({
  op = "insert",
  table = "users",
  columns = ["id", "email"],
  rows = [["1"]]
})
EOF
expect_fail_contains "sql_insert_row_size_mismatch" "SQL_E_INSERT_ROW_SIZE_MISMATCH:"

cat > "$TMP_DIR/sql_bad_returning_type.fxsh" <<'EOF'
let comptime bad = @sql({
  op = "update",
  table = "users",
  set = ["email = ?"],
  returning = 1
})
EOF
expect_fail_contains "sql_bad_returning_type" "SQL_E_RETURNING_TYPE:"

cat > "$TMP_DIR/sql_param_count_mismatch.fxsh" <<'EOF'
let comptime bad = @sql({
  op = "select",
  columns = ["id"],
  from = "users",
  where = ["id = ?"],
  param_count = 2
})
EOF
expect_fail_contains "sql_param_count_mismatch" "SQL_E_PARAM_COUNT_MISMATCH:"

cat > "$TMP_DIR/sql_param_count_type.fxsh" <<'EOF'
let comptime bad = @sql({
  op = "select",
  columns = ["id"],
  from = "users",
  where = ["id = ?"],
  param_count = "1"
})
EOF
expect_fail_contains "sql_param_count_type" "SQL_E_PARAM_COUNT_TYPE:"

cat > "$TMP_DIR/sql_schema_unknown_column.fxsh" <<'EOF'
let user = { id = 1, email = "a@b", active = true }
let comptime bad = @sql({
  op = "select",
  columns = ["id", "missing"],
  from = "users",
  schema = @typeOf(user)
})
EOF
expect_fail_contains "sql_schema_unknown_column" "SQL_E_SCHEMA_UNKNOWN_COLUMN:"

cat > "$TMP_DIR/sql_schema_bad_type.fxsh" <<'EOF'
let comptime bad = @sql({
  op = "select",
  columns = ["id"],
  from = "users",
  schema = "not-a-type"
})
EOF
expect_fail_contains "sql_schema_bad_type" "SQL_E_SCHEMA_TYPE:"

cat > "$TMP_DIR/sqlite_sql_non_record.fxsh" <<'EOF'
let comptime bad = @sqliteSQL(@typeOf(1), "users")
EOF
expect_fail_contains "sqlite_sql_non_record" "SQLITE_E_EXPECT_RECORD:"

echo "comptime integration passed"
