#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

make -C apps/fx-superpowers launcher >/dev/null
BIN="apps/fx-superpowers/bin/fx-superpowers"
TMP=".tmp/fxsp_render_test"

rm -rf "$TMP"
mkdir -p "$TMP"

echo "[fxsp-render] inline template"
OUT1=$("$BIN" render 'A={{ARGUMENTS}} R={{ROOT}} D={{DATE}}' --root "$TMP" --arguments 'x y')
printf '%s' "$OUT1" | grep -q 'A=x y'
printf '%s' "$OUT1" | grep -q "R=$TMP"

echo "[fxsp-render] file template"
cat >"$TMP/tpl.txt" <<'EOF'
arg={{ARGUMENTS}}
root={{ROOT}}
date={{DATE}}
EOF
OUT2=$("$BIN" render "$TMP/tpl.txt" --root "$TMP" --arguments 'abc')
printf '%s' "$OUT2" | grep -q 'arg=abc'
printf '%s' "$OUT2" | grep -q "root=$TMP"

echo "[fxsp-render] unknown token error"
cat >"$TMP/tpl_bad.txt" <<'EOF'
hello {{WHO}}
EOF
OUT3=$("$BIN" render "$TMP/tpl_bad.txt" --root "$TMP")
printf '%s' "$OUT3" | grep -q 'render error: unknown template token'

echo "[fxsp-render] json output"
J=$("$BIN" render 'ok={{ARGUMENTS}}' --arguments 'p' --json)
printf '%s' "$J" | grep -q '"ok":true'
printf '%s' "$J" | grep -q '"output":"ok=p"'

rm -rf "$TMP"
echo "fx_superpowers_render integration passed"

