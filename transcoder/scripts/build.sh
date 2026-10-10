#!/usr/bin/env bash
# Builds the transcoder (the luit fork).
#
# Usage:
#   scripts/build.sh                 # native (dynamic linking, for development)
#   scripts/build.sh --musl          # musl static linking (for distribution, Linux)
#   scripts/build.sh --musl --arch arm64   # musl static linking (aarch64)
#   scripts/build.sh --musl --arch armhf   # musl static linking (32-bit ARM hard-float)
#   scripts/build.sh --warnings      # native, failing on a compiler warning
#   scripts/build.sh --sanitize      # native, with AddressSanitizer and UBSan
#
# --warnings turns on configure's warnings (-Wconversion, -Wshadow and so
# on, for gcc and clang) and fails if the build prints one that isn't in
# KNOWN_WARNINGS. --sanitize builds with -fsanitize=address,undefined
# and --disable-leaks, which frees luit's permanent memory at exit so that
# LeakSanitizer only reports real leaks; run the tests with ASAN_OPTIONS and
# UBSAN_OPTIONS set (see the sanitizers job in .github/workflows/ci.yml).
# Both are for checking, not for distribution, and can be combined.
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
SANITIZE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --musl) STATIC=1; shift ;;
    --arch) ARCH="$2"; shift 2 ;;
    --warnings) WARNINGS=1; shift ;;
    --sanitize) SANITIZE=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

if [ "$STATIC" = "1" ] && { [ "$WARNINGS" = "1" ] || [ "$SANITIZE" = "1" ]; }; then
  echo "--warnings and --sanitize are for native builds, not --musl" >&2
  exit 1
fi

# Warnings in upstream's code, left as upstream has them (the fork keeps
# its diff to its own changes): "<file>:<function>: [-W<flag>]", the
# function as gcc names it before the warning. clang doesn't name it, so
# with clang none of these match and every warning counts.
KNOWN_WARNINGS=(
  # strchr() returning const char * (glibc 2.43+ with C23)
  "parser.c:has_encoding: [-Wdiscarded-qualifiers]"
  # built only with --sanitize (--disable-leaks)
  "charset.c:destroyCharset: [-Wcast-qual]"
  "charset.c:charset_leaks: [-Wcast-qual]"
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
  if [ "$WARNINGS" = "1" ]; then
    CONFIGURE_ARGS+=(--enable-warnings)
  fi
  if [ "$SANITIZE" = "1" ]; then
    # Set only here: configure takes any CFLAGS, even an empty one, in
    # place of its default -g -O2
    SANITIZERS="-fsanitize=address,undefined -fno-sanitize-recover=all"
    export CFLAGS="${CFLAGS:-} -O1 -g -fno-omit-frame-pointer $SANITIZERS"
    export LDFLAGS="${LDFLAGS:-} $SANITIZERS"
    CONFIGURE_ARGS+=(--disable-leaks)
  fi
  ./configure "${CONFIGURE_ARGS[@]}"
  STRIP="strip"
fi

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

# Fails on a warning in make's output (on stdin) that isn't known
check_warnings() {
  # functions: "<file>:<function>" lines, the function each file's last
  # warning context named (make -j interleaves the files' output)
  local unknown=0 line file flag function functions=""
  while IFS= read -r line; do
    printf '%s\n' "$line"
    file="${line%%:*}"
    file="${file#./}"
    case "$line" in
      *": In function '"*)  # also "In function 'f', inlined from ..."
        function="${line#*: In function \'}"
        function="${function%%\'*}"
        functions="$(printf '%s\n' "$functions" | awk -v f="$file:" 'index($0, f) != 1')
$file:$function"
        continue ;;
      *": At top level:")
        functions="$(printf '%s\n' "$functions" | awk -v f="$file:" 'index($0, f) != 1')"
        continue ;;
      *": warning: "*) ;;
      *) continue ;;
    esac
    flag="${line##* }"
    function="$(printf '%s\n' "$functions" |
      awk -v f="$file:" 'index($0, f) == 1 { print substr($0, length(f) + 1) }')"
    if ! printf '%s\n' "${KNOWN_WARNINGS[@]}" | grep -qxF -- "$file:$function: $flag"; then
      unknown=$((unknown + 1))
    fi
  done
  if [ "$unknown" -gt 0 ]; then
    echo "$unknown compiler warning(s) not in KNOWN_WARNINGS (scripts/build.sh)" >&2
    return 1
  fi
}

if [ "$STATIC" = "1" ]; then
  make -j"$JOBS" CC="$CC"
  "$STRIP" luit
  echo "Static build complete: $SRC_DIR/luit ($(du -h luit | cut -f1))"
  file luit
elif [ "$WARNINGS" = "1" ]; then
  # Each line checked is written whole; C quotes the function names
  LC_ALL=C make -j"$JOBS" 2>&1 | check_warnings
  echo "Native build complete, no unknown warnings: $SRC_DIR/luit"
else
  make -j"$JOBS"
  echo "Native build complete: $SRC_DIR/luit"
fi
