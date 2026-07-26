// Windows-only regression coverage for DRIFT-007 and its DRIFT-003 coupling.
// Include the production translation unit and intercept the console and shell
// boundaries so resize/failure sequences are deterministic and no real
// transcript or configuration can be changed.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef _CRT_SECURE_NO_WARNINGS

#define TEST_SCRIPT_CAPACITY 16
#define TEST_POPUP_CAPACITY 8

typedef struct TestWindow {
    SHORT left;
    SHORT top;
    SHORT width;
    SHORT height;
} TestWindow;

typedef struct TestInput {
    INPUT_RECORD record;
    bool changes_window;
    TestWindow window;
} TestInput;

static TestWindow test_window;
static TestInput test_script[TEST_SCRIPT_CAPACITY];
static int test_script_count;
static int test_script_index;
static int test_info_calls;
static int test_fail_info_call;
static int test_popup_writes;
static int test_screen_writes;
static int test_fail_popup_write;
static int test_clip_popup_write;
static SMALL_RECT test_popup_regions[TEST_POPUP_CAPACITY];
static bool test_popup_visible;
static bool test_fail_malloc;
static int test_shell_calls;
static bool test_shell_called_while_visible;
static char test_shell_path[MAX_PATH];
static int test_shell_result;
static int test_flush_calls;

static BOOL WINAPI TestGetConsoleScreenBufferInfo(
    HANDLE console, PCONSOLE_SCREEN_BUFFER_INFO info) {
    (void)console;
    test_info_calls++;
    if (test_info_calls == test_fail_info_call) return FALSE;

    memset(info, 0, sizeof(*info));
    info->dwSize.X = (SHORT)(test_window.left + test_window.width);
    info->dwSize.Y = (SHORT)(test_window.top + test_window.height);
    info->srWindow.Left = test_window.left;
    info->srWindow.Top = test_window.top;
    info->srWindow.Right = (SHORT)(test_window.left + test_window.width - 1);
    info->srWindow.Bottom = (SHORT)(test_window.top + test_window.height - 1);
    return TRUE;
}

static BOOL WINAPI TestWriteConsoleOutputW(
    HANDLE console, const CHAR_INFO* source, COORD buffer_size,
    COORD buffer_coord, PSMALL_RECT region) {
    (void)console;
    (void)buffer_coord;
    bool is_popup = source != NULL && buffer_size.Y == 7 &&
                    source[0].Char.UnicodeChar == L'\x250C';

    if (!is_popup) {
        test_screen_writes++;
        test_popup_visible = false;
        return TRUE;
    }

    test_popup_writes++;
    if (test_popup_writes <= TEST_POPUP_CAPACITY) {
        test_popup_regions[test_popup_writes - 1] = *region;
    }
    if (test_popup_writes == test_fail_popup_write) {
        test_popup_visible = false;
        return FALSE;
    }
    if (test_popup_writes == test_clip_popup_write) {
        region->Right--;
        test_popup_visible = false;
        return TRUE;
    }

    test_popup_visible = true;
    return TRUE;
}

static BOOL WINAPI TestReadConsoleInput(
    HANDLE console, PINPUT_RECORD record, DWORD length, LPDWORD events) {
    (void)console;
    if (length == 0 || test_script_index >= test_script_count) {
        *events = 0;
        return FALSE;
    }

    TestInput* scripted = &test_script[test_script_index++];
    if (scripted->changes_window) {
        // Model console reflow before Drift receives the ordered resize event:
        // the old one-shot overlay is no longer visible at the new geometry.
        test_window = scripted->window;
        test_popup_visible = false;
    }
    *record = scripted->record;
    *events = 1;
    return TRUE;
}

static BOOL WINAPI TestFlushConsoleInputBuffer(HANDLE console) {
    (void)console;
    test_flush_calls++;
    test_script_index = test_script_count;
    return TRUE;
}

static int WINAPI TestSHFileOperation(LPSHFILEOPSTRUCTA operation) {
    test_shell_calls++;
    test_shell_called_while_visible = test_popup_visible;
    if (operation != NULL && operation->pFrom != NULL) {
        snprintf(test_shell_path, sizeof(test_shell_path), "%s",
                 operation->pFrom);
    }
    // Refuse the operation after observing it. The real shell implementation
    // is never reachable from this translation unit.
    return test_shell_result;
}

static void* TestMalloc(size_t size) {
    if (test_fail_malloc) return NULL;
    return malloc(size);
}

#define GetConsoleScreenBufferInfo TestGetConsoleScreenBufferInfo
#define WriteConsoleOutputW TestWriteConsoleOutputW
#undef ReadConsoleInput
#define ReadConsoleInput TestReadConsoleInput
#define FlushConsoleInputBuffer TestFlushConsoleInputBuffer
#undef SHFileOperation
#define SHFileOperation TestSHFileOperation
#define malloc TestMalloc
#define main drift_application_main
#include "../drift.c"
#undef main
#undef malloc
#undef SHFileOperation
#undef FlushConsoleInputBuffer
#undef ReadConsoleInput
#undef WriteConsoleOutputW
#undef GetConsoleScreenBufferInfo

static int test_failures;
static char test_isolation_root[MAX_PATH];

static void TestReport(const char* name, bool passed) {
    printf("  %-72s %s\n", name, passed ? "PASS" : "FAIL");
    if (!passed) test_failures++;
}

static void SetTestWindow(SHORT left, SHORT top, SHORT width, SHORT height) {
    test_window.left = left;
    test_window.top = top;
    test_window.width = width;
    test_window.height = height;
}

static void QueueKey(WORD key) {
    TestInput* input = &test_script[test_script_count++];
    memset(input, 0, sizeof(*input));
    input->record.EventType = KEY_EVENT;
    input->record.Event.KeyEvent.bKeyDown = TRUE;
    input->record.Event.KeyEvent.wVirtualKeyCode = key;
}

static void QueueResize(SHORT left, SHORT top, SHORT width, SHORT height) {
    TestInput* input = &test_script[test_script_count++];
    memset(input, 0, sizeof(*input));
    input->record.EventType = WINDOW_BUFFER_SIZE_EVENT;
    input->record.Event.WindowBufferSizeEvent.dwSize.X = width;
    input->record.Event.WindowBufferSizeEvent.dwSize.Y = height;
    input->changes_window = true;
    input->window.left = left;
    input->window.top = top;
    input->window.width = width;
    input->window.height = height;
}

static bool RegionEquals(const SMALL_RECT* region, SHORT left, SHORT top,
                         SHORT right, SHORT bottom) {
    return region->Left == left && region->Top == top &&
           region->Right == right && region->Bottom == bottom;
}

static void ResetCase(void) {
    memset(test_script, 0, sizeof(test_script));
    memset(test_popup_regions, 0, sizeof(test_popup_regions));
    test_script_count = 0;
    test_script_index = 0;
    test_info_calls = 0;
    test_fail_info_call = -1;
    test_popup_writes = 0;
    test_screen_writes = 0;
    test_fail_popup_write = -1;
    test_clip_popup_write = -1;
    test_popup_visible = false;
    test_fail_malloc = false;
    test_shell_calls = 0;
    test_shell_called_while_visible = false;
    test_shell_path[0] = '\0';
    test_shell_result = ERROR_ACCESS_DENIED;
    test_flush_calls = 0;
    SetTestWindow(2, 3, 80, 25);

    hAlt = (HANDLE)(UINT_PTR)1;
    hIn = (HANDLE)(UINT_PTR)2;
    claude_mode = CM_SESSIONS;
    current_directory_file_count = 0;
    session_count = 1;
    session_selected = 0;
    session_top = 0;
    memset(sessions, 0, sizeof(sessions));
    snprintf(sessions[0].path, sizeof(sessions[0].path),
             "C:\\fake\\original-session.jsonl");
    snprintf(sessions[0].id, sizeof(sessions[0].id), "original-session");
    snprintf(sessions[0].name, sizeof(sessions[0].name), "Original session");
    snprintf(claude_workspace, sizeof(claude_workspace),
             "C:\\fake\\workspace");
    snprintf(claude_workspace_name, sizeof(claude_workspace_name), "workspace");
    snprintf(sessions_loaded_for, sizeof(sessions_loaded_for), "%s",
             claude_workspace);
    workspace_names_loaded = true;
    free(workspace_names);
    workspace_names = NULL;
}

int main(void) {
    char temp[MAX_PATH];
    DWORD temp_len = GetTempPath(MAX_PATH, temp);
    int root_len = temp_len > 0 && temp_len < MAX_PATH
        ? snprintf(test_isolation_root, sizeof(test_isolation_root),
                   "%sdrift-session-delete-test-%lu-%lu", temp,
                   (unsigned long)GetCurrentProcessId(),
                   (unsigned long)GetTickCount())
        : -1;
    if (root_len < 0 || root_len >= (int)sizeof(test_isolation_root) ||
        !SetEnvironmentVariable("DRIFT_HOME", test_isolation_root) ||
        !SetEnvironmentVariable("DRIFT_CLAUDE_DIR", test_isolation_root)) {
        fprintf(stderr, "Could not establish the isolated DRIFT-007 test root.\n");
        return 1;
    }

    ResetCase();
    QueueResize(7, 4, 100, 30);
    QueueKey('Y');
    HandleDeleteSession();
    TestReport("resize redraws the screen and repaints before accepting Y",
               test_screen_writes == 1 && test_popup_writes == 2 &&
               test_shell_calls == 1 && test_shell_called_while_visible);
    TestReport("the repainted prompt is centered in the current window",
               test_popup_writes == 2 &&
               RegionEquals(&test_popup_regions[1], 35, 15, 78, 21));
    TestReport("resize confirmation retains the originally displayed target",
               strcmp(test_shell_path,
                      "C:\\fake\\original-session.jsonl") == 0);

    ResetCase();
    QueueResize(0, 0, 19, 6);
    QueueKey('Y');
    HandleDeleteSession();
    TestReport("a resize too small for the prompt cancels deletion",
               test_popup_writes == 1 && test_shell_calls == 0);

    ResetCase();
    test_fail_popup_write = 2;
    QueueResize(0, 0, 90, 28);
    QueueKey('Y');
    HandleDeleteSession();
    TestReport("failure to repaint after resize cancels deletion",
               test_popup_writes == 2 && test_shell_calls == 0);

    ResetCase();
    test_fail_popup_write = 1;
    QueueKey('Y');
    HandleDeleteSession();
    TestReport("an initial popup write failure cancels deletion",
               test_popup_writes == 1 && test_shell_calls == 0);

    ResetCase();
    test_clip_popup_write = 1;
    QueueKey('Y');
    HandleDeleteSession();
    TestReport("a clipped popup write is not treated as confirmation",
               test_popup_writes == 1 && test_shell_calls == 0);

    ResetCase();
    test_fail_info_call = 1;
    QueueKey('Y');
    HandleDeleteSession();
    TestReport("console geometry failure cancels deletion",
               test_popup_writes == 0 && test_shell_calls == 0);

    ResetCase();
    test_fail_malloc = true;
    QueueKey('Y');
    HandleDeleteSession();
    TestReport("popup allocation failure cancels deletion",
               test_popup_writes == 0 && test_shell_calls == 0);

    ResetCase();
    QueueKey('N');
    HandleDeleteSession();
    TestReport("N cancels a visible confirmation",
               test_popup_writes == 1 && test_shell_calls == 0);

    ResetCase();
    QueueKey(VK_ESCAPE);
    HandleDeleteSession();
    TestReport("Escape cancels a visible confirmation",
               test_popup_writes == 1 && test_shell_calls == 0);

    ResetCase();
    QueueKey('J');
    QueueKey('Y');
    HandleDeleteSession();
    TestReport("an unrelated key preserves a visible prompt and its target",
               test_popup_writes == 1 && test_shell_calls == 1 &&
               test_shell_called_while_visible &&
               strcmp(test_shell_path,
                      "C:\\fake\\original-session.jsonl") == 0);

    ResetCase();
    QueueKey('Y');
    HandleDeleteSession();
    TestReport("ordinary Y reaches only the intercepted shell boundary",
               test_popup_writes == 1 && test_shell_calls == 1 &&
               test_shell_called_while_visible && test_flush_calls == 1);

    ResetCase();
    test_shell_result = 0;
    QueueKey('Y');
    HandleDeleteSession();
    TestReport("DRIFT-003 cleanup failure still reports the partial result",
               test_popup_writes == 1 && test_shell_calls == 1 &&
               test_shell_called_while_visible && test_screen_writes == 1 &&
               session_count == 0);

    ResetCase();
    HandleDeleteSession();
    TestReport("console input failure cancels after a visible prompt",
               test_popup_writes == 1 && test_shell_calls == 0);

    TestReport("the isolated test root remains absent",
               GetFileAttributes(test_isolation_root) ==
                   INVALID_FILE_ATTRIBUTES);

    if (test_failures == 0) {
        printf("All session-delete modal tests passed.\n");
    } else {
        printf("%d session-delete modal test(s) failed.\n", test_failures);
    }
    return test_failures == 0 ? 0 : 1;
}
