#!/usr/bin/env bash
# Checks what a --sanitize build of luit (scripts/build.sh) reported while
# the tests ran, with ASAN_OPTIONS and UBSAN_OPTIONS sending the reports to
# files in a directory (luit's stderr is the terminal, where the tests
# wouldn't see them):
#
#   reports="$(mktemp -d)"
#   export ASAN_OPTIONS="detect_leaks=1:log_path=$reports/asan"
#   export UBSAN_OPTIONS="print_stacktrace=1:log_path=$reports/ubsan"
#   ... run the tests ...
#   scripts/check-sanitizer-reports.sh "$reports"
#
# Not every report fails a test: the converter luit detaches writes its
# report when it exits, possibly after the tests have returned. So this
# first waits for every process running this build's luit to exit (other
# luits, such as VS Code terminals', aren't waited for), then fails on any
# report, or on a luit still running after 30 s, which is a bug too. Linux
# only, like LeakSanitizer itself on the runners: processes are found
# through /proc.
set -euo pipefail

if [ $# -ne 1 ] || [ ! -d "$1" ]; then
  echo "usage: $0 <directory the sanitizers reported to>" >&2
  exit 2
fi
REPORTS="$1"
LUIT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../src" && pwd)/luit"

# The pids of the processes running $LUIT
running() {
  local exe pid
  for exe in /proc/[0-9]*/exe; do
    [ "$(readlink "$exe" 2>/dev/null)" = "$LUIT" ] || continue
    pid="${exe#/proc/}"
    echo "${pid%/exe}"
  done
}

for _ in $(seq 30); do
  [ -z "$(running)" ] && break
  sleep 1
done
lingering="$(running)"

failed=0
shopt -s nullglob
reports=("$REPORTS"/*)
if [ "${#reports[@]}" -gt 0 ]; then
  cat "${reports[@]}"
  echo "${#reports[@]} sanitizer report(s)" >&2
  failed=1
fi
if [ -n "$lingering" ]; then
  echo "luit still running 30 s after the tests:" >&2
  for pid in $lingering; do
    tr '\0' ' ' <"/proc/$pid/cmdline" >&2 || true
    echo >&2
  done
  failed=1
fi
[ "$failed" = "0" ] && echo "No sanitizer reports"
exit "$failed"
