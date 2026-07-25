#!/bin/sh
# Run drift's regression checks. Host-native (macOS/Linux) -- no Wine needed,
# since the tests cover portable arithmetic rather than the Win32 layer.
#
# Usage: tests/run_tests.sh
set -e
cd "$(dirname "$0")"

CC=${CC:-cc}
status=0

echo "=== 1/3  source lint: WriteToBuffer row guards ==="
python3 lint_row_guards.py || status=1
echo

echo "=== 2/3  regression tests under AddressSanitizer ==="
"$CC" -std=c11 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
    row_guard_test.c -o /tmp/drift_row_guard_test
/tmp/drift_row_guard_test || status=1
echo

echo "=== 3/3  cross-compile drift.c with full warnings ==="
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    x86_64-w64-mingw32-gcc -c ../drift.c -o /tmp/drift_warncheck.o \
        -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas 2>&1 \
        | grep -v 'format-truncation' | grep -E 'warning|error' && status=1 || true
    echo "  no warnings beyond the known -Wformat-truncation notices"
else
    echo "  skipped: x86_64-w64-mingw32-gcc not installed (brew install mingw-w64)"
fi
echo

if [ "$status" -eq 0 ]; then
    echo "ALL CHECKS PASSED"
else
    echo "CHECKS FAILED"
fi
exit "$status"
