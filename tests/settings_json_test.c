// Windows production-linked regression coverage for DRIFT-004. Include the
// application translation unit and exercise its real load/save transaction
// only beneath a uniquely named disposable %TEMP% workspace.
#define _CRT_SECURE_NO_WARNINGS
#define main drift_application_main
#include "../drift.c"
#undef main

static int test_failures;
static char test_root[MAX_PATH];
static char test_anchor[MAX_PATH];
static char test_claude_dir[MAX_PATH];
static char test_settings[MAX_PATH];
static char test_tmp[MAX_PATH];
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

static bool TestFileEquals(const void* expected, size_t length) {
    size_t actual_length = 0;
    return TestReadBytes(&actual_length) && actual_length == length &&
           memcmp(read_buffer, expected, length) == 0;
}

static bool TestFileEqualsText(const char* expected) {
    return TestFileEquals(expected, strlen(expected));
}

static bool TestPrepareFixture(const void* contents, size_t length) {
    DeleteFile(test_tmp);
    DeleteFile(test_settings);
    return TestWriteBytes(contents, length);
}

static bool TestPrepareText(const char* contents) {
    return TestPrepareFixture(contents, strlen(contents));
}

static bool TestTmpAbsent(void) {
    return GetFileAttributes(test_tmp) == INVALID_FILE_ATTRIBUTES;
}

static void SetSingleMember(const char* value) {
    memset(members, 0, sizeof(members));
    strcpy(members[0], value);
    member_count = 1;
}

static void TestPluginCollision(void) {
    const char* original =
        "{\n"
        "  \"pluginConfigs\": {\n"
        "    \"example@local\": {\n"
        "      \"options\": {\n"
        "        \"additionalDirectories\": [\"plugin-original\"]\n"
        "      }\n"
        "    }\n"
        "  },\n"
        "  \"permissions\": {\n"
        "    \"additionalDirectories\": [\"permission-original\"]\n"
        "  }\n"
        "}\n";
    bool prepared = TestPrepareText(original);
    LoadMembersFrom(test_anchor);
    bool loaded_permission = prepared && json_block_reason == NULL &&
                             member_count == 1 &&
                             strcmp(members[0], "permission-original") == 0;
    TestReport("supported plugin option never becomes workspace membership",
               loaded_permission);

    SetSingleMember("edited-by-drift");
    bool saved = SaveMembersTo(test_anchor);
    const char* expected =
        "{\n"
        "  \"pluginConfigs\": {\n"
        "    \"example@local\": {\n"
        "      \"options\": {\n"
        "        \"additionalDirectories\": [\"plugin-original\"]\n"
        "      }\n"
        "    }\n"
        "  },\n"
        "  \"permissions\": {\n"
        "    \"additionalDirectories\": [\n"
        "      \"edited-by-drift\"\n"
        "    ]\n"
        "  }\n"
        "}\n";
    bool correct_array = saved && TestFileEqualsText(expected);
    TestReport("save replaces only the root permissions array", correct_array);
}

static void TestPermissionsStringInsertion(void) {
    const char* original =
        "{\n"
        "  \"companyAnnouncements\": [\"permissions\"],\n"
        "  \"env\": {\"KEEP\": \"yes\"},\n"
        "  \"permissions\": {\"allow\": []}\n"
        "}\n";
    bool prepared = TestPrepareText(original);
    LoadMembersFrom(test_anchor);
    SetSingleMember("added-by-drift");
    bool saved = prepared && json_block_reason == NULL &&
                 SaveMembersTo(test_anchor);
    const char* expected =
        "{\n"
        "  \"companyAnnouncements\": [\"permissions\"],\n"
        "  \"env\": {\"KEEP\": \"yes\"},\n"
        "  \"permissions\": {\n"
        "    \"additionalDirectories\": [\n"
        "      \"added-by-drift\"\n"
        "    ],\"allow\": []}\n"
        "}\n";
    bool correct_object = saved && TestFileEqualsText(expected);
    TestReport("permissions text in a value cannot retarget insertion into env",
               correct_object);
}

static void TestMissingRootPermissions(void) {
    const char* original =
        "{\"env\":{\"permissions\":{\"additionalDirectories\":[\"nested\"]}},"
        "\"x\":1}";
    bool prepared = TestPrepareText(original);
    LoadMembersFrom(test_anchor);
    bool loaded_empty = prepared && json_block_reason == NULL && member_count == 0;
    SetSingleMember("root-member");
    bool saved = loaded_empty && SaveMembersTo(test_anchor);
    const char* expected =
        "{\n"
        "  \"permissions\": {\n"
        "    \"additionalDirectories\": [\n"
        "      \"root-member\"\n"
        "    ]\n"
        "  },\"env\":{\"permissions\":{"
        "\"additionalDirectories\":[\"nested\"]}},\"x\":1}";
    bool root_inserted = saved && TestFileEqualsText(expected);
    TestReport("nested target cannot replace a missing root permissions object",
               root_inserted);
}

static void TestEscapedTargetKeys(void) {
    const char* original =
        "{\"permiss\\u0069ons\":{"
        "\"additionalDirec\\u0074ories\":[\"encoded-original\"]}}";
    bool prepared = TestPrepareText(original);
    LoadMembersFrom(test_anchor);
    bool loaded = prepared && json_block_reason == NULL && member_count == 1 &&
                  strcmp(members[0], "encoded-original") == 0;
    SetSingleMember("encoded-edited");
    bool saved = loaded && SaveMembersTo(test_anchor);
    size_t length = 0;
    bool read = TestReadBytes(&length);
    if (read) read_buffer[length] = '\0';
    bool preserved_spelling = saved && read &&
        strstr((char*)read_buffer, "\"permiss\\u0069ons\"") != NULL &&
        strstr((char*)read_buffer, "\"additionalDirec\\u0074ories\"") != NULL &&
        strstr((char*)read_buffer, "encoded-edited") != NULL;
    TestReport("escaped semantic target keys load and retain their spelling",
               preserved_spelling);
}

static void TestBlockedFixture(const char* name, const void* original,
                               size_t length) {
    bool prepared = TestPrepareFixture(original, length);
    LoadMembersFrom(test_anchor);
    bool blocked = prepared && json_block_reason != NULL;
    SetSingleMember("must-not-publish");
    bool refused = !SaveMembersTo(test_anchor);
    TestReport(name, blocked && refused && TestFileEquals(original, length) &&
                     TestTmpAbsent());
}

static void TestUnsafeDocuments(void) {
    const char* wrong_parent = "{\"permissions\":[]}";
    TestBlockedFixture("wrong permissions type is preserved and refused",
                       wrong_parent, strlen(wrong_parent));

    const char* wrong_target =
        "{\"permissions\":{\"additionalDirectories\":null,\"allow\":[]}}";
    TestBlockedFixture("wrong target type cannot create a duplicate key",
                       wrong_target, strlen(wrong_target));

    const char* duplicate =
        "{\"permissions\":{\"additionalDirectories\":[],"
        "\"\\u0061dditionalDirectories\":[]}}";
    TestBlockedFixture("duplicate semantic target keys fail closed",
                       duplicate, strlen(duplicate));

    const char* duplicate_parent =
        "{\"permissions\":{},\"permiss\\u0069ons\":{}}";
    TestBlockedFixture("duplicate semantic permissions keys fail closed",
                       duplicate_parent, strlen(duplicate_parent));

    const char* non_string_member =
        "{\"permissions\":{\"additionalDirectories\":[\"ok\",7]}}";
    TestBlockedFixture("a non-string directory-array member fails closed",
                       non_string_member, strlen(non_string_member));

    const char* trailing = "{\"permissions\":{}} trailing";
    TestBlockedFixture("trailing garbage is preserved and refused",
                       trailing, strlen(trailing));

    const char* malformed = "{\"permissions\":{";
    TestBlockedFixture("malformed JSON is preserved and refused",
                       malformed, strlen(malformed));

    const char empty[] = "";
    TestBlockedFixture("an existing empty settings file is not overwritten",
                       empty, 0);

    const char embedded_nul[] =
        "{\"permissions\":{}}\0{\"after\":\"must-survive\"}";
    TestBlockedFixture("embedded NUL bytes cannot truncate the preserved suffix",
                       embedded_nul, sizeof(embedded_nul) - 1);

    char deep[512];
    size_t at = (size_t)snprintf(deep, sizeof(deep), "{\"x\":");
    for (int i = 0; i < DRIFT_SETTINGS_JSON_MAX_DEPTH + 1; i++) deep[at++] = '[';
    deep[at++] = '0';
    for (int i = 0; i < DRIFT_SETTINGS_JSON_MAX_DEPTH + 1; i++) deep[at++] = ']';
    deep[at++] = '}';
    TestBlockedFixture("over-deep JSON fails closed without exhausting the stack",
                       deep, at);
}

static void TestSaveRevalidatesFreshRead(void) {
    const char* loaded = "{\"permissions\":{}}";
    const char* changed = "{\"permissions\":[]}";
    bool prepared = TestPrepareText(loaded);
    LoadMembersFrom(test_anchor);
    bool load_was_editable = prepared && json_block_reason == NULL;
    bool changed_on_disk = TestWriteBytes(changed, strlen(changed));
    SetSingleMember("must-not-publish");
    bool refused = !SaveMembersTo(test_anchor);
    TestReport("save revalidates a structurally changed file after load",
               load_was_editable && changed_on_disk && refused &&
               TestFileEqualsText(changed) && TestTmpAbsent());
}

static void TestEmptyObjectInsertions(void) {
    bool prepared = TestPrepareText("{}");
    LoadMembersFrom(test_anchor);
    SetSingleMember("root-empty");
    bool saved = prepared && json_block_reason == NULL &&
                 SaveMembersTo(test_anchor);
    const char* root_expected =
        "{\n"
        "  \"permissions\": {\n"
        "    \"additionalDirectories\": [\n"
        "      \"root-empty\"\n"
        "    ]\n"
        "  }}";
    TestReport("an empty root object inserts without a stray comma",
               saved && TestFileEqualsText(root_expected));

    prepared = TestPrepareText("{\"permissions\":{}}");
    LoadMembersFrom(test_anchor);
    SetSingleMember("permissions-empty");
    saved = prepared && json_block_reason == NULL && SaveMembersTo(test_anchor);
    const char* permissions_expected =
        "{\"permissions\":{\n"
        "    \"additionalDirectories\": [\n"
        "      \"permissions-empty\"\n"
        "    ]}}";
    TestReport("an empty permissions object inserts without a stray comma",
               saved && TestFileEqualsText(permissions_expected));
}

static void TestAbsentFileCreation(void) {
    DeleteFile(test_tmp);
    DeleteFile(test_settings);
    json_block_reason = NULL;
    SetSingleMember("first-member");
    bool saved = SaveMembersTo(test_anchor);
    const char* expected =
        "{\n"
        "  \"permissions\": {\n"
        "    \"additionalDirectories\": [\n"
        "      \"first-member\"\n"
        "    ]\n"
        "  }\n"
        "}\n";
    TestReport("an absent file still receives the documented skeleton",
               saved && TestFileEqualsText(expected));
}

static bool TestInitialize(void) {
    DWORD host_length = GetEnvironmentVariable(
        "DRIFT_HOST_DRIVE", prior_host_drive, sizeof(prior_host_drive));
    had_host_drive = host_length > 0 && host_length < sizeof(prior_host_drive);
    SetEnvironmentVariable("DRIFT_HOST_DRIVE", NULL);

    char temp[MAX_PATH];
    DWORD temp_length = GetTempPath(sizeof(temp), temp);
    if (temp_length == 0 || temp_length >= sizeof(temp)) return false;
    if (snprintf(test_root, sizeof(test_root), "%sdrift-settings-test-%lu-%lu",
                 temp, GetCurrentProcessId(), GetTickCount()) >=
        (int)sizeof(test_root)) return false;
    if (snprintf(test_anchor, sizeof(test_anchor), "%s\\workspace", test_root) >=
        (int)sizeof(test_anchor)) return false;
    if (snprintf(test_claude_dir, sizeof(test_claude_dir), "%s\\.claude",
                 test_anchor) >= (int)sizeof(test_claude_dir)) return false;
    if (snprintf(test_settings, sizeof(test_settings), "%s\\settings.json",
                 test_claude_dir) >= (int)sizeof(test_settings)) return false;
    if (snprintf(test_tmp, sizeof(test_tmp), "%s.tmp", test_settings) >=
        (int)sizeof(test_tmp)) return false;
    return CreateDirectory(test_root, NULL) &&
           CreateDirectory(test_anchor, NULL) &&
           CreateDirectory(test_claude_dir, NULL);
}

static void TestCleanup(void) {
    DeleteFile(test_tmp);
    DeleteFile(test_settings);
    RemoveDirectory(test_claude_dir);
    RemoveDirectory(test_anchor);
    RemoveDirectory(test_root);
    if (had_host_drive) {
        SetEnvironmentVariable("DRIFT_HOST_DRIVE", prior_host_drive);
    } else {
        SetEnvironmentVariable("DRIFT_HOST_DRIVE", NULL);
    }
}

int main(void) {
    if (!TestInitialize()) {
        fprintf(stderr, "Could not initialize disposable settings test workspace.\n");
        TestCleanup();
        return 2;
    }

    printf("DRIFT-004 production settings JSON tests\n");
    TestPluginCollision();
    TestPermissionsStringInsertion();
    TestMissingRootPermissions();
    TestEscapedTargetKeys();
    TestUnsafeDocuments();
    TestSaveRevalidatesFreshRead();
    TestEmptyObjectInsertions();
    TestAbsentFileCreation();
    TestCleanup();

    if (test_failures == 0) {
        printf("All settings JSON tests passed.\n");
        return 0;
    }
    printf("%d settings JSON test%s failed.\n", test_failures,
           test_failures == 1 ? "" : "s");
    return 1;
}
