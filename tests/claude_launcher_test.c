// Windows-only regression coverage for DRIFT-001. Include the production
// translation unit so these tests exercise the resolver and process-spec
// builder that Drift actually ships rather than a copied implementation.
#define main drift_application_main
#include "../drift.c"
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

static bool TestWrite(const char* path, const char* contents) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) return false;
    bool wrote = fputs(contents, file) >= 0;
    bool closed = fclose(file) == 0;
    return wrote && closed;
}

static bool TestRead(const char* path, char* out, size_t out_size) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    size_t count = fread(out, 1, out_size - 1, file);
    bool read_ok = !ferror(file);
    bool closed = fclose(file) == 0;
    out[count] = '\0';
    return read_ok && closed;
}

static bool TestRunProcess(ClaudeProcessSpec* process, const char* cwd) {
    STARTUPINFO startup = {0};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child;
    char command[CLAUDE_COMMAND_CAP];
    strcpy(command, process->command); // CreateProcess may modify this buffer
    if (!CreateProcess(process->application, command, NULL, NULL, FALSE, 0,
                       NULL, cwd, &startup, &child)) {
        return false;
    }
    DWORD wait = WaitForSingleObject(child.hProcess, 10000);
    DWORD exit_code = 1;
    if (wait == WAIT_TIMEOUT) TerminateProcess(child.hProcess, 1);
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &exit_code);
    CloseHandle(child.hProcess);
    CloseHandle(child.hThread);
    return wait == WAIT_OBJECT_0 && exit_code == 0;
}

static bool LauncherIs(const ClaudeLauncher* launcher, const char* path,
                       enum ClaudeLauncherKind kind) {
    return launcher->kind == kind && _stricmp(launcher->path, path) == 0;
}

int main(int argc, char* argv[]) {
    char child_marker[MAX_PATH];
    DWORD marker_len = GetEnvironmentVariable("DRIFT_LAUNCH_TEST_MARKER",
                                               child_marker, MAX_PATH);
    if (marker_len > 0 && marker_len < MAX_PATH) {
        char child_cwd[MAX_PATH];
        DWORD cwd_len = GetCurrentDirectory(MAX_PATH, child_cwd);
        FILE* marker = fopen(child_marker, "wb");
        if (marker == NULL || cwd_len == 0 || cwd_len >= MAX_PATH) return 2;
        fprintf(marker, "%s\n", child_cwd);
        for (int i = 1; i < argc; i++) fprintf(marker, "%s\n", argv[i]);
        return fclose(marker) == 0 ? 0 : 2;
    }

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
    int root_len = snprintf(root, sizeof(root), "%sdrift-launch-%lu-%lu",
                            temp_dir, (unsigned long)GetCurrentProcessId(),
                            (unsigned long)GetTickCount());
    if (root_len < 0 || root_len >= (int)sizeof(root) ||
        !TestMakeDirectory(root)) {
        fprintf(stderr, "Could not create test root.\n");
        return 1;
    }

    char cwd[MAX_PATH], relative[MAX_PATH], early[MAX_PATH], later[MAX_PATH];
    char spaces[MAX_PATH], semicolon[MAX_PATH], both[MAX_PATH];
    bool paths_ok =
        TestJoin(cwd, sizeof(cwd), root, "cwd") &&
        TestJoin(relative, sizeof(relative), cwd, "relative") &&
        TestJoin(early, sizeof(early), root, "early") &&
        TestJoin(later, sizeof(later), root, "later") &&
        TestJoin(spaces, sizeof(spaces), root, "space dir") &&
        TestJoin(semicolon, sizeof(semicolon), root, "semi; & space") &&
        TestJoin(both, sizeof(both), root, "both");
    if (!paths_ok || !TestMakeDirectory(cwd) || !TestMakeDirectory(relative) ||
        !TestMakeDirectory(early) || !TestMakeDirectory(later) ||
        !TestMakeDirectory(spaces) || !TestMakeDirectory(semicolon) ||
        !TestMakeDirectory(both)) {
        fprintf(stderr, "Could not create test fixture directories.\n");
        return 1;
    }

    char cwd_exe[MAX_PATH], cwd_cmd[MAX_PATH], relative_exe[MAX_PATH];
    char early_cmd[MAX_PATH], later_exe[MAX_PATH], spaces_exe[MAX_PATH];
    char semicolon_cmd[MAX_PATH], both_exe[MAX_PATH], both_cmd[MAX_PATH];
    bool files_ok =
        TestJoin(cwd_exe, sizeof(cwd_exe), cwd, "claude.exe") &&
        TestJoin(cwd_cmd, sizeof(cwd_cmd), cwd, "cmd.exe") &&
        TestJoin(relative_exe, sizeof(relative_exe), relative, "claude.exe") &&
        TestJoin(early_cmd, sizeof(early_cmd), early, "claude.cmd") &&
        TestJoin(later_exe, sizeof(later_exe), later, "claude.exe") &&
        TestJoin(spaces_exe, sizeof(spaces_exe), spaces, "claude.exe") &&
        TestJoin(semicolon_cmd, sizeof(semicolon_cmd), semicolon, "claude.cmd") &&
        TestJoin(both_exe, sizeof(both_exe), both, "claude.exe") &&
        TestJoin(both_cmd, sizeof(both_cmd), both, "claude.cmd");
    if (!files_ok || !TestTouch(cwd_exe) || !TestTouch(cwd_cmd) ||
        !TestTouch(relative_exe) || !TestTouch(early_cmd) ||
        !TestTouch(later_exe) || !TestTouch(spaces_exe) ||
        !TestTouch(semicolon_cmd) || !TestTouch(both_exe) ||
        !TestTouch(both_cmd) || !SetCurrentDirectory(cwd)) {
        fprintf(stderr, "Could not create test fixture files.\n");
        return 1;
    }

    ClaudeLauncher launcher;
    TestReport("a missing PATH never resolves the current directory",
               !ResolveClaudeLauncherFromPath(NULL, &launcher) &&
               launcher.kind == CLAUDE_LAUNCHER_NONE);
    TestReport("an empty PATH never resolves the current directory",
               !ResolveClaudeLauncherFromPath("", &launcher));
    TestReport("empty and relative PATH entries are ignored",
               !ResolveClaudeLauncherFromPath(";.;relative;;", &launcher));

    char path_value[MAX_PATH * 3];
    snprintf(path_value, sizeof(path_value), "%s;%s", early, later);
    TestReport("an earlier .cmd directory wins over a later .exe directory",
               ResolveClaudeLauncherFromPath(path_value, &launcher) &&
               LauncherIs(&launcher, early_cmd, CLAUDE_LAUNCHER_CMD));

    snprintf(path_value, sizeof(path_value), "%s;%s", later, early);
    TestReport("absolute PATH directory order is preserved",
               ResolveClaudeLauncherFromPath(path_value, &launcher) &&
               LauncherIs(&launcher, later_exe, CLAUDE_LAUNCHER_EXE));

    snprintf(path_value, sizeof(path_value), "\"%s\"", spaces);
    TestReport("a quoted absolute PATH entry containing spaces resolves",
               ResolveClaudeLauncherFromPath(path_value, &launcher) &&
               LauncherIs(&launcher, spaces_exe, CLAUDE_LAUNCHER_EXE));

    snprintf(path_value, sizeof(path_value), "\"%s\"", semicolon);
    TestReport("a quoted absolute PATH entry containing a semicolon resolves",
               ResolveClaudeLauncherFromPath(path_value, &launcher) &&
               LauncherIs(&launcher, semicolon_cmd, CLAUDE_LAUNCHER_CMD));

    TestReport(".exe wins over .cmd within the same absolute directory",
               ResolveClaudeLauncherFromPath(both, &launcher) &&
               LauncherIs(&launcher, both_exe, CLAUDE_LAUNCHER_EXE));
    TestReport("the current directory is used only when explicitly on PATH",
               ResolveClaudeLauncherFromPath(cwd, &launcher) &&
               LauncherIs(&launcher, cwd_exe, CLAUDE_LAUNCHER_EXE));

    snprintf(path_value, sizeof(path_value), ";relative;%s", later);
    TestReport("invalid entries do not prevent a later absolute match",
               ResolveClaudeLauncherFromPath(path_value, &launcher) &&
               LauncherIs(&launcher, later_exe, CLAUDE_LAUNCHER_EXE));

    ClaudeProcessSpec process;
    launcher.kind = CLAUDE_LAUNCHER_EXE;
    strcpy(launcher.path, spaces_exe);
    bool built = BuildClaudeProcessSpec(&launcher, "dead-beef", &process);
    TestReport("a native Claude executable is launched directly",
               built && _stricmp(process.application, spaces_exe) == 0 &&
               strstr(process.command, "--resume \"dead-beef\"") != NULL);

    launcher.kind = CLAUDE_LAUNCHER_CMD;
    strcpy(launcher.path, early_cmd);
    built = BuildClaudeProcessSpec(&launcher, NULL, &process);
    char system_dir[MAX_PATH], system_cmd[MAX_PATH];
    DWORD system_len = GetSystemDirectory(system_dir, MAX_PATH);
    bool system_path_ok = system_len > 0 && system_len < MAX_PATH &&
        snprintf(system_cmd, sizeof(system_cmd), "%s\\cmd.exe", system_dir) > 0;
    TestReport("a .cmd shim uses the absolute System32 command processor",
               built && system_path_ok &&
               _stricmp(process.application, system_cmd) == 0 &&
               _stricmp(process.application, cwd_cmd) != 0);
    TestReport("the trusted command processor disables mutable shell features",
               built && strstr(process.command, " /d /v:off /s /c ") != NULL &&
               strstr(process.command, early_cmd) != NULL);

    char self[MAX_PATH], exe_marker[MAX_PATH], cmd_marker[MAX_PATH];
    char marker_contents[MAX_PATH * 2];
    DWORD self_len = GetModuleFileName(NULL, self, MAX_PATH);
    bool integration_paths_ok = self_len > 0 && self_len < MAX_PATH &&
        TestJoin(exe_marker, sizeof(exe_marker), root, "exe-result.txt") &&
        TestJoin(cmd_marker, sizeof(cmd_marker), root, "cmd-result.txt");

    bool exe_ran = integration_paths_ok && CopyFile(self, spaces_exe, FALSE);
    launcher.kind = CLAUDE_LAUNCHER_EXE;
    strcpy(launcher.path, spaces_exe);
    exe_ran = exe_ran && BuildClaudeProcessSpec(&launcher, "dead-beef", &process) &&
        SetEnvironmentVariable("DRIFT_LAUNCH_TEST_MARKER", exe_marker) &&
        TestRunProcess(&process, cwd);
    SetEnvironmentVariable("DRIFT_LAUNCH_TEST_MARKER", NULL);
    exe_ran = exe_ran && TestRead(exe_marker, marker_contents,
                                  sizeof(marker_contents));
    TestReport("the resolved .exe receives the workspace cwd and resume id",
               exe_ran && strstr(marker_contents, cwd) != NULL &&
               strstr(marker_contents, "--resume\ndead-beef\n") != NULL);

    const char* batch =
        "@echo off\r\n"
        "> \"%DRIFT_LAUNCH_TEST_MARKER%\" echo %CD%\r\n"
        ">> \"%DRIFT_LAUNCH_TEST_MARKER%\" echo %1\r\n"
        ">> \"%DRIFT_LAUNCH_TEST_MARKER%\" echo %2\r\n";
    bool cmd_ran = integration_paths_ok && TestWrite(semicolon_cmd, batch);
    launcher.kind = CLAUDE_LAUNCHER_CMD;
    strcpy(launcher.path, semicolon_cmd);
    cmd_ran = cmd_ran && BuildClaudeProcessSpec(&launcher, "dead-beef", &process) &&
        SetEnvironmentVariable("DRIFT_LAUNCH_TEST_MARKER", cmd_marker) &&
        TestRunProcess(&process, cwd);
    SetEnvironmentVariable("DRIFT_LAUNCH_TEST_MARKER", NULL);
    cmd_ran = cmd_ran && TestRead(cmd_marker, marker_contents,
                                  sizeof(marker_contents));
    if (cmd_ran && (strstr(marker_contents, cwd) == NULL ||
                    strstr(marker_contents, "--resume") == NULL ||
                    strstr(marker_contents, "dead-beef") == NULL)) {
        printf("    observed .cmd marker: [%s]\n", marker_contents);
    }
    TestReport("the resolved .cmd receives the workspace cwd and resume id",
               cmd_ran && strstr(marker_contents, cwd) != NULL &&
               strstr(marker_contents, "--resume") != NULL &&
               strstr(marker_contents, "dead-beef") != NULL);

    TestReport("an unsafe session id is rejected before process creation",
               !BuildClaudeProcessSpec(&launcher, "bad&id", &process));
    strcpy(launcher.path, "C:\\unsafe%PATH%\\claude.cmd");
    TestReport("a .cmd path subject to percent expansion is rejected",
               !BuildClaudeProcessSpec(&launcher, NULL, &process));

    SetCurrentDirectory(original_dir);
    DeleteFile(cwd_exe);
    DeleteFile(cwd_cmd);
    DeleteFile(relative_exe);
    DeleteFile(early_cmd);
    DeleteFile(later_exe);
    DeleteFile(spaces_exe);
    DeleteFile(semicolon_cmd);
    DeleteFile(both_exe);
    DeleteFile(both_cmd);
    DeleteFile(exe_marker);
    DeleteFile(cmd_marker);
    RemoveDirectory(relative);
    RemoveDirectory(cwd);
    RemoveDirectory(early);
    RemoveDirectory(later);
    RemoveDirectory(spaces);
    RemoveDirectory(semicolon);
    RemoveDirectory(both);
    RemoveDirectory(root);

    if (test_failures == 0) {
        printf("All Claude launcher tests passed.\n");
        return 0;
    }
    printf("%d Claude launcher test(s) failed.\n", test_failures);
    return 1;
}
