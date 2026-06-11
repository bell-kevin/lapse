#!/usr/bin/env bash
# Smoke test for lapse. Usage: tests/smoke.sh [path-to-binary]
set -euo pipefail

BIN="${1:-./lapse}"
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
[ -x "$BIN" ] || { echo "binary not found: $BIN"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; exit 1; }

# --- snapshot a small tree -------------------------------------------------
mkdir -p project/src
echo "hello"          > project/notes.txt
echo "int main(){}"   > project/src/main.cpp
cd project

"$BIN" snap -m "first" >/dev/null

# --- modify, ignore, status ------------------------------------------------
echo "hello world" > notes.txt
echo "junk"        > scratch.tmp
echo "*.tmp"       > .lapseignore

"$BIN" status | grep -q "notes.txt"            || fail "status should list notes.txt"
"$BIN" status | grep -q "scratch.tmp"          && fail "status should ignore *.tmp" || true

"$BIN" snap -m "second" >/dev/null
"$BIN" log | grep -q "second"                  || fail "log should show message"
"$BIN" show last | grep -q "scratch.tmp"       && fail "snapshot should ignore *.tmp" || true

# --- identical re-snap is a no-op -------------------------------------------
"$BIN" snap | grep -q "nothing changed"        || fail "identical snap should be a no-op"

# --- cat / restore -----------------------------------------------------------
FIRST="$("$BIN" log | awk '$1=="*"{print $2}' | tail -n 1)"
"$BIN" cat "$FIRST" notes.txt | grep -qx "hello"        || fail "cat of first snapshot"
"$BIN" cat last     notes.txt | grep -qx "hello world"  || fail "cat of last snapshot"

# restore refuses to clobber without --force
"$BIN" restore "$FIRST" notes.txt >/dev/null 2>&1 && fail "restore should refuse without --force" || true
grep -qx "hello world" notes.txt               || fail "file should be untouched after refusal"

"$BIN" restore "$FIRST" notes.txt --force >/dev/null
grep -qx "hello" notes.txt                     || fail "restore --force should bring old content back"

# restore an entire snapshot elsewhere
"$BIN" restore last --to "$TMP/export" >/dev/null
grep -qx "hello world" "$TMP/export/notes.txt" || fail "restore --to"
[ -f "$TMP/export/src/main.cpp" ]              || fail "restore --to should recreate tree"

# --- diff --------------------------------------------------------------------
"$BIN" diff "$FIRST" last | grep -q "M  notes.txt" || fail "diff should mark notes.txt modified"

# --- prune + gc ----------------------------------------------------------------
"$BIN" prune --keep 1 >/dev/null
[ "$("$BIN" log | grep -c '^\*')" = "1" ]      || fail "prune should leave one snapshot"
"$BIN" cat last notes.txt | grep -qx "hello world" || fail "kept snapshot must survive prune"

echo "PASS: all smoke tests"
