// Regression tests for the memory-safety fixes in drift.c.
//
// drift.c is Windows-only, so these tests mirror the exact arithmetic of the
// routines under test rather than linking against them. tests/lint_row_guards.c
// is the companion check that keeps drift.c itself in sync with the invariant
// proved here.
//
// Build and run:  tests/run_tests.sh   (Unix)
//                 tests\run_tests.bat  (Windows/MSVC)
//
// Covers:
//   1. WriteToBuffer row-guard invariant -- the frame buffer holds exactly
//      width*height cells, and no pane may write at a row >= height.
//      Regression for the three heap overflows at drift.c:658, 1955-1956 and
//      1521-1531 (each confirmed under ASAN before the fix).
//   2. SaveMembersTo's worst-case array size stays inside `cap`, so the
//      accumulation can never make `cap - n` wrap -- plus AppendFmt, the
//      clamped replacement for the bare `n += snprintf(...)` idiom.
//   3. RemoveMemberAt's row-shifting strcpy operates on disjoint memory
//      (GCC's -fanalyzer reports a false positive here).

#define _CRT_SECURE_NO_WARNINGS // MSVC deprecates strcpy; drift.c uses it too

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../settings_json.h"

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

    // long long, not ssize_t: the latter is POSIX-only and absent on MSVC
    printf("    n=%llu cap=%llu headroom=%lld\n", (unsigned long long)n,
           (unsigned long long)cap, (long long)cap - (long long)n);
    report("no snprintf truncation, so `cap - n` never wraps", !wrapped && n < cap);
    report("final array fits inside cap", strlen(arr) < cap);
    free(arr);
}

// ---------------------------------------------------------------------------
// 2b. AppendFmt clamp
// ---------------------------------------------------------------------------

// Verbatim from drift.c:AppendFmt.
static size_t AppendFmt(char* buf, size_t n, size_t cap, const char* fmt, ...) {
    if (cap == 0 || n >= cap - 1) return n;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (written < 0) return n;
    return (size_t)written >= cap - n ? cap - 1 : n + (size_t)written;
}

static void test_append_fmt_clamp(void) {
    printf("AppendFmt clamp (n can never exceed cap - 1)\n");

    // Deliberately overflow a tiny buffer: the bare "n += snprintf(...)" idiom
    // would push n past cap here, making the next cap - n wrap.
    enum { CAP = 16 };
    char buf[CAP];
    memset(buf, 0, sizeof(buf));

    size_t n = 0;
    bool in_bounds = true;
    for (int i = 0; i < 40; i++) {
        n = AppendFmt(buf, n, CAP, "0123456789");
        if (n > CAP - 1) in_bounds = false;
    }
    report("40 oversized appends keep n <= cap - 1", in_bounds);
    report("buffer stays NUL-terminated inside cap", strlen(buf) < CAP);

    // And it still appends correctly when there is room.
    char ok[64];
    memset(ok, 0, sizeof(ok));
    size_t m = 0;
    m = AppendFmt(ok, m, sizeof(ok), "[");
    m = AppendFmt(ok, m, sizeof(ok), "\"%s\"", "abc");
    m = AppendFmt(ok, m, sizeof(ok), "]");
    report("normal appends produce the expected text", strcmp(ok, "[\"abc\"]") == 0 && m == 7);
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

// ---------------------------------------------------------------------------
// 4. Settings JSON path targeting
// ---------------------------------------------------------------------------

// Return the exact text SaveMembersTo would splice over. The parser is included
// from the same header as production rather than copied into this test.
static bool spliced(const char* json, char* out, size_t out_size) {
    DriftSettingsJsonTarget target;
    if (!DriftLocateSettingsJsonTarget(json, strlen(json), &target) ||
        target.action != DRIFT_SETTINGS_JSON_REPLACE_ARRAY) return false;
    size_t n = (size_t)(target.array_end - target.array_start + 1);
    if (n >= out_size) return false;
    memcpy(out, json + target.array_start, n);
    out[n] = '\0';
    return true;
}

static void test_find_array_span(void) {
    char got[256];
    DriftSettingsJsonTarget target;
    printf("Settings JSON path targeting\n");

    report("plain key resolves to its own array",
           spliced("{\"permissions\":{\"additionalDirectories\":[\"a\"]}}",
                   got, sizeof got) && strcmp(got, "[\"a\"]") == 0);

    report("whitespace around ':' and '[' is tolerated",
           spliced("{\"permissions\":{\"additionalDirectories\"  :\n  [\"a\"]}}",
                   got, sizeof got) &&
           strcmp(got, "[\"a\"]") == 0);

    report("name appearing as a value does not retarget onto allow",
           !spliced("{\"env\":{\"DOC\":\"additionalDirectories\"},"
                    "\"permissions\":{\"allow\":[\"Bash(git:*)\"]}}",
                    got, sizeof got));

    const char* wrong_type =
        "{\"permissions\":{\"additionalDirectories\":null},"
        "\"hooks\":{\"x\":[\"a\"]}}";
    report("non-array value is unsafe rather than treated as missing",
           !DriftLocateSettingsJsonTarget(wrong_type, strlen(wrong_type), &target));

    report("a key merely ending in the name is not matched",
           spliced("{\"permissions\":{\"x-additionalDirectories\":[\"no\"],"
                   "\"additionalDirectories\":[\"yes\"]}}", got, sizeof got) &&
           strcmp(got, "[\"yes\"]") == 0);

    report("an escaped quote in an earlier string does not desync the scan",
           spliced("{\"note\":\"say \\\"additionalDirectories\\\" loudly\","
                   "\"permissions\":{\"additionalDirectories\":[\"ok\"]}}",
                   got, sizeof got) &&
           strcmp(got, "[\"ok\"]") == 0);

    report("a ']' inside a member string does not end the span early",
           spliced("{\"permissions\":{\"additionalDirectories\":[\"a]b\",\"c\"]}}",
                   got, sizeof got) &&
           strcmp(got, "[\"a]b\",\"c\"]") == 0);

    report("a supported plugin option cannot outrank the permissions path",
           spliced("{\"pluginConfigs\":{\"p@m\":{\"options\":{"
                   "\"additionalDirectories\":[\"plugin\"]}}},"
                   "\"permissions\":{\"additionalDirectories\":[\"permission\"]}}",
                   got, sizeof got) && strcmp(got, "[\"permission\"]") == 0);

    const char* insertion =
        "{\"companyAnnouncements\":[\"permissions\"],\"env\":{\"KEEP\":\"yes\"},"
        "\"permissions\":{\"allow\":[]}}";
    const char* permissions = strstr(insertion, "\"permissions\":{\"allow\"");
    report("a permissions string cannot retarget insertion into env",
           permissions != NULL &&
           DriftLocateSettingsJsonTarget(insertion, strlen(insertion), &target) &&
           target.action == DRIFT_SETTINGS_JSON_INSERT_IN_PERMISSIONS &&
           target.insert_at == (int)(strchr(permissions, '{') - insertion) + 1);

    const char* nested =
        "{\"env\":{\"permissions\":{\"additionalDirectories\":[\"nested\"]}}}";
    report("nested permissions does not replace a missing root object",
           DriftLocateSettingsJsonTarget(nested, strlen(nested), &target) &&
           target.action == DRIFT_SETTINGS_JSON_INSERT_PERMISSIONS &&
           target.insert_at == 1);

    report("Unicode-escaped semantic target keys are recognized",
           spliced("{\"permiss\\u0069ons\":{"
                   "\"additionalDirec\\u0074ories\":[\"ok\"]}}",
                   got, sizeof got) && strcmp(got, "[\"ok\"]") == 0);

    const char* duplicate =
        "{\"permissions\":{\"additionalDirectories\":[],"
        "\"\\u0061dditionalDirectories\":[]}}";
    report("duplicate semantic target keys fail closed",
           !DriftLocateSettingsJsonTarget(duplicate, strlen(duplicate), &target));

    const char* malformed = "{\"permissions\":";
    const char* trailing = "{} trailing";
    report("malformed and trailing-garbage documents fail closed",
           !DriftLocateSettingsJsonTarget(malformed, strlen(malformed), &target) &&
           !DriftLocateSettingsJsonTarget(trailing, strlen(trailing), &target));

    const char* unknown_values =
        "{\"yes\":true,\"no\":false,\"nothing\":null,"
        "\"number\":-12.5e+2}";
    report("valid unknown JSON value types remain editable",
           DriftLocateSettingsJsonTarget(unknown_values, strlen(unknown_values),
                                         &target) &&
           target.action == DRIFT_SETTINGS_JSON_INSERT_PERMISSIONS);

    const char* bad_numbers = "{\"x\":01}";
    report("invalid JSON number grammar fails closed",
           !DriftLocateSettingsJsonTarget(bad_numbers, strlen(bad_numbers),
                                          &target));

    const char* empty_root = "{}";
    const char* empty_permissions = "{\"permissions\":{}}";
    report("empty insertion parents require no comma",
           DriftLocateSettingsJsonTarget(empty_root, strlen(empty_root), &target) &&
           target.action == DRIFT_SETTINGS_JSON_INSERT_PERMISSIONS &&
           !target.needs_comma &&
           DriftLocateSettingsJsonTarget(empty_permissions,
                                         strlen(empty_permissions), &target) &&
           target.action == DRIFT_SETTINGS_JSON_INSERT_IN_PERMISSIONS &&
           !target.needs_comma);

    const char* trailing_commas = "{\"permissions\":{\"allow\":[],}}";
    report("JSON trailing commas fail closed",
           !DriftLocateSettingsJsonTarget(trailing_commas,
                                          strlen(trailing_commas), &target));

    const char* bad_escape = "{\"x\":\"\\q\"}";
    report("invalid JSON string escapes fail closed",
           !DriftLocateSettingsJsonTarget(bad_escape, strlen(bad_escape), &target));

    const char bom[] =
        "\xef\xbb\xbf{\"permissions\":{\"additionalDirectories\":[\"bom\"]}}";
    report("a UTF-8 BOM is preserved while offsets target the real array",
           spliced(bom, got, sizeof got) && strcmp(got, "[\"bom\"]") == 0);
}

// ---------------------------------------------------------------------------
// 5. LoadMembersFrom refuses lossy edits
// ---------------------------------------------------------------------------

static char lm_members[MAX_MEMBERS][MAX_PATH];
static int lm_member_count;
static const char* lm_block_reason;

// The member-parsing half of drift.c:LoadMembersFrom, verbatim apart from
// taking the document as a string rather than reading it from a file. The
// host-drive rewrite is omitted; it does not touch any of these paths.
static void parse_members(const char* json) {
    lm_member_count = 0;
    lm_block_reason = NULL;

    DriftSettingsJsonTarget target;
    if (!DriftLocateSettingsJsonTarget(json, strlen(json), &target)) {
        lm_block_reason = "(settings.json structure cannot be edited safely)";
        return;
    }
    if (target.action != DRIFT_SETTINGS_JSON_REPLACE_ARRAY) return;
    int s = target.array_start;
    int e = target.array_end;
    const char* p = json + s + 1;
    const char* stop = json + e;
    while (p < stop && lm_member_count < MAX_MEMBERS) {
        if (*p != '"') {
            p++;
            continue;
        }
        p++;
        char* out = lm_members[lm_member_count];
        int n = 0;
        while (p < stop && *p != '"' && n < MAX_PATH - 1) {
            if (*p == '\\' && p + 1 < stop) {
                p++;
                if (*p == 'u') {
                    lm_block_reason = "(unsupported \\u escape in settings)";
                    out[n++] = '?';
                    p++;
                    for (int k = 0; k < 4 && p < stop; k++) p++;
                    continue;
                }
                out[n++] = *p++;
            } else {
                out[n++] = *p++;
            }
        }
        out[n] = '\0';
        bool truncated = n >= MAX_PATH - 1 && p < stop && *p != '"';
        if (truncated) lm_block_reason = "(a folder path is too long to edit)";
        if (!truncated && n > 0) lm_member_count++;
        while (p < stop && *p != '"') p++;
        if (p < stop) p++;
    }

    if (lm_member_count == MAX_MEMBERS) {
        for (const char* q = p; q < stop; q++) {
            if (*q == '"') {
                lm_block_reason = "(too many folders to edit safely)";
                break;
            }
        }
    }
}

// {"permissions":{"additionalDirectories":["d0","d1",...]}} with count entries
static void build_list(char* buf, size_t cap, int count) {
    size_t n = 0;
    n += (size_t)snprintf(buf + n, cap - n,
                          "{\"permissions\":{\"additionalDirectories\":[");
    for (int i = 0; i < count; i++) {
        n += (size_t)snprintf(buf + n, cap - n, "%s\"d%d\"", i == 0 ? "" : ",", i);
    }
    snprintf(buf + n, cap - n, "]}}");
}

static void test_member_parse_refusals(void) {
    static char buf[16384];
    printf("LoadMembersFrom refuses lossy edits\n");

    parse_members("{\"permissions\":{\"additionalDirectories\":["
                  "\"C:\\\\a\",\"C:\\\\b\"]}}");
    report("ordinary entries load and stay editable",
           lm_member_count == 2 && lm_block_reason == NULL &&
           strcmp(lm_members[0], "C:\\a") == 0 &&
           strcmp(lm_members[1], "C:\\b") == 0);

    build_list(buf, sizeof buf, MAX_MEMBERS);
    parse_members(buf);
    report("exactly MAX_MEMBERS entries stay editable",
           lm_member_count == MAX_MEMBERS && lm_block_reason == NULL);

    // Regression: the tail past 128 was dropped on load, and the save rewrote
    // the array from what was read, deleting those entries from the file
    build_list(buf, sizeof buf, MAX_MEMBERS + 1);
    parse_members(buf);
    report("more than MAX_MEMBERS entries refuse the edit",
           lm_member_count == MAX_MEMBERS && lm_block_reason != NULL);

    // Regression: a path over MAX_PATH was kept as its 259-char prefix and
    // written back over the real entry
    size_t n = (size_t)snprintf(
        buf, sizeof buf, "{\"permissions\":{\"additionalDirectories\":[\"");
    for (int i = 0; i < 300; i++) buf[n++] = 'x';
    snprintf(buf + n, sizeof buf - n, "\"]}}");
    parse_members(buf);
    report("a path longer than MAX_PATH refuses the edit",
           lm_block_reason != NULL && lm_member_count == 0);

    // A path of exactly MAX_PATH-1 fills the buffer without being cut
    n = (size_t)snprintf(
        buf, sizeof buf, "{\"permissions\":{\"additionalDirectories\":[\"");
    for (int i = 0; i < MAX_PATH - 1; i++) buf[n++] = 'x';
    snprintf(buf + n, sizeof buf - n, "\"]}}");
    parse_members(buf);
    report("a path of exactly MAX_PATH-1 is not treated as truncated",
           lm_member_count == 1 && lm_block_reason == NULL);

    // Regression: \u was lowered to '?' and the '?' written back as the path
    parse_members("{\"permissions\":{\"additionalDirectories\":["
                  "\"C:\\\\Users\\\\jos\\u00e9\"]}}");
    report("a \\u escape refuses the edit", lm_block_reason != NULL);
}

// ---------------------------------------------------------------------------
// 6. IsRootDirectory
// ---------------------------------------------------------------------------

// Verbatim from drift.c:IsRootDirectory.
static bool IsRootDirectory(char* path) {
    if (strlen(path) == 3 && path[1] == ':' && path[2] == '\\') {
        return true;
    }
    if (path[0] == '\\' && path[1] == '\\') {
        const char* share = strchr(path + 2, '\\');
        if (share == NULL) {
            return true;
        }
        const char* next = strchr(share + 1, '\\');
        if (next == NULL || next[1] == '\0') {
            return true;
        }
    }
    return false;
}

static void test_is_root_directory(void) {
    printf("IsRootDirectory\n");

    report("a drive root is a root",
           IsRootDirectory("C:\\") && IsRootDirectory("d:\\"));
    report("a directory on a drive is not",
           !IsRootDirectory("C:\\Users") && !IsRootDirectory("C:\\Users\\me"));

    // Regression: these walked up to "\\server" and then to "\", whose search
    // pattern resolves against the local drive
    report("a UNC share root is a root",
           IsRootDirectory("\\\\server\\share"));
    report("a UNC share root with a trailing slash is a root",
           IsRootDirectory("\\\\server\\share\\"));
    report("a UNC server on its own is a root",
           IsRootDirectory("\\\\server"));
    report("a directory inside a UNC share is not",
           !IsRootDirectory("\\\\server\\share\\sub") &&
           !IsRootDirectory("\\\\server\\share\\sub\\deeper"));

    // Must not read past the end of a short string
    report("empty and one-character paths are handled",
           !IsRootDirectory("") && !IsRootDirectory("\\") && !IsRootDirectory("C"));
}

// ---------------------------------------------------------------------------
// 7. UTF-8 preview detection: trailing partial-sequence trim
// ---------------------------------------------------------------------------

// Verbatim from the detection block in drift.c:LoadPreview. The read stops at
// a fixed byte count and can land mid-character, so the bytes that could still
// be an unfinished sequence are excluded before the buffer is validated --
// otherwise one dangling lead byte sends the whole file to the wrong decoder.
static int utf8_trim(const unsigned char* bytes, int len) {
    int test = len;
    for (int back = 1; back <= 4 && back <= test; back++) {
        unsigned char c = bytes[test - back];
        if (c < 0x80) break;
        if (c >= 0xC0) {
            int need = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2);
            if (back < need) test -= back;
            break;
        }
    }
    return test;
}

static void test_utf8_trim(void) {
    printf("UTF-8 preview trailing-sequence trim\n");

    const unsigned char ascii[] = { 'a', 'b', 'c' };
    report("pure ASCII is never trimmed", utf8_trim(ascii, 3) == 3);

    // e-acute: C3 A9
    const unsigned char two_ok[] = { 'a', 0xC3, 0xA9 };
    const unsigned char two_cut[] = { 'a', 0xC3 };
    report("a complete 2-byte character is kept", utf8_trim(two_ok, 3) == 3);
    report("a cut 2-byte character is dropped", utf8_trim(two_cut, 2) == 1);

    // euro sign: E2 82 AC
    const unsigned char three_ok[] = { 'a', 0xE2, 0x82, 0xAC };
    const unsigned char three_cut2[] = { 'a', 0xE2, 0x82 };
    const unsigned char three_cut1[] = { 'a', 0xE2 };
    report("a complete 3-byte character is kept", utf8_trim(three_ok, 4) == 4);
    report("a 3-byte character missing one byte is dropped",
           utf8_trim(three_cut2, 3) == 1);
    report("a 3-byte character missing two bytes is dropped",
           utf8_trim(three_cut1, 2) == 1);

    // grinning face: F0 9F 98 80
    const unsigned char four_ok[] = { 'a', 0xF0, 0x9F, 0x98, 0x80 };
    const unsigned char four_cut[] = { 'a', 0xF0, 0x9F, 0x98 };
    report("a complete 4-byte character is kept", utf8_trim(four_ok, 5) == 5);
    report("a cut 4-byte character is dropped", utf8_trim(four_cut, 4) == 1);

    report("an empty buffer trims to nothing", utf8_trim(ascii, 0) == 0);
}

// ---------------------------------------------------------------------------
// 8. Utf8Prefix -- character-boundary slicing for the session detail pane
// ---------------------------------------------------------------------------

// Verbatim from drift.c:Utf8Prefix.
static int Utf8Prefix(const char* s, int cells, int max_bytes) {
    int n = 0;
    int used = 0;
    while (s[n] != '\0' && used < cells) {
        unsigned char b = (unsigned char)s[n];
        int adv = b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : (b >= 0xC0 ? 2 : 1));
        for (int k = 1; k < adv; k++) {
            if (s[n + k] == '\0') {
                adv = k;
                break;
            }
        }
        int cost = adv == 4 ? 2 : 1;
        if (n + adv > max_bytes || used + cost > cells) break;
        n += adv;
        used += cost;
    }
    return n;
}

static void test_utf8_prefix(void) {
    printf("Utf8Prefix character-boundary slicing\n");

    report("ASCII slices one byte per cell",
           Utf8Prefix("abcdef", 3, 99) == 3 && Utf8Prefix("abcdef", 99, 99) == 6);

    // "Caf" + e-acute (C3 A9): 4 characters, 5 bytes
    const char* cafe = "Caf\xC3\xA9";
    report("a 2-byte character is taken whole", Utf8Prefix(cafe, 4, 99) == 5);
    report("a 2-byte character that does not fit is left out",
           Utf8Prefix(cafe, 3, 99) == 3);
    report("max_bytes stops before splitting a character",
           Utf8Prefix(cafe, 99, 4) == 3);

    // Euro sign (E2 82 AC): 1 character, 3 bytes, 1 cell
    report("a 3-byte character costs one cell",
           Utf8Prefix("\xE2\x82\xAC", 1, 99) == 3);

    // Grinning face (F0 9F 98 80): 1 character, 4 bytes, a surrogate pair
    report("a 4-byte character costs two cells",
           Utf8Prefix("\xF0\x9F\x98\x80", 2, 99) == 4);
    report("a 4-byte character is refused when only one cell is left",
           Utf8Prefix("\xF0\x9F\x98\x80", 1, 99) == 0);

    // A lead byte with nothing after it must still advance, or the caller's
    // wrap loop would never terminate
    report("a truncated sequence still makes progress",
           Utf8Prefix("ab\xC3", 99, 99) == 3);

    report("an empty string yields nothing", Utf8Prefix("", 99, 99) == 0);
}

int main(void) {
    printf("drift regression tests\n\n");
    test_row_guards();
    printf("\n");
    test_save_members_bound();
    printf("\n");
    test_append_fmt_clamp();
    printf("\n");
    test_remove_member_disjoint();
    printf("\n");
    test_find_array_span();
    printf("\n");
    test_member_parse_refusals();
    printf("\n");
    test_is_root_directory();
    printf("\n");
    test_utf8_trim();
    printf("\n");
    test_utf8_prefix();
    printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
