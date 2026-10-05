#!/usr/bin/env bash
# Builds the transcoder (the luit fork).
#
# Usage:
#   scripts/build.sh                 # native (dynamic linking, for development)
#   scripts/build.sh --musl          # musl static linking (for distribution, Linux)
#   scripts/build.sh --musl --arch arm64   # musl static linking (aarch64)
#   scripts/build.sh --musl --arch armhf   # musl static linking (32-bit ARM hard-float)
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
  ./configure --disable-fontenc
  STRIP="strip"
fi

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

if [ "$STATIC" = "1" ]; then
  make -j"$JOBS" CC="$CC"
  "$STRIP" luit
  echo "Static build complete: $SRC_DIR/luit ($(du -h luit | cut -f1))"
  file luit
else
  make -j"$JOBS"
  echo "Native build complete: $SRC_DIR/luit"
fi
