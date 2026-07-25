// Regression tests for the memory-safety fixes in drift.c.
//
// drift.c is Windows-only, so these tests mirror the exact arithmetic of the
// routines under test rather than linking against them. tests/lint_row_guards.py
// is the companion check that keeps drift.c itself in sync with the invariant
// proved here.
//
// Build and run:  tests/run_tests.sh
//
// Covers:
//   1. WriteToBuffer row-guard invariant -- the frame buffer holds exactly
//      width*height cells, and no pane may write at a row >= height.
//      Regression for the three heap overflows at drift.c:658, 1955-1956 and
//      1521-1531 (each confirmed under ASAN before the fix).
//   2. SaveMembersTo's worst-case array size stays inside `cap`, so the
//      `n += snprintf(...)` accumulation can never make `cap - n` wrap.
//   3. RemoveMemberAt's row-shifting strcpy operates on disjoint memory
//      (GCC's -fanalyzer reports a false positive here).

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MAX_PATH 260
#define MAX_MEMBERS 128
#define COLUMN_DIVIDER_POSITION 28
#define MIN_WINDOW_HEIGHT 7
#define THREE_PANE_MIN_WIDTH 80
#define SECOND_DIVIDER_MAX (COLUMN_DIVIDER_POSITION + 58)

// Matches Win32 CHAR_INFO: two WORDs.
typedef struct { unsigned short u; unsigned short a; } CHAR_INFO;

static int failures = 0;

static void report(const char* name, bool ok) {
    printf("  %-58s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

// ---------------------------------------------------------------------------
// 1. Row-guard invariant
// ---------------------------------------------------------------------------

// Verbatim from drift.c:WriteToBuffer -- note it bounds the column but not the
// row, which is why every caller owes a "row < height" guard.
static CHAR_INFO* g_buf;
static size_t g_cells;      // real allocation, in cells
static bool g_overflowed;

static void WriteToBuffer(CHAR_INFO* buffer, int width, int row, int col,
                          const char* text, unsigned short color) {
    wchar_t wtext[MAX_PATH];
    int k = 0;
    for (; text[k] != '\0' && k < MAX_PATH - 1; k++) wtext[k] = (wchar_t)(unsigned char)text[k];
    wtext[k] = L'\0';

    int len = (int)wcslen(wtext);
    for (int i = 0; i < len && col + i < width - 1; i++) {
        int index = row * width + col + i;
        if (index < 0 || (size_t)index >= g_cells) {
            g_overflowed = true;  // would be a heap-buffer-overflow
            return;
        }
        buffer[index].u = wtext[i];
        buffer[index].a = color;
    }
}

// The three call sites that had unguarded literal rows, now carrying their
// guards. Mirrors drift.c exactly; if a guard is dropped there and here, ASAN
// plus the g_cells check both fire.
static void draw_empty_workspace_hint(int width, int height) {
    WriteToBuffer(g_buf, width, 2, COLUMN_DIVIDER_POSITION + 4, "(no workspaces yet)", 8);
    if (3 < height) {
        WriteToBuffer(g_buf, width, 3, COLUMN_DIVIDER_POSITION + 4, "press a to create one", 8);
    }
}

static void draw_claude_info_pane(int width, int height, int divider2) {
    int col = divider2 + 2;
    if (col >= width - 8) return;
    WriteToBuffer(g_buf, width, 2, col, "0 sessions", 7);
    if (4 < height) WriteToBuffer(g_buf, width, 4, col, "press e to select the folders", 8);
    if (5 < height) WriteToBuffer(g_buf, width, 5, col, "this workspace opens in Claude", 8);
}

static void draw_sessions_detail_pane(int width, int height, int divider2) {
    if (divider2 >= width) return;
    int col_start = divider2 + 2;
    WriteToBuffer(g_buf, width, height - 1, col_start, "Enter resume  n new  r rename  d delete", 8);
    WriteToBuffer(g_buf, width, 2, col_start, "a-long-workspace-display-name", 6);
    if (3 < height) WriteToBuffer(g_buf, width, 3, col_start, "12 sessions", 7);
    if (5 < height) WriteToBuffer(g_buf, width, 5, col_start, "last active: 3h", 7);
    if (6 < height) WriteToBuffer(g_buf, width, 6, col_start, "id: 4f2c9a11-0e7d-4b52-9c31-8a6b0f3d1e77", 8);

    // The wrapped first-prompt block, which was already guarded
    int pane_w = width - 1 - col_start;
    if (pane_w < 8) return;
    int row = 8;
    int off = 0, total = 300;
    while (off < total && row < height) {
        int c = total - off;
        if (c > pane_w) c = pane_w;
        char chunk[200];
        if (c > (int)sizeof(chunk) - 1) c = (int)sizeof(chunk) - 1;
        memset(chunk, 'x', c);
        chunk[c] = '\0';
        WriteToBuffer(g_buf, width, row, col_start, chunk, 7);
        off += c;
        row++;
    }
}

// Mirrors DrawScreen's divider maths so the panes get realistic geometry.
static int divider2_for(int width) {
    if (width < THREE_PANE_MIN_WIDTH) return width;
    int d = width - width / 3;
    return d > SECOND_DIVIDER_MAX ? SECOND_DIVIDER_MAX : d;
}

static void test_row_guards(void) {
    printf("Row-guard invariant (no pane writes at row >= height)\n");

    // Sweep well below MIN_WINDOW_HEIGHT too: the guards must hold on their
    // own, so that lowering the minimum can never reintroduce the overflow.
    bool ok = true;
    for (int height = 3; height <= 24 && ok; height++) {
        for (int width = 36; width <= 200 && ok; width += 4) {
            g_cells = (size_t)width * (size_t)height;
            g_buf = (CHAR_INFO*)malloc(g_cells * sizeof(CHAR_INFO));
            assert(g_buf != NULL);
            g_overflowed = false;

            draw_empty_workspace_hint(width, height);
            draw_claude_info_pane(width, height, divider2_for(width));
            draw_sessions_detail_pane(width, height, divider2_for(width));

            if (g_overflowed) {
                printf("    overflow at width=%d height=%d\n", width, height);
                ok = false;
            }
            free(g_buf);
            g_buf = NULL;
        }
    }
    report("guarded panes stay in bounds, heights 3-24 x widths 36-200", ok);

    // Negative control: the pre-fix code, to prove this harness can actually
    // see the bug it is guarding against.
    g_cells = 80 * 3;
    g_buf = (CHAR_INFO*)malloc(g_cells * sizeof(CHAR_INFO));
    g_overflowed = false;
    WriteToBuffer(g_buf, 80, 2, COLUMN_DIVIDER_POSITION + 4, "(no workspaces yet)", 8);
    WriteToBuffer(g_buf, 80, 3, COLUMN_DIVIDER_POSITION + 4, "press a to create one", 8); // unguarded
    report("negative control: unguarded row 3 at height 3 is detected", g_overflowed);
    free(g_buf);
    g_buf = NULL;
}

// ---------------------------------------------------------------------------
// 2. SaveMembersTo array bound
// ---------------------------------------------------------------------------

static char members[MAX_MEMBERS][MAX_PATH];

static void test_save_members_bound(void) {
    printf("SaveMembersTo worst-case array bound\n");

    // Worst case: every slot full, every path max length, every character
    // needing a JSON escape (so each source byte costs two output bytes).
    for (int i = 0; i < MAX_MEMBERS; i++) {
        memset(members[i], '\\', MAX_PATH - 1);
        members[i][MAX_PATH - 1] = '\0';
    }

    size_t cap = (size_t)MAX_MEMBERS * (MAX_PATH * 2 + 16) + 64;
    char* arr = (char*)malloc(cap);
    assert(arr != NULL);

    size_t n = 0;
    bool wrapped = false;
    n += snprintf(arr + n, cap - n, "[");
    for (int i = 0; i < MAX_MEMBERS; i++) {
        if (n > cap) { wrapped = true; break; }   // cap - n would underflow
        n += snprintf(arr + n, cap - n, "%s\n      \"", i == 0 ? "" : ",");
        for (const char* p = members[i]; *p != '\0' && n < cap - 8; p++) {
            if (*p == '\\' || *p == '"') arr[n++] = '\\';
            arr[n++] = *p;
        }
        arr[n] = '\0';
        n += snprintf(arr + n, cap - n, "\"");
    }
    n += snprintf(arr + n, cap - n, "\n    ]");

    printf("    n=%zu cap=%zu headroom=%zd\n", n, cap, (ssize_t)cap - (ssize_t)n);
    report("no snprintf truncation, so `cap - n` never wraps", !wrapped && n < cap);
    report("final array fits inside cap", strlen(arr) < cap);
    free(arr);
}

// ---------------------------------------------------------------------------
// 3. RemoveMemberAt strcpy disjointness
// ---------------------------------------------------------------------------

static void test_remove_member_disjoint(void) {
    printf("RemoveMemberAt row shift (-fanalyzer false positive)\n");

    for (int i = 0; i < MAX_MEMBERS; i++) {
        memset(members[i], 'A', MAX_PATH - 1);
        members[i][MAX_PATH - 1] = '\0';
    }

    // A row is MAX_PATH bytes and a copy is at most MAX_PATH bytes (MAX_PATH-1
    // chars plus NUL), so dest [j*MAX_PATH, j*MAX_PATH+MAX_PATH-1] and source
    // [(j+1)*MAX_PATH, ...] never intersect.
    bool disjoint = true;
    for (int j = 0; j < MAX_MEMBERS - 1; j++) {
        char* d = members[j];
        char* s = members[j + 1];
        size_t copy = strlen(s) + 1;
        if (copy > (size_t)(s - d)) disjoint = false;
        strcpy(d, s);
    }
    report("all 128 max-length row shifts operate on disjoint memory", disjoint);
}

int main(void) {
    printf("drift regression tests\n\n");
    test_row_guards();
    printf("\n");
    test_save_members_bound();
    printf("\n");
    test_remove_member_disjoint();
    printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
