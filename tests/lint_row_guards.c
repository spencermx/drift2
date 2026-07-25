// Static check for the WriteToBuffer row-guard invariant in drift.c.
//
// WriteToBuffer bounds the column it writes but not the row (it has no height to
// compare against), so the frame buffer -- exactly width*height cells -- is only
// safe if every caller checks the row itself. Three call sites once did not,
// which produced heap overflows in short console windows.
//
// This flags any WriteToBuffer(buffer, width, ROW, ...) call whose ROW is a
// literal >= 3 and which carries no reference to `height` on its own line or the
// line above.
//
// Rows 0-2 need no guard: DrawScreen refuses to draw below MIN_WINDOW_HEIGHT, and
// the header and rule at rows 0-1 are structural.
//
// Scope: the frame buffer only, matched by the parameter name `buffer`. Popup
// buffers are separate allocations, each already gated on its own popup_h.
//
// Ported from the original lint_row_guards.py so the check needs only a C
// compiler -- which the project already requires -- rather than a Python
// install that Windows machines generally lack.
//
// Build and run:  tests/run_tests.sh   (Unix)
//                 tests\run_tests.bat  (Windows/MSVC)
//
// Usage: lint_row_guards [path-to-drift.c]

#define _CRT_SECURE_NO_WARNINGS

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// drift.c's longest line is well under this; longer lines are still scanned,
// they just get split, which can only produce extra reports, never miss one
#define MAX_LINE 4096
#define UNGUARDED_MAX_ROW 2

static const char* CALL = "WriteToBuffer(";

static const char* SkipSpace(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// Matches an exact identifier, so `buffer2` does not satisfy a `buffer` match
static const char* ExpectIdent(const char* p, const char* word) {
    p = SkipSpace(p);
    size_t n = strlen(word);
    if (strncmp(p, word, n) != 0) return NULL;
    char c = p[n];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_') {
        return NULL;
    }
    return p + n;
}

static const char* ExpectChar(const char* p, char c) {
    p = SkipSpace(p);
    if (*p != c) return NULL;
    return p + 1;
}

// Equivalent of the Python regex
//   WriteToBuffer\(\s*buffer\s*,\s*width\s*,\s*([^,]+?)\s*,
// `p` points just past the opening paren. Returns the trimmed third argument.
static bool ParseCall(const char* p, char* row, size_t row_size) {
    p = ExpectIdent(p, "buffer");
    if (p == NULL) return false;
    p = ExpectChar(p, ',');
    if (p == NULL) return false;
    p = ExpectIdent(p, "width");
    if (p == NULL) return false;
    p = ExpectChar(p, ',');
    if (p == NULL) return false;

    p = SkipSpace(p);
    const char* start = p;
    while (*p != '\0' && *p != ',') p++;
    if (*p != ',') return false; // no further argument on this line
    const char* end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;

    size_t len = (size_t)(end - start);
    if (len == 0 || len >= row_size) return false;
    memcpy(row, start, len);
    row[len] = '\0';
    return true;
}

static bool AllDigits(const char* s) {
    if (*s == '\0') return false;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    const char* source = argc > 1 ? argv[1] : NULL;
    FILE* f = NULL;
    if (source != NULL) {
        f = fopen(source, "rb");
    } else {
        // Runnable from the repo root or from tests/
        source = "../drift.c";
        f = fopen(source, "rb");
        if (f == NULL) {
            source = "drift.c";
            f = fopen(source, "rb");
        }
    }
    if (f == NULL) {
        fprintf(stderr, "lint_row_guards: cannot read %s\n",
                source != NULL ? source : "drift.c");
        return 2;
    }

    // Report against the basename, so a path argument still reads as drift.c:N
    const char* name = source;
    for (const char* s = source; *s != '\0'; s++) {
        if (*s == '/' || *s == '\\') name = s + 1;
    }

    char line[MAX_LINE];
    char prev[MAX_LINE];
    prev[0] = '\0';
    int lineno = 0;
    int problems = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        for (const char* p = strstr(line, CALL); p != NULL;
             p = strstr(p + 1, CALL)) {
            char row[128];
            if (!ParseCall(p + strlen(CALL), row, sizeof(row))) continue;
            // A non-literal row (row++, height - 1, 4 + i) takes its bound from
            // the enclosing loop or guard, which this check does not model
            if (!AllDigits(row)) continue;
            if (atoi(row) <= UNGUARDED_MAX_ROW) continue;
            if (strstr(line, "height") != NULL) continue;
            if (strstr(prev, "height") != NULL) continue;

            problems++;
            printf("  %s:%d: writes literal row %s with no height guard\n",
                   name, lineno, row);
            printf("    %s\n", SkipSpace(line));
        }

        memcpy(prev, line, len + 1);
    }
    fclose(f);

    printf("lint_row_guards: scanned %s (%d lines)\n", name, lineno);
    if (problems > 0) {
        printf("\nFAIL (%d unguarded row write%s)\n", problems,
               problems == 1 ? "" : "s");
        return 1;
    }
    printf("  every literal row >= 3 into the frame buffer is height-guarded\n");
    printf("\nPASS\n");
    return 0;
}
