#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT_DIR"

make -C apps/fx-superpowers launcher >/dev/null
BIN="apps/fx-superpowers/bin/fx-superpowers"
TMP=".tmp/fxsp_init_test"

rm -rf "$TMP"

echo "[fxsp-init] dry-run"
"$BIN" init --root "$TMP" --dry-run >/dev/null
if [ -d "$TMP/.codex" ]; then
  echo "dry-run should not create .codex" >&2
  exit 1
fi

echo "[fxsp-init] apply"
"$BIN" init --root "$TMP" >/dev/null
[ -d "$TMP/.codex/skills/demo" ]
[ -d "$TMP/.codex/commands" ]
[ -f "$TMP/.codex/skills/demo/manifest.json" ]
[ -f "$TMP/.codex/skills/demo/SKILL.md" ]

echo "[fxsp-init] status/list json"
S=$("$BIN" status --root "$TMP" --json)
L=$("$BIN" list --root "$TMP" --json)
printf '%s' "$S" | grep -q '"initialized":true'
printf '%s' "$L" | grep -q '"demo"'

rm -rf "$TMP"
echo "fx_superpowers_init integration passed"

