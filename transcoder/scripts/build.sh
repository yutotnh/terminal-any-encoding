#!/usr/bin/env bash
# Builds the transcoder (the luit fork).
#
# Usage:
#   scripts/build.sh                 # native (dynamic linking, for development)
#   scripts/build.sh --musl          # musl static linking (for distribution, Linux)
#   scripts/build.sh --musl --arch arm64   # musl static linking (aarch64)
#   scripts/build.sh --musl --arch armhf   # musl static linking (32-bit ARM hard-float)
#   scripts/build.sh --warnings      # native, failing on a compiler warning
#   scripts/build.sh --leak-check    # native, freeing everything at exit
#   scripts/build.sh --sanitize      # native, with AddressSanitizer and UBSan
#
# --warnings turns on configure's warnings (-Wconversion, -Wshadow and so
# on, for gcc and clang) and fails if the build prints one that isn't in
# KNOWN_WARNINGS. --leak-check builds with configure's --disable-leaks,
# which frees luit's permanent memory at exit (the code that does is only
# built then), so that LeakSanitizer only reports real leaks. --sanitize
# builds with -fsanitize=address,undefined and --leak-check; run the tests
# with ASAN_OPTIONS and UBSAN_OPTIONS set (see
# scripts/check-sanitizer-reports.sh). All are for checking, not for
# distribution, and can be combined, though gcc warns more falsely with
# sanitizers, so CI checks warnings without them.
#
# --musl builds with the system compiler where it already targets musl for
# that architecture (e.g. in an Alpine container, which is what CI does:
# `docker run --platform linux/arm64 -v "$PWD":/src -w /src alpine ...`),
# and otherwise with a musl-cross toolchain (<triple>-gcc) on PATH, as in
# the devcontainer.
#
# See docs/transcoder-design.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/../src"

STATIC=0
ARCH="x64"
WARNINGS=0
LEAK_CHECK=0
SANITIZE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --musl) STATIC=1; shift ;;
    --arch) ARCH="$2"; shift 2 ;;
    --warnings) WARNINGS=1; shift ;;
    --leak-check) LEAK_CHECK=1; shift ;;
    --sanitize) SANITIZE=1; LEAK_CHECK=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

if [ "$STATIC" = "1" ] && { [ "$WARNINGS" = "1" ] || [ "$LEAK_CHECK" = "1" ]; }; then
  echo "--warnings, --leak-check and --sanitize are for native builds, not --musl" >&2
  exit 1
fi

# Warnings in upstream's code, left as upstream has them (the fork keeps
# its diff to its own changes): "<file>: <the source line, trimmed>:
# [-W<flag>]". gcc and clang both quote the line after the warning, so this
# holds for either, whatever line it moves to, and a warning of the same
# kind elsewhere in the file still counts.
KNOWN_WARNINGS=(
  # strchr() returning const char * (gcc with glibc 2.43+'s C23 strchr)
  "parser.c: char *dot = strchr(locale, '.');: [-Wdiscarded-qualifiers]"
  # built only with --leak-check
  "charset.c: destroyFontencCharsetPtr((FontencCharsetPtr) p->data);: [-Wcast-qual]"
  "charset.c: free((void *) fakeLocaleCharset.name);: [-Wcast-qual]"
)

cd "$SRC_DIR"

# Regenerate the table-generation pipeline first if needed (normally uses the committed output)
if [ ! -f builtin_fork.c ]; then
  echo "builtin_fork.c not found. Run tools/gen-tables/gen_tables.py first." >&2
  exit 1
fi

rm -f -- Makefile config.h config.log config.status luit ./*.o

if [ "$STATIC" = "1" ]; then
  case "$ARCH" in
    x64)   TARGET_TRIPLE="x86_64-linux-musl" ;;
    arm64) TARGET_TRIPLE="aarch64-linux-musl" ;;
    armhf) TARGET_TRIPLE="arm-linux-musleabihf" ;;
    *) echo "unknown --arch: $ARCH (must be x64, arm64 or armhf)" >&2; exit 1 ;;
  esac
  BUILD_TRIPLE="$(cc -dumpmachine 2>/dev/null || echo x86_64-pc-linux-gnu)"
  case "$ARCH:$BUILD_TRIPLE" in
    x64:x86_64-*-musl | arm64:aarch64-*-musl | armhf:arm*-*-musleabihf)
      # The system compiler already targets musl here: a native build
      CC="cc"
      STRIP="strip"
      LDFLAGS="-static" ./configure --disable-fontenc
      ;;
    *)
      CC="${TARGET_TRIPLE}-gcc"
      STRIP="${TARGET_TRIPLE}-strip"
      if ! command -v "$CC" >/dev/null 2>&1; then
        echo "$CC not found. Build in an Alpine container for that" \
             "architecture, or add a musl-cross toolchain to PATH" \
             "(it should already be there in the devcontainer)." >&2
        exit 1
      fi
      CC="$CC" LDFLAGS="-static" ./configure --host="$TARGET_TRIPLE" --build="$BUILD_TRIPLE" --disable-fontenc
      ;;
  esac
else
  CONFIGURE_ARGS=(--disable-fontenc)
  # --enable-warnings also defines attributes (noreturn on ExitProgram()
  # and so on), which --sanitize wants for them, not for the warnings
  # (they aren't checked then): LeakSanitizer takes any pointer it finds in
  # memory as a reference, so what it reports depends on the generated
  # code, and without them a stale pointer on main()'s stack hid a leak of
  # every -encode-last-arg run.
  if [ "$WARNINGS" = "1" ] || [ "$SANITIZE" = "1" ]; then
    CONFIGURE_ARGS+=(--enable-warnings)
  fi
  if [ "$SANITIZE" = "1" ]; then
    # Set only here: configure takes any CFLAGS, even an empty one, in
    # place of its default -g -O2
    SANITIZERS="-fsanitize=address,undefined -fno-sanitize-recover=all"
    export CFLAGS="${CFLAGS:-} -O1 -g -fno-omit-frame-pointer $SANITIZERS"
    export LDFLAGS="${LDFLAGS:-} $SANITIZERS"
  fi
  if [ "$LEAK_CHECK" = "1" ]; then
    CONFIGURE_ARGS+=(--disable-leaks)
  fi
  ./configure "${CONFIGURE_ARGS[@]}"
  STRIP="strip"
fi

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

# Fails on a warning in make's output (on stdin) that isn't known, by the
# source line the compiler quotes right after it (" 185 |   <line>", or
# the line alone). Serially (one compiler writing at a time), each warning
# is followed by its own line.
check_warnings() {
  KNOWN="$(printf '%s\n' "${KNOWN_WARNINGS[@]}")" awk '
    BEGIN {
      n = split(ENVIRON["KNOWN"], list, "\n")
      for (i = 1; i <= n; i++) is_known[list[i]] = 1
    }
    function settle(line) {
      if (pending == "") return
      sub(/^ *[0-9]+ \| /, "", line)
      gsub(/^[ \t]+|[ \t]+$/, "", line)
      if (!((pending ": " line ": " flag) in is_known)) unknown++
      pending = ""
    }
    { print }
    /: warning: / {
      settle("")  # a warning straight after a warning: no line quoted
      pending = substr($0, 1, index($0, ":") - 1)
      sub(/^\.\//, "", pending)
      flag = $NF
      next
    }
    pending != "" { settle($0) }
    END {
      settle("")
      if (unknown) {
        printf "%d compiler warning(s) not in KNOWN_WARNINGS (scripts/build.sh)\n", unknown > "/dev/stderr"
        exit 1
      }
    }'
}

if [ "$STATIC" = "1" ]; then
  make -j"$JOBS" CC="$CC"
  "$STRIP" luit
  echo "Static build complete: $SRC_DIR/luit ($(du -h luit | cut -f1))"
  file luit
elif [ "$WARNINGS" = "1" ]; then
  # Serially (see check_warnings), in C for messages as listed
  LC_ALL=C make 2>&1 | check_warnings
  echo "Native build complete, no unknown warnings: $SRC_DIR/luit"
else
  make -j"$JOBS"
  echo "Native build complete: $SRC_DIR/luit"
fi
