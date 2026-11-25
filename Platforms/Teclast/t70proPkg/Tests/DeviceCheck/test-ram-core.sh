#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP="$HERE/../../Applications/DeviceCheck"
OUT=${TMPDIR:-/tmp}/t70-ram-core-test
${CC:-clang} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$APP" "$HERE/test-ram-core.c" "$APP/T70RamTestCore.c" -o "$OUT"
"$OUT"
rm -f "$OUT"
