#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/jbi-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
JBI="$ROOT/jbi"

fail() { echo "not ok - $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

# CLI basics
"$JBI" --version | grep -q "^jbi $(sed -n 's/.*JBI_VERSION "\(.*\)"/\1/p' "$ROOT/jbi.h")\$" || fail version
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

# Autobuild (no configs): GNU-style main, "/*" in a string, extra mains.
export JBI_SHAREDIR="$TMP/none"
mkdir -p "$TMP/a1"
cat > "$TMP/a1/gnu.c" <<'SRC'
#include <stdio.h>
#include <math.h>
int
main (int argc, char **argv)
{
  (void) argc; (void) argv;
  puts("/* not a comment");
  return sqrt (4.0) == 2.0 ? 0 : 1;
}
SRC
"$JBI" -C "$TMP/a1" build >/dev/null 2>&1 || fail autobuild-gnu-main
[ -x "$TMP/a1/build/a1" ] && "$TMP/a1/build/a1" >/dev/null || fail autobuild-gnu-exe
mkdir -p "$TMP/a2/x" "$TMP/a2/y"
printf 'int main(void){return 0;}\n' > "$TMP/a2/x/main.c"
printf 'int main(void){return 0;}\n' > "$TMP/a2/y/main.c"
"$JBI" -C "$TMP/a2" build >/dev/null 2>&1 || fail autobuild-multi
[ "$(ls "$TMP/a2/build" | grep -vc obj)" = 2 ] || fail autobuild-multi-names
mkdir -p "$TMP/a3"
printf 'int main(void){return 0;}\n' > "$TMP/a3/ok.c"
printf 'int main(void){return 0 }\n' > "$TMP/a3/bad.c"
"$JBI" -C "$TMP/a3" build >/dev/null 2>&1 && fail autobuild-compile-error
pass "autobuild"

# Autobuild install/uninstall: executable, library headers, name fallback.
export DESTDIR="$TMP/root" PREFIX=/usr/local
rm -rf "$TMP/root"
mkdir -p "$TMP/tool" "$TMP/lib/include/mylib" "$TMP/lib/src"
printf 'int main(void){return 0;}\n' > "$TMP/tool/main.c"
printf 'int f(void);\n' > "$TMP/lib/include/mylib/f.h"
printf 'int g(void);\n' > "$TMP/lib/include/g.h"
printf '#include <mylib/f.h>\nint f(void){return 1;}\n' > "$TMP/lib/src/f.c"
printf '#include "g.h"\nint g(void){return 2;}\n' > "$TMP/lib/src/g.c"
"$JBI" -C "$TMP/tool" build >/dev/null 2>&1 || fail auto-install-build
"$JBI" -C "$TMP/tool" install >/dev/null 2>&1 || fail auto-install
[ -x "$TMP/root/usr/local/bin/tool" ] || fail auto-install-bin
"$JBI" -C "$TMP/lib" build >/dev/null 2>&1 || fail auto-install-libbuild
"$JBI" -C "$TMP/lib" install >/dev/null 2>&1 || fail auto-install-lib
[ -f "$TMP/root/usr/local/lib/liblib.a" ] || fail auto-install-liba
[ -f "$TMP/root/usr/local/include/mylib/f.h" ] || fail auto-install-header
[ -f "$TMP/root/usr/local/include/lib/g.h" ] || fail auto-install-header-name
"$JBI" -C "$TMP/tool" uninstall >/dev/null 2>&1 || fail auto-uninstall
[ ! -e "$TMP/root/usr/local/bin/tool" ] || fail auto-uninstall-bin
[ ! -e "$TMP/tool/.jbi/state" ] || fail auto-uninstall-state
rm -f "$TMP/lib/.jbi/auto.installed" # fall back to removal by name
"$JBI" -C "$TMP/lib" uninstall >/dev/null 2>&1 || fail auto-uninstall-lib
[ ! -e "$TMP/root/usr/local/lib/liblib.a" ] || fail auto-uninstall-liba
[ ! -e "$TMP/root/usr/local/include/lib" ] || fail auto-uninstall-include
pass "autobuild install/uninstall"
unset DESTDIR PREFIX

# .gitmodules without a git checkout must not abort the build.
mkdir -p "$TMP/a4"
printf '[submodule "x"]\n\tpath = x\n\turl = none\n' > "$TMP/a4/.gitmodules"
printf 'int main(void){return 0;}\n' > "$TMP/a4/m.c"
(cd / && "$JBI" -C "$TMP/a4" build >/dev/null 2>&1) || fail submodule-tarball
pass "submodule handling"

echo "all tests passed"
