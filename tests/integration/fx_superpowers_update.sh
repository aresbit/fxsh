#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

make -C apps/fx-superpowers launcher >/dev/null
BIN="apps/fx-superpowers/bin/fx-superpowers"
TMP=".tmp/fxsp_update_test"
SRC=".tmp/fxsp_update_src"
CMD_SRC=".tmp/fxsp_update_cmd_src"

rm -rf "$TMP" "$SRC" "$CMD_SRC"
mkdir -p "$SRC"
cat >"$SRC/manifest.json" <<'EOF'
{"name":"alpha","version":"0.1.0","entry":"SKILL.md","description":"alpha"}
EOF
cat >"$SRC/SKILL.md" <<'EOF'
# Alpha v1
EOF

echo "[fxsp-update] install seed"
"$BIN" install skill "$SRC" --root "$TMP" >/dev/null
[ -f "$TMP/.codex/skills/fxsp_update_src/.fxsp-source.json" ]

echo "[fxsp-update] apply update"
cat >"$SRC/SKILL.md" <<'EOF'
# Alpha v2
EOF
U1=$("$BIN" update --root "$TMP" --json)
printf '%s' "$U1" | grep -q '"status":"updated"'
grep -q 'v2' "$TMP/.codex/skills/fxsp_update_src/SKILL.md"

echo "[fxsp-update] dry-run update"
cat >"$SRC/SKILL.md" <<'EOF'
# Alpha v3
EOF
U2=$("$BIN" update --root "$TMP" --dry-run --json)
printf '%s' "$U2" | grep -q '"status":"would_update"'
if grep -q 'v3' "$TMP/.codex/skills/fxsp_update_src/SKILL.md"; then
  echo "dry-run should not modify installed skill content" >&2
  exit 1
fi

echo "[fxsp-update] command install seed"
mkdir -p "$CMD_SRC"
cat >"$CMD_SRC/review.md" <<'EOF'
# review v1
EOF
"$BIN" install command "$CMD_SRC/review.md" --root "$TMP" >/dev/null
[ -f "$TMP/.codex/commands/review.md.fxsp-source.json" ]

echo "[fxsp-update] command apply update"
cat >"$CMD_SRC/review.md" <<'EOF'
# review v2
EOF
U3=$("$BIN" update --root "$TMP" --json)
printf '%s' "$U3" | grep -q '"kind":"command"'
printf '%s' "$U3" | grep -q '"name":"review.md"'
grep -q 'v2' "$TMP/.codex/commands/review.md"

echo "[fxsp-update] command dry-run update"
cat >"$CMD_SRC/review.md" <<'EOF'
# review v3
EOF
U4=$("$BIN" update --root "$TMP" --dry-run --json)
printf '%s' "$U4" | grep -q '"kind":"command"'
printf '%s' "$U4" | grep -q '"status":"would_update"'
if grep -q 'v3' "$TMP/.codex/commands/review.md"; then
  echo "dry-run should not modify installed command content" >&2
  exit 1
fi

rm -rf "$TMP" "$SRC" "$CMD_SRC"
echo "fx_superpowers_update integration passed"
