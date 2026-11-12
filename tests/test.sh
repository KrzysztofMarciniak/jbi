#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/jbi-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
JBI="$ROOT/jbi"

fail() { echo "not ok - $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

# CLI basics
"$JBI" --version | grep -q '^jbi 1\.0\.0$' || fail version
"$JBI" --help >/dev/null || fail help
pass "CLI version and help"

# Project-local config is detected and can build/install/uninstall.
mkdir -p "$TMP/project/.jbi"
: > "$TMP/project/source.marker"
cat > "$TMP/project/.jbi/test.jbi" <<'CFG'
files = source.marker
order = 1
build = touch built.marker
install = touch installed.marker
uninstall = rm -f installed.marker
CFG
"$JBI" -C "$TMP/project" list | grep -q 'test .*\[files\]' || fail detection
"$JBI" -C "$TMP/project" build >/dev/null || fail build
[ -f "$TMP/project/built.marker" ] || fail build-output
"$JBI" -C "$TMP/project" install >/dev/null || fail install
[ -f "$TMP/project/installed.marker" ] || fail install-output
"$JBI" -C "$TMP/project" uninstall >/dev/null || fail uninstall
[ ! -e "$TMP/project/installed.marker" ] || fail uninstall-output
[ ! -e "$TMP/project/.jbi/state" ] || fail state-cleanup
pass "project config build/install/uninstall lifecycle"

# Dry-run prints but does not execute.
mkdir -p "$TMP/dry/.jbi"
: > "$TMP/dry/marker"
cat > "$TMP/dry/.jbi/dry.jbi" <<'CFG'
files = marker
build = touch should-not-exist
CFG
"$JBI" -C "$TMP/dry" -n build | grep -q 'touch should-not-exist' || fail dry-run-output
[ ! -e "$TMP/dry/should-not-exist" ] || fail dry-run-executed
pass "dry-run does not execute commands"

# Documentation evidence outranks file-only evidence.
mkdir -p "$TMP/rank/.jbi"
: > "$TMP/rank/one"
: > "$TMP/rank/two"
cat > "$TMP/rank/.jbi/alpha.jbi" <<'CFG'
files = one
order = 1
build = alpha-build
CFG
cat > "$TMP/rank/.jbi/beta.jbi" <<'CFG'
files = two
order = 99
build = beta-build
CFG
printf '    beta-build\n' > "$TMP/rank/README.md"
FIRST=$("$JBI" -C "$TMP/rank" list | sed -n '1s/.*[0-9]\. \([^ ]*\).*/\1/p')
[ "$FIRST" = beta ] || fail docs-ranking
pass "documentation evidence outranks order"

echo "all tests passed"
