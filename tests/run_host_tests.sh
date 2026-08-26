#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/components/nilan_cts602/include" \
  "$root/tests/test_nilan_cts602.c" \
  "$root/components/nilan_cts602/src/modbus_rtu.c" \
  "$root/components/nilan_cts602/src/register_decoder.c" \
  -o "${TMPDIR:-/tmp}/nilan_cts602_tests"
"${TMPDIR:-/tmp}/nilan_cts602_tests"
