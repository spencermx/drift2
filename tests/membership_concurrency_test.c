// Windows production-linked regression coverage for DRIFT-005. Child modes
// exercise the real cross-process lock; every filesystem fixture stays below a
// uniquely named disposable %TEMP% workspace.
#define DRIFT_MEMBER_LOCK_TIMEOUT_MS 400
#define DRIFT_MEMBER_LOCK_RETRY_MS 10
#define _CRT_SECURE_NO_WARNINGS
#define main drift_application_main
#include "../src/drift.c"
#undef main

#define CHILD_RESULT_BASE 20

static int test_failures;
static char test_root[MAX_PATH];
static char test_anchor[MAX_PATH];
static char test_claude_dir[MAX_PATH];
static char test_settings[MAX_PATH];
static char test_tmp[MAX_PATH];
static char test_lock[MAX_PATH];
static unsigned char read_buffer[70000];
static char prior_host_drive[32768];
static bool had_host_drive;

static void TestReport(const char* name, bool passed) {
    printf("  %-78s %s\n", name, passed ? "PASS" : "FAIL");
    if (!passed) test_failures++;
}

static bool TestWriteBytes(const void* contents, size_t length) {
    FILE* file = fopen(test_settings, "wb");
    if (file == NULL) return false;
    bool wrote = fwrite(contents, 1, length, file) == length;
    bool closed = fclose(file) == 0;
    return wrote && closed;
}

static bool TestWriteText(const char* contents) {
    return TestWriteBytes(contents, strlen(contents));
}

static bool TestReadBytes(size_t* length) {
    FILE* file = fopen(test_settings, "rb");
    if (file == NULL) return false;
    size_t count = fread(read_buffer, 1, sizeof(read_buffer), file);
    bool complete = !ferror(file) && count < sizeof(read_buffer) &&
                    fgetc(file) == EOF;
    bool closed = fclose(file) == 0;
    if (length != NULL) *length = count;
    return complete && closed;
}

static bool TestFileEqualsText(const char* expected) {
    size_t length = 0;
    return TestReadBytes(&length) && length == strlen(expected) &&
           memcmp(read_buffer, expected, length) == 0;
}

static bool TestPrepareText(const char* contents) {
    DeleteFile(test_tmp);
    DeleteFile(test_settings);
    return TestWriteText(contents);
}

static bool TestTmpAbsent(void) {
    return GetFileAttributes(test_tmp) == INVALID_FILE_ATTRIBUTES;
}

static bool HasExactMembers(const char* first, const char* second,
                            const char* third, int expected_count) {
    LoadMembersFrom(test_anchor);
    if (json_block_reason != NULL || member_count != expected_count) return false;
    const char* expected[] = { first, second, third };
    for (int i = 0; i < expected_count; i++) {
        if (expected[i] == NULL || FindMember(expected[i]) < 0) return false;
    }
    return true;
}

static bool StartChild(const char* mode, const char* path,
                       const char* event_name, PROCESS_INFORMATION* process) {
    if (!SetEnvironmentVariable("DRIFT_TEST_ANCHOR", test_anchor)) return false;
    if (!SetEnvironmentVariable("DRIFT_TEST_MEMBER", path)) return false;
    if (!SetEnvironmentVariable("DRIFT_TEST_EVENT", event_name)) return false;

    char application[MAX_PATH];
    DWORD length = GetModuleFileName(NULL, application, sizeof(application));
    if (length == 0 || length >= sizeof(application)) return false;
    char command[MAX_PATH + 80];
    if (snprintf(command, sizeof(command), "\"%s\" %s", application, mode) >=
        (int)sizeof(command)) return false;

    STARTUPINFO startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(process, 0, sizeof(*process));
    bool started = CreateProcess(application, command, NULL, NULL, FALSE, 0,
                                 NULL, NULL, &startup, process) != 0;
    if (started) CloseHandle(process->hThread);
    return started;
}

static bool FinishChild(PROCESS_INFORMATION* process, DWORD timeout,
                        DWORD* exit_code) {
    DWORD waited = WaitForSingleObject(process->hProcess, timeout);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process->hProcess, 99);
        WaitForSingleObject(process->hProcess, 5000);
        CloseHandle(process->hProcess);
        return false;
    }
    bool read = GetExitCodeProcess(process->hProcess, exit_code) != 0;
    CloseHandle(process->hProcess);
    return read;
}

static int ChildMain(const char* mode) {
    char anchor[MAX_PATH];
    DWORD anchor_length = GetEnvironmentVariable(
        "DRIFT_TEST_ANCHOR", anchor, sizeof(anchor));
    if (anchor_length == 0 || anchor_length >= sizeof(anchor)) return 2;

    if (strcmp(mode, "--child-add") == 0) {
        char path[MAX_PATH];
        DWORD path_length = GetEnvironmentVariable(
            "DRIFT_TEST_MEMBER", path, sizeof(path));
        if (path_length == 0 || path_length >= sizeof(path)) return 3;
        enum MemberChangeResult result = ApplyMemberChange(
            anchor, path, MEMBER_CHANGE_ADD);
        return CHILD_RESULT_BASE + (int)result;
    }

    if (strcmp(mode, "--child-hold-lock") == 0) {
        char event_name[128];
        DWORD event_length = GetEnvironmentVariable(
            "DRIFT_TEST_EVENT", event_name, sizeof(event_name));
        if (event_length == 0 || event_length >= sizeof(event_name)) return 4;
        HANDLE lock;
        if (AcquireMemberLock(anchor, &lock) != MEMBER_LOCK_ACQUIRED) return 5;
        HANDLE event = OpenEvent(EVENT_MODIFY_STATE, FALSE, event_name);
        if (event == NULL) {
            CloseHandle(lock);
            return 6;
        }
        SetEvent(event);
        CloseHandle(event);
        Sleep(INFINITE);
        CloseHandle(lock);
        return 7;
    }
    return 8;
}

static void TestStaleOperationsRebase(void) {
    const char* one =
        "{\"permissions\":{\"additionalDirectories\":[\"base\"]}}";
    const char* one_plus_b =
        "{\"permissions\":{\"additionalDirectories\":[\"base\",\"B-added\"]}}";
    bool prepared = TestPrepareText(one);
    LoadMembersFrom(test_anchor); // deliberately stale display snapshot
    bool external = TestWriteText(one_plus_b);
    enum MemberChangeResult result = ApplyMemberChange(
        test_anchor, "A-added", MEMBER_CHANGE_ADD);
    TestReport("a stale add rebases and preserves the other writer's addition",
               prepared && external && result == MEMBER_CHANGE_SAVED &&
               HasExactMembers("base", "B-added", "A-added", 3));

    const char* two =
        "{\"permissions\":{\"additionalDirectories\":[\"base\",\"victim\"]}}";
    const char* two_plus_b =
        "{\"permissions\":{\"additionalDirectories\":[\"base\",\"victim\",\"B-added\"]}}";
    prepared = TestPrepareText(two);
    LoadMembersFrom(test_anchor);
    external = TestWriteText(two_plus_b);
    result = ApplyMemberChange(test_anchor, "victim", MEMBER_CHANGE_REMOVE);
    TestReport("a stale removal preserves an unrelated concurrent addition",
               prepared && external && result == MEMBER_CHANGE_SAVED &&
               HasExactMembers("base", "B-added", NULL, 2) &&
               FindMember("victim") < 0);

    prepared = TestPrepareText(two);
    LoadMembersFrom(test_anchor);
    external = TestWriteText(one); // another writer removed victim
    result = ApplyMemberChange(test_anchor, "A-added", MEMBER_CHANGE_ADD);
    TestReport("a stale add cannot resurrect another writer's removal",
               prepared && external && result == MEMBER_CHANGE_SAVED &&
               HasExactMembers("base", "A-added", NULL, 2) &&
               FindMember("victim") < 0);
}

static void TestTargetSourceConflict(void) {
    const char* original =
        "{\"permissions\":{\"additionalDirectories\":[\"base\"]}}";
    const char* external =
        "{\"permissions\":{\"additionalDirectories\":[\"base\",\"external\"]}}";
    bool prepared = TestPrepareText(original);
    LoadMembersFrom(test_anchor);
    strcpy(members[member_count], "stale-local");
    member_count++;
    bool changed = TestWriteText(external);
    bool refused = !SaveMembersTo(test_anchor);
    TestReport("save refuses a target-array change after its transaction load",
               prepared && changed && refused && json_block_reason != NULL &&
               strcmp(json_block_reason, MEMBER_CONFLICT_REASON) == 0 &&
               TestFileEqualsText(external) && TestTmpAbsent());

    const char* first =
        "{\"env\":{\"A\":\"1\"},\"permissions\":{\"additionalDirectories\":[\"base\"]}}";
    const char* unrelated =
        "{\"env\":{\"A\":\"2\"},\"permissions\":{\"additionalDirectories\":[\"base\"]}}";
    prepared = TestPrepareText(first);
    LoadMembersFrom(test_anchor);
    strcpy(members[member_count], "local");
    member_count++;
    changed = TestWriteText(unrelated);
    bool saved = SaveMembersTo(test_anchor);
    size_t length = 0;
    bool read = TestReadBytes(&length);
    if (read) read_buffer[length] = '\0';
    TestReport("an unrelated settings change is preserved rather than conflicted",
               prepared && changed && saved && read &&
               strstr((char*)read_buffer, "\"A\":\"2\"") != NULL &&
               HasExactMembers("base", "local", NULL, 2));
}

static void TestIdempotentOperations(void) {
    const char* original =
        "{\"permissions\":{\"additionalDirectories\":[\"base\"]}}";
    bool prepared = TestPrepareText(original);
    enum MemberChangeResult add = ApplyMemberChange(
        test_anchor, "base", MEMBER_CHANGE_ADD);
    bool add_unchanged = prepared && add == MEMBER_CHANGE_NO_CHANGE &&
                         TestFileEqualsText(original);
    enum MemberChangeResult remove = ApplyMemberChange(
        test_anchor, "missing", MEMBER_CHANGE_REMOVE);
    TestReport("same-state add and remove operations are idempotent",
               add_unchanged && remove == MEMBER_CHANGE_NO_CHANGE &&
               TestFileEqualsText(original));
}

static void TestRefusedOperations(void) {
    char full[4096];
    size_t at = 0;
    at = AppendFmt(full, at, sizeof(full),
                   "{\"permissions\":{\"additionalDirectories\":[");
    for (int i = 0; i < MAX_MEMBERS; i++) {
        at = AppendFmt(full, at, sizeof(full), "%s\"m%03d\"",
                       i == 0 ? "" : ",", i);
    }
    at = AppendFmt(full, at, sizeof(full), "]}}");
    bool prepared = TestPrepareText(full);
    enum MemberChangeResult result = ApplyMemberChange(
        test_anchor, "overflow", MEMBER_CHANGE_ADD);
    TestReport("a full latest list is preserved and reported without publication",
               prepared && at == strlen(full) && result == MEMBER_CHANGE_FULL &&
               TestFileEqualsText(full) && TestTmpAbsent());

    const char* malformed = "{\"permissions\":{";
    prepared = TestPrepareText(malformed);
    result = ApplyMemberChange(test_anchor, "blocked", MEMBER_CHANGE_ADD);
    TestReport("unsafe settings are preserved by the locked operation path",
               prepared && result == MEMBER_CHANGE_SETTINGS_BLOCKED &&
               TestFileEqualsText(malformed) && TestTmpAbsent());

    const char* original =
        "{\"permissions\":{\"additionalDirectories\":[\"base\"]}}";
    prepared = TestPrepareText(original);
    SetFileAttributes(test_lock, FILE_ATTRIBUTE_NORMAL);
    DeleteFile(test_lock);
    bool lock_is_directory = CreateDirectory(test_lock, NULL) != 0;
    result = ApplyMemberChange(test_anchor, "must-not-save", MEMBER_CHANGE_ADD);
    bool unchanged = TestFileEqualsText(original) && TestTmpAbsent();
    if (lock_is_directory) RemoveDirectory(test_lock);
    TestReport("a lock-open failure is visible and leaves settings unchanged",
               prepared && lock_is_directory &&
               result == MEMBER_CHANGE_IO_FAILED && unchanged);
}

static void TestCrossProcessSerialization(void) {
    const char* original =
        "{\"permissions\":{\"additionalDirectories\":[\"base\"]}}";
    bool prepared = TestPrepareText(original);
    HANDLE lock;
    bool held = AcquireMemberLock(test_anchor, &lock) == MEMBER_LOCK_ACQUIRED;
    PROCESS_INFORMATION first;
    PROCESS_INFORMATION second;
    bool first_started = held && StartChild(
        "--child-add", "child-one", NULL, &first);
    bool second_started = first_started && StartChild(
        "--child-add", "child-two", NULL, &second);
    if (second_started) Sleep(80);
    if (held) CloseHandle(lock);

    DWORD first_exit = 0;
    DWORD second_exit = 0;
    bool first_done = first_started && FinishChild(&first, 5000, &first_exit);
    bool second_done = second_started && FinishChild(&second, 5000, &second_exit);
    TestReport("two real Drift processes serialize and preserve both additions",
               prepared && first_done && second_done &&
               first_exit == CHILD_RESULT_BASE + MEMBER_CHANGE_SAVED &&
               second_exit == CHILD_RESULT_BASE + MEMBER_CHANGE_SAVED &&
               HasExactMembers("base", "child-one", "child-two", 3));
}

static void TestBoundedContention(void) {
    const char* original =
        "{\"permissions\":{\"additionalDirectories\":[\"base\"]}}";
    bool prepared = TestPrepareText(original);
    HANDLE lock;
    bool held = AcquireMemberLock(test_anchor, &lock) == MEMBER_LOCK_ACQUIRED;
    PROCESS_INFORMATION child;
    bool started = held && StartChild("--child-add", "blocked", NULL, &child);
    DWORD exit_code = 0;
    bool finished = started && FinishChild(&child, 5000, &exit_code);
    bool unchanged = TestFileEqualsText(original) && TestTmpAbsent();
    if (held) CloseHandle(lock);
    enum MemberChangeResult after = ApplyMemberChange(
        test_anchor, "after-release", MEMBER_CHANGE_ADD);
    TestReport("lock contention times out visibly without touching settings",
               prepared && finished &&
               exit_code == CHILD_RESULT_BASE + MEMBER_CHANGE_BUSY &&
               unchanged && after == MEMBER_CHANGE_SAVED &&
               HasExactMembers("base", "after-release", NULL, 2));
}

static void TestCrashReleasesLock(void) {
    char event_name[128];
    snprintf(event_name, sizeof(event_name),
             "Local\\DriftMembershipTest-%lu-%lu",
             GetCurrentProcessId(), GetTickCount());
    HANDLE event = CreateEvent(NULL, TRUE, FALSE, event_name);
    PROCESS_INFORMATION child;
    memset(&child, 0, sizeof(child));
    bool started = event != NULL && StartChild(
        "--child-hold-lock", NULL, event_name, &child);
    bool acquired = started &&
        WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    bool requested = started && TerminateProcess(child.hProcess, 99) != 0;
    bool exited = requested &&
        WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0;
    bool terminated = acquired && exited;
    if (started) CloseHandle(child.hProcess);
    if (event != NULL) CloseHandle(event);

    HANDLE recovered_lock = INVALID_HANDLE_VALUE;
    bool recovered = terminated &&
        AcquireMemberLock(test_anchor, &recovered_lock) == MEMBER_LOCK_ACQUIRED;
    if (recovered) CloseHandle(recovered_lock);
    TestReport("process termination releases the persistent lock file",
               started && acquired && terminated && recovered);
}

static int CountSubstring(const char* text, const char* needle) {
    int count = 0;
    size_t length = strlen(needle);
    while ((text = strstr(text, needle)) != NULL) {
        count++;
        text += length;
    }
    return count;
}

static void TestProductionWiring(void) {
    FILE* file = fopen("src\\drift.c", "rb");
    bool opened = file != NULL;
    char* source = NULL;
    if (opened) {
        fseek(file, 0, SEEK_END);
        long length = ftell(file);
        rewind(file);
        if (length > 0) {
            source = (char*)malloc((size_t)length + 1);
            if (source != NULL) {
                size_t read = fread(source, 1, (size_t)length, file);
                source[read] = '\0';
                if (read != (size_t)length) {
                    free(source);
                    source = NULL;
                }
            }
        }
        fclose(file);
    }
    bool wired = source != NULL &&
        CountSubstring(source, "SaveMembersTo(") == 3 &&
        CountSubstring(source, "ApplyMemberChange(") == 5 &&
        CountSubstring(source, "AcquireMemberLock(") == 3 &&
        strstr(source, "CloseHandle(lock);") != NULL &&
        strstr(source, "\\.drift-members.lock") != NULL;
    TestReport("all production mutations route through one lock-owning transaction",
               wired);
    free(source);
}

static bool TestInitialize(void) {
    DWORD host_length = GetEnvironmentVariable(
        "DRIFT_HOST_DRIVE", prior_host_drive, sizeof(prior_host_drive));
    had_host_drive = host_length > 0 && host_length < sizeof(prior_host_drive);
    SetEnvironmentVariable("DRIFT_HOST_DRIVE", NULL);

    char temp[MAX_PATH];
    DWORD temp_length = GetTempPath(sizeof(temp), temp);
    if (temp_length == 0 || temp_length >= sizeof(temp)) return false;
    if (snprintf(test_root, sizeof(test_root),
                 "%sdrift-membership-test-%lu-%lu", temp,
                 GetCurrentProcessId(), GetTickCount()) >=
        (int)sizeof(test_root)) return false;
    if (snprintf(test_anchor, sizeof(test_anchor), "%s\\workspace", test_root) >=
        (int)sizeof(test_anchor)) return false;
    if (snprintf(test_claude_dir, sizeof(test_claude_dir), "%s\\.claude",
                 test_anchor) >= (int)sizeof(test_claude_dir)) return false;
    if (snprintf(test_settings, sizeof(test_settings), "%s\\settings.json",
                 test_claude_dir) >= (int)sizeof(test_settings)) return false;
    if (snprintf(test_tmp, sizeof(test_tmp), "%s.tmp", test_settings) >=
        (int)sizeof(test_tmp)) return false;
    if (snprintf(test_lock, sizeof(test_lock), "%s\\.drift-members.lock",
                 test_claude_dir) >= (int)sizeof(test_lock)) return false;
    return CreateDirectory(test_root, NULL) &&
           CreateDirectory(test_anchor, NULL) &&
           CreateDirectory(test_claude_dir, NULL);
}

static void TestCleanup(void) {
    ClearMemberSource();
    DeleteFile(test_tmp);
    DeleteFile(test_settings);
    SetFileAttributes(test_lock, FILE_ATTRIBUTE_NORMAL);
    DeleteFile(test_lock);
    RemoveDirectory(test_claude_dir);
    RemoveDirectory(test_anchor);
    RemoveDirectory(test_root);
    SetEnvironmentVariable("DRIFT_TEST_ANCHOR", NULL);
    SetEnvironmentVariable("DRIFT_TEST_MEMBER", NULL);
    SetEnvironmentVariable("DRIFT_TEST_EVENT", NULL);
    if (had_host_drive) {
        SetEnvironmentVariable("DRIFT_HOST_DRIVE", prior_host_drive);
    } else {
        SetEnvironmentVariable("DRIFT_HOST_DRIVE", NULL);
    }
}

int main(int argc, char** argv) {
    if (argc == 2 && strncmp(argv[1], "--child-", 8) == 0) {
        return ChildMain(argv[1]);
    }
    if (!TestInitialize()) {
        fprintf(stderr, "Could not initialize disposable membership workspace.\n");
        TestCleanup();
        return 2;
    }

    printf("DRIFT-005 membership concurrency tests\n");
    TestStaleOperationsRebase();
    TestTargetSourceConflict();
    TestIdempotentOperations();
    TestRefusedOperations();
    TestCrossProcessSerialization();
    TestBoundedContention();
    TestCrashReleasesLock();
    TestProductionWiring();
    TestCleanup();

    if (test_failures == 0) {
        printf("All membership concurrency tests passed.\n");
        return 0;
    }
    printf("%d membership concurrency test%s failed.\n", test_failures,
           test_failures == 1 ? "" : "s");
    return 1;
}
