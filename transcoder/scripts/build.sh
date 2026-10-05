#!/usr/bin/env bash
# Builds the transcoder (the luit fork).
#
# Usage:
#   scripts/build.sh                 # native (dynamic linking, for development)
#   scripts/build.sh --musl          # musl static linking (for distribution, Linux)
#   scripts/build.sh --musl --arch arm64   # musl static linking (cross, aarch64)
#   scripts/build.sh --musl --arch armhf   # musl static linking (cross, 32-bit ARM hard-float)
#
# See docs/transcoder-design.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/../src"

STATIC=0
ARCH="x64"

while [ $# -gt 0 ]; do
  case "$1" in
    --musl) STATIC=1; shift ;;
    --arch) ARCH="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

cd "$SRC_DIR"

# Regenerate the table-generation pipeline first if needed (normally uses the committed output)
if [ ! -f builtin_ja.c ]; then
  echo "builtin_ja.c not found. Run tools/gen-tables/gen_tables.py first." >&2
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
  CC="${TARGET_TRIPLE}-gcc"
  STRIP="${TARGET_TRIPLE}-strip"
  if ! command -v "$CC" >/dev/null 2>&1; then
    echo "$CC not found. Add the musl-cross toolchain to PATH" \
         "(it should already be there in the devcontainer)." >&2
    exit 1
  fi
  BUILD_TRIPLE="$(cc -dumpmachine 2>/dev/null || echo x86_64-pc-linux-gnu)"
  CC="$CC" ./configure --host="$TARGET_TRIPLE" --build="$BUILD_TRIPLE" --disable-fontenc
else
  ./configure --disable-fontenc
  STRIP="strip"
fi

# Add the generated tables and the CP932-specific implementation to the build
sed -i 's/^SRCS\(.*\)= \(.*\)$/SRCS\1= \2 builtin_ja.c other_ja.c gb18030_ranges.c/' Makefile
sed -i 's/^OBJS\(.*\)= \(.*\)$/OBJS\1= \2 builtin_ja.o other_ja.o gb18030_ranges.o/' Makefile

if [ "$STATIC" = "1" ]; then
  sed -i 's/^LDFLAGS\s*=.*/LDFLAGS = -static/' Makefile
  make -j"$(nproc)" CC="$CC"
  "$STRIP" luit
  echo "Static build complete: $SRC_DIR/luit ($(du -h luit | cut -f1))"
  file luit
else
  make -j"$(nproc)"
  echo "Native build complete: $SRC_DIR/luit"
fi
