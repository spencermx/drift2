// Windows-only regression coverage for DRIFT-003. Include the production
// translation unit and intercept its actual stdio/publication calls so every
// failure path can be exercised deterministically without touching real data.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#undef _CRT_SECURE_NO_WARNINGS

typedef struct NameIoFaults {
    bool fail_read_open;
    bool fail_output_open;
    int fail_read_after_chunks;
    bool fail_copy_write;
    bool fail_append_write;
    bool fail_input_close;
    bool fail_output_close;
    bool fail_move;
    bool create_destination_before_move;
} NameIoFaults;

static NameIoFaults io_faults;
static char injected_file[MAX_PATH];
static char injected_tmp[MAX_PATH];
static FILE* tracked_input;
static FILE* tracked_output;
static int input_chunks;
static bool read_error_injected;
static int move_calls;
static DWORD last_move_flags;

static FILE* TestFopen(const char* path, const char* mode) {
    if (_stricmp(path, injected_file) == 0 && strcmp(mode, "rb") == 0) {
        if (io_faults.fail_read_open) {
            errno = EACCES;
            return NULL;
        }
        tracked_input = fopen(path, mode);
        return tracked_input;
    }
    if (_stricmp(path, injected_tmp) == 0 && strcmp(mode, "wb") == 0) {
        if (io_faults.fail_output_open) {
            errno = EACCES;
            return NULL;
        }
        tracked_output = fopen(path, mode);
        return tracked_output;
    }
    return fopen(path, mode);
}

static char* TestFgets(char* buffer, int count, FILE* stream) {
    if (stream == tracked_input && io_faults.fail_read_after_chunks >= 0 &&
        input_chunks >= io_faults.fail_read_after_chunks) {
        read_error_injected = true;
        return NULL;
    }
    char* result = fgets(buffer, count, stream);
    if (stream == tracked_input && result != NULL) input_chunks++;
    return result;
}

static int TestFerror(FILE* stream) {
    if (stream == tracked_input && read_error_injected) return 1;
    return ferror(stream);
}

static int TestFputs(const char* text, FILE* stream) {
    if (stream == tracked_output && io_faults.fail_copy_write) return EOF;
    return fputs(text, stream);
}

static int TestFprintf(FILE* stream, const char* format, ...) {
    if (stream == tracked_output && io_faults.fail_append_write) return -1;
    va_list args;
    va_start(args, format);
    int result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}

static int TestFclose(FILE* stream) {
    bool fail = (stream == tracked_input && io_faults.fail_input_close) ||
                (stream == tracked_output && io_faults.fail_output_close);
    int result = fclose(stream);
    if (stream == tracked_input) tracked_input = NULL;
    if (stream == tracked_output) tracked_output = NULL;
    return fail ? EOF : result;
}

static BOOL TestMoveFileEx(LPCSTR existing, LPCSTR replacement, DWORD flags) {
    move_calls++;
    last_move_flags = flags;
    if (io_faults.fail_move) {
        SetLastError(ERROR_SHARING_VIOLATION);
        return FALSE;
    }
    if (io_faults.create_destination_before_move) {
        FILE* concurrent = fopen(replacement, "wb");
        bool wrote = concurrent != NULL &&
                     fputs("other\tConcurrent\n", concurrent) != EOF;
        bool closed = concurrent != NULL && fclose(concurrent) == 0;
        if (!wrote || !closed) {
            SetLastError(ERROR_WRITE_FAULT);
            return FALSE;
        }
    }
    return MoveFileExA(existing, replacement, flags);
}

#define fopen TestFopen
#define fgets TestFgets
#define ferror TestFerror
#define fputs TestFputs
#define fprintf TestFprintf
#define fclose TestFclose
#undef MoveFileEx
#define MoveFileEx TestMoveFileEx
#define main drift_application_main
#include "../drift.c"
#undef main
#undef MoveFileEx
#undef fclose
#undef fprintf
#undef fputs
#undef ferror
#undef fgets
#undef fopen

static int test_failures = 0;

static void TestReport(const char* name, bool passed) {
    printf("  %-72s %s\n", name, passed ? "PASS" : "FAIL");
    if (!passed) test_failures++;
}

static void ResetFaults(void) {
    memset(&io_faults, 0, sizeof(io_faults));
    io_faults.fail_read_after_chunks = -1;
    tracked_input = NULL;
    tracked_output = NULL;
    input_chunks = 0;
    read_error_injected = false;
    move_calls = 0;
    last_move_flags = 0;
}

static bool TestWrite(const char* path, const char* contents) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) return false;
    bool wrote = fputs(contents, file) >= 0;
    bool closed = fclose(file) == 0;
    return wrote && closed;
}

static bool TestFileEquals(const char* path, const char* expected) {
    char actual[4096];
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    size_t count = fread(actual, 1, sizeof(actual) - 1, file);
    bool complete = !ferror(file) && fgetc(file) == EOF;
    bool closed = fclose(file) == 0;
    actual[count] = '\0';
    return complete && closed && strcmp(actual, expected) == 0;
}

static bool TempIsAbsent(void) {
    return GetFileAttributes(injected_tmp) == INVALID_FILE_ATTRIBUTES;
}

static bool PrepareFixture(const char* contents) {
    DeleteFile(injected_file);
    DeleteFile(injected_tmp);
    return TestWrite(injected_file, contents);
}

static bool FailurePreserved(bool result, const char* original) {
    return !result && TestFileEquals(injected_file, original) &&
           TempIsAbsent() && workspace_names_loaded && move_calls == 0;
}

int main(void) {
    static const char* fixture =
        "keep-a\tAlpha\n"
        "keep-b\tBravo\n"
        "target\tOld\n"
        "keep-c\tCharlie\n";
    static const char* replaced =
        "keep-a\tAlpha\n"
        "keep-b\tBravo\n"
        "keep-c\tCharlie\n"
        "target\tNew\n";
    static const char* deleted =
        "keep-a\tAlpha\n"
        "keep-b\tBravo\n"
        "keep-c\tCharlie\n";

    char temp[MAX_PATH];
    char root[MAX_PATH];
    DWORD temp_len = GetTempPath(MAX_PATH, temp);
    int root_len = temp_len > 0 && temp_len < MAX_PATH
        ? snprintf(root, sizeof(root), "%sdrift-names-%lu-%lu", temp,
                   (unsigned long)GetCurrentProcessId(),
                   (unsigned long)GetTickCount())
        : -1;
    if (root_len < 0 || root_len >= (int)sizeof(root) ||
        !CreateDirectory(root, NULL) ||
        !SetEnvironmentVariable("DRIFT_HOME", root) ||
        !GetNameFile(injected_file, WORKSPACE_NAMES_FILE)) {
        fprintf(stderr, "Could not create the DRIFT-003 test root.\n");
        return 1;
    }
    int tmp_len = snprintf(injected_tmp, sizeof(injected_tmp), "%s.tmp",
                           injected_file);
    if (tmp_len < 0 || tmp_len >= (int)sizeof(injected_tmp)) {
        fprintf(stderr, "Could not create the DRIFT-003 temp path.\n");
        return 1;
    }

    ResetFaults();
    bool ready = PrepareFixture(fixture);
    workspace_names_loaded = true;
    io_faults.fail_read_open = true;
    bool saved = ready && SetWorkspaceName("target", "New");
    TestReport("an unreadable existing file is preserved and reported as failure",
               ready && FailurePreserved(saved, fixture));

    ResetFaults();
    ready = PrepareFixture(fixture);
    workspace_names_loaded = true;
    io_faults.fail_output_open = true;
    saved = ready && SetNameEntry(WORKSPACE_NAMES_FILE, "target", "New");
    TestReport("failure to open the temp leaves the original untouched",
               ready && FailurePreserved(saved, fixture));

    ResetFaults();
    ready = PrepareFixture(fixture);
    workspace_names_loaded = true;
    io_faults.fail_read_after_chunks = 1;
    saved = ready && SetNameEntry(WORKSPACE_NAMES_FILE, "target", "New");
    TestReport("a mid-read error cannot publish the copied prefix",
               ready && FailurePreserved(saved, fixture));

    ResetFaults();
    ready = PrepareFixture(fixture);
    workspace_names_loaded = true;
    io_faults.fail_copy_write = true;
    saved = ready && SetNameEntry(WORKSPACE_NAMES_FILE, "target", "New");
    TestReport("a copy write error preserves the original and removes the temp",
               ready && FailurePreserved(saved, fixture));

    ResetFaults();
    ready = PrepareFixture(fixture);
    workspace_names_loaded = true;
    io_faults.fail_append_write = true;
    saved = ready && SetNameEntry(WORKSPACE_NAMES_FILE, "target", "New");
    TestReport("an appended-row write error cannot publish the temp",
               ready && FailurePreserved(saved, fixture));

    ResetFaults();
    ready = PrepareFixture(fixture);
    workspace_names_loaded = true;
    io_faults.fail_input_close = true;
    saved = ready && SetNameEntry(WORKSPACE_NAMES_FILE, "target", "New");
    TestReport("an input close error preserves the original",
               ready && FailurePreserved(saved, fixture));

    ResetFaults();
    ready = PrepareFixture(fixture);
    workspace_names_loaded = true;
    io_faults.fail_output_close = true;
    saved = ready && SetNameEntry(WORKSPACE_NAMES_FILE, "target", "New");
    TestReport("an output flush/close error preserves the original",
               ready && FailurePreserved(saved, fixture));

    ResetFaults();
    ready = PrepareFixture(fixture);
    workspace_names_loaded = true;
    io_faults.fail_move = true;
    saved = ready && SetNameEntry(WORKSPACE_NAMES_FILE, "target", "New");
    TestReport("a publication error preserves the original and cached names",
               ready && !saved && TestFileEquals(injected_file, fixture) &&
               TempIsAbsent() && workspace_names_loaded && move_calls == 1);

    ResetFaults();
    ready = PrepareFixture(fixture);
    workspace_names_loaded = true;
    saved = ready && SetNameEntry(WORKSPACE_NAMES_FILE, "target", "New");
    TestReport("successful replacement preserves unrelated rows in order",
               ready && saved && TestFileEquals(injected_file, replaced) &&
               !workspace_names_loaded && move_calls == 1 &&
               last_move_flags == MOVEFILE_REPLACE_EXISTING);

    ResetFaults();
    ready = PrepareFixture(fixture);
    saved = ready && SetNameEntry(WORKSPACE_NAMES_FILE, "target", "");
    TestReport("an empty name removes only the matching row",
               ready && saved && TestFileEquals(injected_file, deleted));

    ResetFaults();
    DeleteFile(injected_file);
    DeleteFile(injected_tmp);
    workspace_names_loaded = true;
    io_faults.create_destination_before_move = true;
    saved = SetNameEntry(WORKSPACE_NAMES_FILE, "target", "New");
    TestReport("first-file publication never replaces a concurrently created file",
               !saved && TestFileEquals(injected_file, "other\tConcurrent\n") &&
               TempIsAbsent() && workspace_names_loaded && move_calls == 1 &&
               last_move_flags == 0);

    ResetFaults();
    DeleteFile(injected_file);
    DeleteFile(injected_tmp);
    workspace_names_loaded = true;
    saved = SetNameEntry(WORKSPACE_NAMES_FILE, "target", "New");
    TestReport("first-file creation is exact and refuses replacement semantics",
               saved && TestFileEquals(injected_file, "target\tNew\n") &&
               !workspace_names_loaded && move_calls == 1 && last_move_flags == 0);

    char long_fixture[2048];
    int prefix = snprintf(long_fixture, sizeof(long_fixture), "target\t");
    if (prefix > 0 && prefix + 1200 + 12 < (int)sizeof(long_fixture)) {
        memset(long_fixture + prefix, 'x', 1200);
        strcpy(long_fixture + prefix + 1200, "\nkeep\tSafe\n");
        ResetFaults();
        ready = PrepareFixture(long_fixture);
        saved = ready && SetNameEntry(WORKSPACE_NAMES_FILE, "target", "");
        TestReport("an over-long matching row is removed through its newline",
                   ready && saved && TestFileEquals(injected_file, "keep\tSafe\n"));
    } else {
        TestReport("an over-long matching row is removed through its newline", false);
    }

    DeleteFile(injected_tmp);
    DeleteFile(injected_file);
    char drift_dir[MAX_PATH];
    snprintf(drift_dir, sizeof(drift_dir), "%s\\.drift", root);
    RemoveDirectory(drift_dir);
    RemoveDirectory(root);
    SetEnvironmentVariable("DRIFT_HOME", NULL);

    if (test_failures == 0) {
        printf("All name metadata tests passed.\n");
        return 0;
    }
    printf("%d name metadata test(s) failed.\n", test_failures);
    return 1;
}
