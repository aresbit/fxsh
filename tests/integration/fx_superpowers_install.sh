#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

make -C apps/fx-superpowers launcher >/dev/null
BIN="apps/fx-superpowers/bin/fx-superpowers"
TMP=".tmp/fxsp_install_test"
SRC=".tmp/fxsp_skill_src"
CMD_SRC=".tmp/fxsp_cmd_src"
CMD_REPO=".tmp/fxsp_cmd_repo"

rm -rf "$TMP" "$SRC" "$CMD_SRC" "$CMD_REPO"
mkdir -p "$SRC"
cat >"$SRC/manifest.json" <<'EOF'
{"name":"alpha","version":"0.1.0","entry":"SKILL.md","description":"alpha"}
EOF
cat >"$SRC/SKILL.md" <<'EOF'
# Alpha Skill
EOF

echo "[fxsp-install] local install"
J1=$("$BIN" install skill "$SRC" --root "$TMP" --json)
printf '%s' "$J1" | grep -q '"installed":true'
[ -d "$TMP/.codex/skills/fxsp_skill_src" ]
[ -f "$TMP/.codex/skills/fxsp_skill_src/.fxsp-source.json" ]

echo "[fxsp-install] skip conflict"
J2=$("$BIN" install skill "$SRC" --root "$TMP" --json)
printf '%s' "$J2" | grep -q '"skipped":true'

echo "[fxsp-install] overwrite conflict"
J3=$("$BIN" install skill "$SRC" --root "$TMP" --on-conflict overwrite --json)
printf '%s' "$J3" | grep -q '"installed":true'

echo "[fxsp-install] backup conflict"
J4=$("$BIN" install skill "$SRC" --root "$TMP" --on-conflict backup --json)
printf '%s' "$J4" | grep -q '"installed":true'
ls "$TMP/.codex/skills" | grep -q 'fxsp_skill_src\.bak\.'

echo "[fxsp-install] list filters backup dirs"
L=$("$BIN" list --root "$TMP" --json)
printf '%s' "$L" | grep -q '"fxsp_skill_src"'
if printf '%s' "$L" | grep -q '.bak.'; then
  echo "list should not expose backup directories" >&2
  exit 1
fi

echo "[fxsp-install] command local file"
mkdir -p "$CMD_SRC"
cat >"$CMD_SRC/review.md" <<'EOF'
# review command
EOF
C1=$("$BIN" install command "$CMD_SRC/review.md" --root "$TMP" --json)
printf '%s' "$C1" | grep -q '"status":"installed"'
[ -f "$TMP/.codex/commands/review.md" ]
[ -f "$TMP/.codex/commands/review.md.fxsp-source.json" ]

echo "[fxsp-install] command skip + backup"
C2=$("$BIN" install command "$CMD_SRC/review.md" --root "$TMP" --json)
printf '%s' "$C2" | grep -q '"status":"skipped"'
C3=$("$BIN" install command "$CMD_SRC/review.md" --root "$TMP" --on-conflict backup --json)
printf '%s' "$C3" | grep -q '"status":"installed"'
ls "$TMP/.codex/commands" | grep -q 'review.md\.bak\.'

echo "[fxsp-install] command from git repo commands/"
mkdir -p "$CMD_REPO/commands"
cat >"$CMD_REPO/commands/plan.md" <<'EOF'
# plan command
EOF
cat >"$CMD_REPO/commands/fix.md" <<'EOF'
# fix command
EOF
(cd "$CMD_REPO" && git init -q && git add . && git -c user.name='t' -c user.email='t@t' commit -qm init)
URL="file://$ROOT_DIR/$CMD_REPO"
C4=$("$BIN" install command "$URL" --root "$TMP" --on-conflict overwrite --json)
printf '%s' "$C4" | grep -q '"name":"plan.md"'
printf '%s' "$C4" | grep -q '"name":"fix.md"'
[ -f "$TMP/.codex/commands/plan.md" ]
[ -f "$TMP/.codex/commands/fix.md" ]

rm -rf "$TMP" "$SRC" "$CMD_SRC" "$CMD_REPO"
echo "fx_superpowers_install integration passed"
