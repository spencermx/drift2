// Windows production-linked regression coverage for DRIFT-006. The tests use
// the real membership load/save transaction and only touch a uniquely named
// disposable %TEMP% workspace.
#define _CRT_SECURE_NO_WARNINGS
#define main drift_application_main
#include "../src/drift.c"
#undef main

static int path_test_failures;
static char path_test_original_cwd[MAX_PATH];
static char path_test_root[MAX_PATH];
static char path_test_anchor[MAX_PATH];
static char path_test_claude_dir[MAX_PATH];
static char path_test_settings[MAX_PATH];
static char path_test_tmp[MAX_PATH];
static char path_test_lock[MAX_PATH];
static char path_test_shared[MAX_PATH];
static char path_test_process_cwd[MAX_PATH];
static char path_test_marker[MAX_PATH];
static char path_test_prior_host_drive[32768];
static bool path_test_had_host_drive;
static unsigned char path_test_read_buffer[70000];

static void PathTestReport(const char* name, bool passed) {
    printf("  %-78s %s\n", name, passed ? "PASS" : "FAIL");
    if (!passed) path_test_failures++;
}

static bool PathTestWriteText(const char* contents) {
    FILE* file = fopen(path_test_settings, "wb");
    if (file == NULL) return false;
    size_t length = strlen(contents);
    bool wrote = fwrite(contents, 1, length, file) == length;
    bool closed = fclose(file) == 0;
    return wrote && closed;
}

static bool PathTestReadBytes(size_t* length) {
    FILE* file = fopen(path_test_settings, "rb");
    if (file == NULL) return false;
    size_t count = fread(path_test_read_buffer, 1,
                         sizeof(path_test_read_buffer) - 1, file);
    bool complete = !ferror(file) && fgetc(file) == EOF;
    bool closed = fclose(file) == 0;
    path_test_read_buffer[count] = '\0';
    if (length != NULL) *length = count;
    return complete && closed;
}

static bool PathTestFileEquals(const char* expected) {
    size_t length = 0;
    return PathTestReadBytes(&length) && length == strlen(expected) &&
           memcmp(path_test_read_buffer, expected, length) == 0;
}

static bool PathTestPrepare(const char* contents) {
    DeleteFile(path_test_tmp);
    DeleteFile(path_test_settings);
    return PathTestWriteText(contents);
}

static bool PathTestTmpAbsent(void) {
    return GetFileAttributes(path_test_tmp) == INVALID_FILE_ATTRIBUTES;
}

static bool PathTestJsonEscape(const char* source, char* out, size_t capacity) {
    size_t used = 0;
    for (const char* p = source; *p != '\0'; p++) {
        if ((*p == '\\' || *p == '"') && used + 1 >= capacity) return false;
        if (*p == '\\' || *p == '"') out[used++] = '\\';
        if (used + 1 >= capacity) return false;
        out[used++] = *p;
    }
    out[used] = '\0';
    return true;
}

static bool PathTestCreateEmptyFile(const char* path) {
    FILE* file = fopen(path, "wb");
    return file != NULL && fclose(file) == 0;
}

static void TestResolverForms(void) {
    char resolved[MAX_PATH];
    char expected[MAX_PATH];

    snprintf(expected, sizeof(expected), "%s\\child", path_test_anchor);
    bool dot_child = ResolveMemberPath(
        path_test_anchor, ".\\child", resolved) &&
        _stricmp(resolved, expected) == 0;
    bool bare_child = ResolveMemberPath(
        path_test_anchor, "child", resolved) &&
        _stricmp(resolved, expected) == 0;
    bool forward_child = ResolveMemberPath(
        path_test_anchor, "./child/", resolved) &&
        _stricmp(resolved, expected) == 0;
    bool parent = ResolveMemberPath(
        path_test_anchor, "../shared/", resolved) &&
        _stricmp(resolved, path_test_shared) == 0;
    PathTestReport("relative spellings resolve lexically from the workspace anchor",
                   dot_child && bare_child && forward_child && parent);

    snprintf(expected, sizeof(expected), "%c:\\drift-rooted", path_test_anchor[0]);
    bool rooted = ResolveMemberPath(
        path_test_anchor, "\\drift-rooted", resolved) &&
        _stricmp(resolved, expected) == 0;
    bool absolute = ResolveMemberPath(
        path_test_anchor, path_test_shared, resolved) &&
        _stricmp(resolved, path_test_shared) == 0;
    bool unc = ResolveMemberPath(
        path_test_anchor, "\\\\server\\share\\folder\\..", resolved) &&
        _stricmp(resolved, "\\\\server\\share") == 0;
    PathTestReport("drive-rooted, drive-absolute, and UNC forms stay deterministic",
                   rooted && absolute && unc);

    bool drive_relative_refused =
        !ResolveMemberPath(path_test_anchor, "C:folder", resolved);
    bool unc_rooted_refused = !ResolveMemberPath(
        "\\\\server\\share\\anchor", "\\folder", resolved);
    PathTestReport("ambiguous drive-relative and UNC root-relative forms fail closed",
                   drive_relative_refused && unc_rooted_refused);
}

static void TestLoadAndFindIdentity(void) {
    const char* fixture =
        "{\"permissions\":{\"additionalDirectories\":[\"..\\\\shared\"]}}";
    bool prepared = PathTestPrepare(fixture);
    LoadMembersFrom(path_test_anchor);
    PathTestReport("loaded relative membership matches its browser absolute path",
                   prepared && json_block_reason == NULL && member_count == 1 &&
                   strcmp(members[0], "..\\shared") == 0 &&
                   FindMember(path_test_shared) == 0);
}

static void TestEquivalentAddIsBytePreserving(void) {
    const char* fixture =
        "{\"permissions\":{\"additionalDirectories\":[\"../shared/\"]},"
        "\"env\":{\"KEEP\":\"yes\"}}\n";
    bool prepared = PathTestPrepare(fixture);
    enum MemberChangeResult result = ApplyMemberChange(
        path_test_anchor, path_test_shared, MEMBER_CHANGE_ADD);
    PathTestReport("equivalent add is a no-op and leaves settings bytes untouched",
                   prepared && result == MEMBER_CHANGE_NO_CHANGE &&
                   PathTestFileEquals(fixture) && PathTestTmpAbsent());
}

static void TestRemovalRevokesAllEquivalentEntries(void) {
    char escaped_absolute[MAX_PATH * 2];
    char fixture[2048];
    bool escaped = PathTestJsonEscape(
        path_test_shared, escaped_absolute, sizeof(escaped_absolute));
    bool formatted = escaped && snprintf(
        fixture, sizeof(fixture),
        "{\"permissions\":{\"additionalDirectories\":["
        "\"..\\\\shared\",\"%s\",\"../shared/\",\".\\\\keep\"]},"
        "\"env\":{\"KEEP\":\"yes\"}}\n", escaped_absolute) <
        (int)sizeof(fixture);
    bool prepared = formatted && PathTestPrepare(fixture);
    enum MemberChangeResult result = ApplyMemberChange(
        path_test_anchor, path_test_shared, MEMBER_CHANGE_REMOVE);
    LoadMembersFrom(path_test_anchor);
    size_t length = 0;
    bool read = PathTestReadBytes(&length);
    PathTestReport("one removal revokes every relative/absolute duplicate",
                   prepared && result == MEMBER_CHANGE_SAVED &&
                   json_block_reason == NULL && member_count == 1 &&
                   strcmp(members[0], ".\\keep") == 0 &&
                   FindMember(path_test_shared) < 0 && read && length > 0 &&
                   strstr((char*)path_test_read_buffer, "\"KEEP\":\"yes\"") != NULL);
}

static void TestUnrelatedEditPreservesSpelling(void) {
    const char* fixture =
        "{\"permissions\":{\"additionalDirectories\":[\"../shared/\"]},"
        "\"env\":{\"KEEP\":\"yes\"}}";
    char other[MAX_PATH];
    snprintf(other, sizeof(other), "%s\\other", path_test_root);
    bool prepared = PathTestPrepare(fixture);
    enum MemberChangeResult added = ApplyMemberChange(
        path_test_anchor, other, MEMBER_CHANGE_ADD);
    enum MemberChangeResult removed = ApplyMemberChange(
        path_test_anchor, other, MEMBER_CHANGE_REMOVE);
    LoadMembersFrom(path_test_anchor);
    size_t length = 0;
    bool read = PathTestReadBytes(&length);
    PathTestReport("unrelated edits preserve configured relative spelling and JSON keys",
                   prepared && added == MEMBER_CHANGE_SAVED &&
                   removed == MEMBER_CHANGE_SAVED && json_block_reason == NULL &&
                   member_count == 1 && strcmp(members[0], "../shared/") == 0 &&
                   read && length > 0 &&
                   strstr((char*)path_test_read_buffer, "../shared/") != NULL &&
                   strstr((char*)path_test_read_buffer, "\"KEEP\":\"yes\"") != NULL);
}

static void TestManifestJumpUsesAnchor(void) {
    memset(members, 0, sizeof(members));
    strcpy(members[0], "..\\shared");
    member_count = 1;
    strcpy(edit_workspace, path_test_anchor);
    manifest_focused = true;
    bool changed_process_cwd = SetCurrentDirectory(path_test_process_cwd) != 0;
    bool jumped = changed_process_cwd && JumpToMemberAt(0);
    bool correct = jumped && _stricmp(current_directory, path_test_shared) == 0 &&
                   !manifest_focused;
    SetCurrentDirectory(path_test_original_cwd);
    PathTestReport("manifest jump is anchored instead of using the process CWD",
                   correct);
}

static void TestWineHostAndRelativeRoundTrip(void) {
    const char* fixture =
        "{\"permissions\":{\"additionalDirectories\":["
        "\"/Users/example/project\",\"../shared/\"]}}";
    bool env_set = SetEnvironmentVariable("DRIFT_HOST_DRIVE", "Z:") != 0;
    bool prepared = PathTestPrepare(fixture);
    LoadMembersFrom(path_test_anchor);
    bool loaded = json_block_reason == NULL && member_count == 2 &&
                  strcmp(members[0], "Z:\\Users\\example\\project") == 0 &&
                  strcmp(members[1], "../shared/") == 0;
    bool saved = loaded && SaveMembersTo(path_test_anchor);
    size_t length = 0;
    bool read = saved && PathTestReadBytes(&length);
    SetEnvironmentVariable("DRIFT_HOST_DRIVE", NULL);
    PathTestReport("Wine host-absolute and relative entries retain their storage forms",
                   env_set && prepared && loaded && saved && read && length > 0 &&
                   strstr((char*)path_test_read_buffer,
                          "\"/Users/example/project\"") != NULL &&
                   strstr((char*)path_test_read_buffer, "\"../shared/\"") != NULL);
}

static void TestOverlongResolutionFailsClosed(void) {
    char long_relative[241];
    memset(long_relative, 'a', sizeof(long_relative) - 1);
    long_relative[sizeof(long_relative) - 1] = '\0';
    char fixture[1024];
    bool formatted = snprintf(
        fixture, sizeof(fixture),
        "{\"permissions\":{\"additionalDirectories\":[\"%s\"]}}",
        long_relative) < (int)sizeof(fixture);
    bool prepared = formatted && PathTestPrepare(fixture);
    LoadMembersFrom(path_test_anchor);
    bool blocked_on_load = json_block_reason != NULL &&
        strcmp(json_block_reason, MEMBER_PATH_BLOCK_REASON) == 0;
    enum MemberChangeResult result = ApplyMemberChange(
        path_test_anchor, long_relative, MEMBER_CHANGE_REMOVE);
    PathTestReport("overlong resolved paths block edits without altering settings",
                   prepared && blocked_on_load &&
                   result == MEMBER_CHANGE_SETTINGS_BLOCKED &&
                   PathTestFileEquals(fixture) && PathTestTmpAbsent());
}

static void TestInvalidConfiguredPathFailsClosed(void) {
    const char* fixture =
        "{\"permissions\":{\"additionalDirectories\":[\"\"]}}";
    bool prepared = PathTestPrepare(fixture);
    LoadMembersFrom(path_test_anchor);
    bool blocked_on_load = json_block_reason != NULL &&
        strcmp(json_block_reason, MEMBER_PATH_BLOCK_REASON) == 0;
    enum MemberChangeResult result = ApplyMemberChange(
        path_test_anchor, path_test_shared, MEMBER_CHANGE_ADD);
    PathTestReport("an empty configured path blocks edits without being discarded",
                   prepared && blocked_on_load &&
                   result == MEMBER_CHANGE_SETTINGS_BLOCKED &&
                   PathTestFileEquals(fixture) && PathTestTmpAbsent());
}

static int PathTestCountSubstring(const char* text, const char* needle) {
    int count = 0;
    size_t length = strlen(needle);
    while ((text = strstr(text, needle)) != NULL) {
        count++;
        text += length;
    }
    return count;
}

static void TestPathProductionWiring(void) {
    FILE* file = fopen("src\\drift.c", "rb");
    char* source = NULL;
    if (file != NULL) {
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
        PathTestCountSubstring(source, "JumpToMemberAt(") == 3 &&
        strstr(source, "MemberPathsEqual(anchor, members[i], path)") != NULL &&
        strstr(source, "ResolveMemberPath(edit_workspace, members[index], jump)") != NULL;
    PathTestReport("production add/remove/jump paths use resolved membership identity",
                   wired);
    free(source);
}

static bool PathTestInitialize(void) {
    DWORD cwd_length = GetCurrentDirectory(
        sizeof(path_test_original_cwd), path_test_original_cwd);
    if (cwd_length == 0 || cwd_length >= sizeof(path_test_original_cwd)) return false;

    DWORD host_length = GetEnvironmentVariable(
        "DRIFT_HOST_DRIVE", path_test_prior_host_drive,
        sizeof(path_test_prior_host_drive));
    path_test_had_host_drive = host_length > 0 &&
                               host_length < sizeof(path_test_prior_host_drive);
    SetEnvironmentVariable("DRIFT_HOST_DRIVE", NULL);

    char temp[MAX_PATH];
    DWORD temp_length = GetTempPath(sizeof(temp), temp);
    if (temp_length == 0 || temp_length >= sizeof(temp)) return false;
    if (snprintf(path_test_root, sizeof(path_test_root),
                 "%sdrift-member-path-test-%lu-%lu", temp,
                 GetCurrentProcessId(), GetTickCount()) >=
        (int)sizeof(path_test_root)) return false;
    if (snprintf(path_test_anchor, sizeof(path_test_anchor), "%s\\anchor",
                 path_test_root) >= (int)sizeof(path_test_anchor)) return false;
    if (snprintf(path_test_claude_dir, sizeof(path_test_claude_dir), "%s\\.claude",
                 path_test_anchor) >= (int)sizeof(path_test_claude_dir)) return false;
    if (snprintf(path_test_settings, sizeof(path_test_settings), "%s\\settings.json",
                 path_test_claude_dir) >= (int)sizeof(path_test_settings)) return false;
    if (snprintf(path_test_tmp, sizeof(path_test_tmp), "%s.tmp",
                 path_test_settings) >= (int)sizeof(path_test_tmp)) return false;
    if (snprintf(path_test_lock, sizeof(path_test_lock), "%s\\.drift-members.lock",
                 path_test_claude_dir) >= (int)sizeof(path_test_lock)) return false;
    if (snprintf(path_test_shared, sizeof(path_test_shared), "%s\\shared",
                 path_test_root) >= (int)sizeof(path_test_shared)) return false;
    if (snprintf(path_test_process_cwd, sizeof(path_test_process_cwd), "%s\\process-cwd",
                 path_test_root) >= (int)sizeof(path_test_process_cwd)) return false;
    if (snprintf(path_test_marker, sizeof(path_test_marker), "%s\\intended.marker",
                 path_test_shared) >= (int)sizeof(path_test_marker)) return false;

    return CreateDirectory(path_test_root, NULL) &&
           CreateDirectory(path_test_anchor, NULL) &&
           CreateDirectory(path_test_claude_dir, NULL) &&
           CreateDirectory(path_test_shared, NULL) &&
           CreateDirectory(path_test_process_cwd, NULL) &&
           PathTestCreateEmptyFile(path_test_marker);
}

static void PathTestCleanup(void) {
    SetCurrentDirectory(path_test_original_cwd);
    ClearMemberSource();
    DeleteFile(path_test_tmp);
    DeleteFile(path_test_settings);
    SetFileAttributes(path_test_lock, FILE_ATTRIBUTE_NORMAL);
    DeleteFile(path_test_lock);
    DeleteFile(path_test_marker);
    RemoveDirectory(path_test_claude_dir);
    RemoveDirectory(path_test_anchor);
    RemoveDirectory(path_test_shared);
    RemoveDirectory(path_test_process_cwd);
    RemoveDirectory(path_test_root);
    if (path_test_had_host_drive) {
        SetEnvironmentVariable("DRIFT_HOST_DRIVE", path_test_prior_host_drive);
    } else {
        SetEnvironmentVariable("DRIFT_HOST_DRIVE", NULL);
    }
}

int main(void) {
    if (!PathTestInitialize()) {
        fprintf(stderr, "Could not initialize disposable membership path workspace.\n");
        PathTestCleanup();
        return 2;
    }

    printf("DRIFT-006 membership path tests\n");
    TestResolverForms();
    TestLoadAndFindIdentity();
    TestEquivalentAddIsBytePreserving();
    TestRemovalRevokesAllEquivalentEntries();
    TestUnrelatedEditPreservesSpelling();
    TestManifestJumpUsesAnchor();
    TestWineHostAndRelativeRoundTrip();
    TestOverlongResolutionFailsClosed();
    TestInvalidConfiguredPathFailsClosed();
    TestPathProductionWiring();
    PathTestCleanup();

    if (path_test_failures == 0) {
        printf("All membership path tests passed.\n");
        return 0;
    }
    printf("%d membership path test%s failed.\n", path_test_failures,
           path_test_failures == 1 ? "" : "s");
    return 1;
}
