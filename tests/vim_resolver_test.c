// Windows-only regression coverage for DRIFT-002. Include the production
// translation unit so these tests exercise the resolver Drift actually ships.
#define main drift_application_main
#include "../src/drift.c"
#undef main

static int test_failures = 0;

static void TestReport(const char* name, bool passed) {
    printf("  %-68s %s\n", name, passed ? "PASS" : "FAIL");
    if (!passed) test_failures++;
}

static bool TestJoin(char* out, size_t out_size, const char* dir,
                     const char* leaf) {
    int written = snprintf(out, out_size, "%s\\%s", dir, leaf);
    return written >= 0 && written < (int)out_size;
}

static bool TestMakeDirectory(const char* path) {
    return CreateDirectory(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool TestTouch(const char* path) {
    HANDLE file = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

static bool TestResolvedPathIs(const char* resolved, const char* expected) {
    return _stricmp(resolved, expected) == 0;
}

static char* TestReadSource(void) {
    FILE* file = fopen("src/drift.c", "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char* source = (char*)malloc((size_t)length + 1);
    if (source == NULL) {
        fclose(file);
        return NULL;
    }
    size_t count = fread(source, 1, (size_t)length, file);
    bool read_ok = count == (size_t)length && !ferror(file);
    bool closed = fclose(file) == 0;
    if (!read_ok || !closed) {
        free(source);
        return NULL;
    }
    source[count] = '\0';
    return source;
}

int main(void) {
    char original_dir[MAX_PATH];
    char temp_dir[MAX_PATH];
    char root[MAX_PATH];
    DWORD original_len = GetCurrentDirectory(MAX_PATH, original_dir);
    DWORD temp_len = GetTempPath(MAX_PATH, temp_dir);
    if (original_len == 0 || original_len >= MAX_PATH ||
        temp_len == 0 || temp_len >= MAX_PATH) {
        fprintf(stderr, "Could not obtain test directories.\n");
        return 1;
    }
    char* source = TestReadSource();
    int root_len = snprintf(root, sizeof(root), "%sdrift-vim-%lu-%lu",
                            temp_dir, (unsigned long)GetCurrentProcessId(),
                            (unsigned long)GetTickCount());
    if (root_len < 0 || root_len >= (int)sizeof(root) ||
        !TestMakeDirectory(root)) {
        fprintf(stderr, "Could not create test root.\n");
        return 1;
    }

    char cwd[MAX_PATH], relative[MAX_PATH], early[MAX_PATH], later[MAX_PATH];
    char quoted[MAX_PATH], directory_only[MAX_PATH];
    bool paths_ok =
        TestJoin(cwd, sizeof(cwd), root, "cwd") &&
        TestJoin(relative, sizeof(relative), cwd, "relative") &&
        TestJoin(early, sizeof(early), root, "early") &&
        TestJoin(later, sizeof(later), root, "later") &&
        TestJoin(quoted, sizeof(quoted), root, "semi; & space") &&
        TestJoin(directory_only, sizeof(directory_only), root, "directory-only");
    if (!paths_ok || !TestMakeDirectory(cwd) || !TestMakeDirectory(relative) ||
        !TestMakeDirectory(early) || !TestMakeDirectory(later) ||
        !TestMakeDirectory(quoted) || !TestMakeDirectory(directory_only)) {
        fprintf(stderr, "Could not create test fixture directories.\n");
        return 1;
    }

    char cwd_vim[MAX_PATH], relative_vim[MAX_PATH], early_vim[MAX_PATH];
    char later_vim[MAX_PATH], quoted_vim[MAX_PATH], directory_vim[MAX_PATH];
    bool files_ok =
        TestJoin(cwd_vim, sizeof(cwd_vim), cwd, "vim.exe") &&
        TestJoin(relative_vim, sizeof(relative_vim), relative, "vim.exe") &&
        TestJoin(early_vim, sizeof(early_vim), early, "vim.exe") &&
        TestJoin(later_vim, sizeof(later_vim), later, "vim.exe") &&
        TestJoin(quoted_vim, sizeof(quoted_vim), quoted, "vim.exe") &&
        TestJoin(directory_vim, sizeof(directory_vim), directory_only, "vim.exe");
    if (!files_ok || !TestTouch(cwd_vim) || !TestTouch(relative_vim) ||
        !TestTouch(early_vim) || !TestTouch(later_vim) ||
        !TestTouch(quoted_vim) || !TestMakeDirectory(directory_vim) ||
        !SetCurrentDirectory(cwd)) {
        fprintf(stderr, "Could not create test fixture files.\n");
        return 1;
    }

    char resolved[MAX_PATH] = "stale";
    TestReport("a missing PATH never resolves a planted current-directory Vim",
               !ResolveVimFromPath(NULL, resolved) && resolved[0] == '\0');
    TestReport("an empty PATH never resolves a planted current-directory Vim",
               !ResolveVimFromPath("", resolved) && resolved[0] == '\0');
    TestReport("empty, relative, drive-relative, and root-relative entries are ignored",
               !ResolveVimFromPath(";.;relative;C:relative;\\relative;;",
                                   resolved));

    char path_value[MAX_PATH * 3];
    snprintf(path_value, sizeof(path_value), "%s;%s", early, later);
    TestReport("the first matching absolute PATH directory wins",
               ResolveVimFromPath(path_value, resolved) &&
               TestResolvedPathIs(resolved, early_vim));

    snprintf(path_value, sizeof(path_value), "%s;%s", later, early);
    TestReport("reversing absolute PATH directory order reverses the match",
               ResolveVimFromPath(path_value, resolved) &&
               TestResolvedPathIs(resolved, later_vim));

    snprintf(path_value, sizeof(path_value), "\"%s\"", quoted);
    TestReport("a quoted absolute entry containing spaces and a semicolon resolves",
               ResolveVimFromPath(path_value, resolved) &&
               TestResolvedPathIs(resolved, quoted_vim));

    TestReport("a directory named vim.exe is not accepted as an executable",
               !ResolveVimFromPath(directory_only, resolved));
    TestReport("the current directory is trusted only when explicitly absolute",
               ResolveVimFromPath(cwd, resolved) &&
               TestResolvedPathIs(resolved, cwd_vim));

    snprintf(path_value, sizeof(path_value), ";relative;%s", later);
    TestReport("invalid entries do not prevent a later absolute match",
               ResolveVimFromPath(path_value, resolved) &&
               TestResolvedPathIs(resolved, later_vim));

    DWORD original_path_required = GetEnvironmentVariable("PATH", NULL, 0);
    char* original_path = original_path_required > 0
        ? (char*)malloc(original_path_required) : NULL;
    bool environment_ok = original_path != NULL &&
        GetEnvironmentVariable("PATH", original_path, original_path_required) > 0 &&
        SetEnvironmentVariable("PATH", later) && ResolveVim(resolved) &&
        TestResolvedPathIs(resolved, later_vim);
    if (original_path != NULL) {
        environment_ok = SetEnvironmentVariable("PATH", original_path) &&
            environment_ok;
        free(original_path);
    }
    TestReport("the environment wrapper uses the same absolute-only resolver",
               environment_ok);

    TestReport("production contains no SearchPath or SetSearchPathMode calls",
               source != NULL && strstr(source, "SearchPath(") == NULL &&
               strstr(source, "SetSearchPathMode(") == NULL);
    TestReport("the editor passes the resolved Vim path directly to CreateProcess",
               source != NULL && strstr(source, "ResolveVim(vim_exe)") != NULL &&
               strstr(source, "CreateProcess(vim_exe, command") != NULL);
    TestReport("a missing safe Vim still uses the default-application fallback",
               source != NULL &&
               strstr(source, "ShellExecute(NULL, \"open\", file_path") != NULL);
    free(source);

    SetCurrentDirectory(original_dir);
    DeleteFile(cwd_vim);
    DeleteFile(relative_vim);
    DeleteFile(early_vim);
    DeleteFile(later_vim);
    DeleteFile(quoted_vim);
    RemoveDirectory(directory_vim);
    RemoveDirectory(relative);
    RemoveDirectory(cwd);
    RemoveDirectory(early);
    RemoveDirectory(later);
    RemoveDirectory(quoted);
    RemoveDirectory(directory_only);
    RemoveDirectory(root);

    if (test_failures == 0) {
        printf("All Vim resolver tests passed.\n");
        return 0;
    }
    printf("%d Vim resolver test(s) failed.\n", test_failures);
    return 1;
}
