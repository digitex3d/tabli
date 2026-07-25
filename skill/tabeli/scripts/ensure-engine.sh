#!/bin/sh
# Prints the path to a tabeli engine for this machine's architecture,
# compiling it from the bundled source on first use if no prebuilt
# binary is present. Safe to call every time (fast no-op when cached).
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARCH=$(uname -m)
BIN="$DIR/bin/tabeli-$ARCH"
if [ ! -x "$BIN" ]; then
  command -v cc >/dev/null 2>&1 || {
    echo "# error: no prebuilt engine for $ARCH and no C compiler (cc) found" >&2
    echo "# install gcc/clang, or copy a tabeli-$ARCH binary into $DIR/bin/" >&2
    exit 1
  }
  mkdir -p "$DIR/bin"
  cc -O2 -Wall -Wextra -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE \
     "$DIR/src/tabeli.c" -o "$BIN" -pie -Wl,-z,relro,-z,now
  strip "$BIN" 2>/dev/null || true
  echo "# built tabeli engine for $ARCH from bundled source" >&2
fi
echo "$BIN"
