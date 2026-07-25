#!/usr/bin/env python3
"""Static check for the WriteToBuffer row-guard invariant in drift.c.

WriteToBuffer bounds the column it writes but not the row (it has no height to
compare against), so the frame buffer -- exactly width*height cells -- is only
safe if every caller checks the row itself. Three call sites once did not, which
produced heap overflows in short console windows.

This flags any WriteToBuffer(buffer, ...) call whose row is a literal >= 3 and
which carries no reference to `height` on its own line or the line above.

Rows 0-2 need no guard: DrawScreen refuses to draw below MIN_WINDOW_HEIGHT, and
the header and rule at rows 0-1 are structural.

Scope: the frame buffer only, matched by the parameter name `buffer`. Popup
buffers are separate allocations, each already gated on its own popup_h.
"""

import re
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parent.parent / "drift.c"
CALL = re.compile(r"WriteToBuffer\(\s*buffer\s*,\s*width\s*,\s*([^,]+?)\s*,")
UNGUARDED_MAX_ROW = 2


def main() -> int:
    try:
        lines = SOURCE.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        print(f"lint_row_guards: cannot read {SOURCE}: {exc}", file=sys.stderr)
        return 2

    problems = []
    for idx, line in enumerate(lines):
        for match in CALL.finditer(line):
            row = match.group(1).strip()
            if not row.isdigit():
                continue  # an expression (row++, height - 1, 4 + i): loop/guard bound
            if int(row) <= UNGUARDED_MAX_ROW:
                continue
            context = line + (lines[idx - 1] if idx > 0 else "")
            if "height" in context:
                continue
            problems.append((idx + 1, int(row), line.strip()))

    print(f"lint_row_guards: scanned {SOURCE.name} ({len(lines)} lines)")
    if problems:
        for lineno, row, text in problems:
            print(f"  drift.c:{lineno}: writes literal row {row} with no height guard")
            print(f"    {text}")
        print(f"\nFAIL ({len(problems)} unguarded row write"
              f"{'' if len(problems) == 1 else 's'})")
        return 1

    print("  every literal row >= 3 into the frame buffer is height-guarded")
    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
