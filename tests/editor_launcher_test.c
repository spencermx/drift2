// Windows-only regression coverage for the 'v' open-in-editor verb. Include the
// production translation unit so these tests exercise the resolvers, the shared
// process-spec builder, and the solution scan that Drift actually ships.
//
// The most important case here is not a new behavior at all: BuildClaudeProcessSpec
// became a wrapper over BuildLauncherProcessSpec, and DRIFT-001's contract is that
// the command lines it produces did not change. Those are asserted byte for byte.
#define main drift_application_main
#include "../src/drift.c"
#undef main

static int test_failures = 0;

static void TestReport(const char* name, bool passed) {
    printf("  %-70s %s\n", name, passed ? "PASS" : "FAIL");
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

static bool LauncherIs(const ClaudeLauncher* launcher, const char* path,
                       enum ClaudeLauncherKind kind) {
    return launcher->kind == kind && _stricmp(launcher->path, path) == 0;
}

static bool SolutionsContain(char solutions[][MAX_PATH], int count,
                             const char* leaf) {
    for (int i = 0; i < count; i++) {
        if (_stricmp(LeafName(solutions[i]), leaf) == 0) return true;
    }
    return false;
}

int main(int argc, char* argv[]) {
    // Detached-spawn child: record that we ran, then exit.
    char child_marker[MAX_PATH];
    DWORD marker_len = GetEnvironmentVariable("DRIFT_EDITOR_TEST_MARKER",
                                              child_marker, MAX_PATH);
    if (marker_len > 0 && marker_len < MAX_PATH) {
        FILE* marker = fopen(child_marker, "wb");
        if (marker == NULL) return 2;
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
    int root_len = snprintf(root, sizeof(root), "%sdrift-editor-%lu-%lu",
                            temp_dir, (unsigned long)GetCurrentProcessId(),
                            (unsigned long)GetTickCount());
    if (root_len < 0 || root_len >= (int)sizeof(root) ||
        !TestMakeDirectory(root)) {
        fprintf(stderr, "Could not create test root.\n");
        return 1;
    }

    char cwd[MAX_PATH], editors[MAX_PATH], vs[MAX_PATH];
    char none[MAX_PATH], one[MAX_PATH], many[MAX_PATH];
    bool paths_ok =
        TestJoin(cwd, sizeof(cwd), root, "cwd") &&
        TestJoin(editors, sizeof(editors), root, "editors") &&
        TestJoin(vs, sizeof(vs), root, "vs") &&
        TestJoin(none, sizeof(none), root, "no-solution") &&
        TestJoin(one, sizeof(one), root, "one-solution") &&
        TestJoin(many, sizeof(many), root, "many-solutions");
    if (!paths_ok || !TestMakeDirectory(cwd) || !TestMakeDirectory(editors) ||
        !TestMakeDirectory(vs) || !TestMakeDirectory(none) ||
        !TestMakeDirectory(one) || !TestMakeDirectory(many)) {
        fprintf(stderr, "Could not create test fixture directories.\n");
        return 1;
    }

    // ---------------------------------------------------------------------
    // Resolvers: the absolute-PATH-entry rule from DRIFT-001/002 applies to
    // every launcher, not just the two it was written for
    // ---------------------------------------------------------------------
    char code_cmd[MAX_PATH], code_exe[MAX_PATH], devenv_exe[MAX_PATH];
    char cwd_code[MAX_PATH], cwd_devenv[MAX_PATH];
    bool files_ok =
        TestJoin(code_cmd, sizeof(code_cmd), editors, "code.cmd") &&
        TestJoin(code_exe, sizeof(code_exe), editors, "code.exe") &&
        TestJoin(devenv_exe, sizeof(devenv_exe), editors, "devenv.exe") &&
        TestJoin(cwd_code, sizeof(cwd_code), cwd, "code.cmd") &&
        TestJoin(cwd_devenv, sizeof(cwd_devenv), cwd, "devenv.exe");
    if (!files_ok || !TestTouch(code_cmd) || !TestTouch(code_exe) ||
        !TestTouch(devenv_exe) || !TestTouch(cwd_code) ||
        !TestTouch(cwd_devenv) || !SetCurrentDirectory(cwd)) {
        fprintf(stderr, "Could not create test fixture files.\n");
        return 1;
    }

    ClaudeLauncher launcher;
    TestReport("VS Code is never resolved from a missing PATH",
               !ResolveVsCodeFromPath(NULL, &launcher) &&
               launcher.kind == CLAUDE_LAUNCHER_NONE);
    TestReport("VS Code ignores empty, relative and drive-relative entries",
               !ResolveVsCodeFromPath(";.;editors;C:editors;\\editors;", &launcher));
    TestReport("the VS Code shim resolves as a cmd launcher",
               ResolveVsCodeFromPath(editors, &launcher) &&
               LauncherIs(&launcher, code_cmd, CLAUDE_LAUNCHER_CMD));

    TestReport("devenv is never resolved from a missing PATH",
               !ResolveDevenvFromPath(NULL, &launcher) &&
               launcher.kind == CLAUDE_LAUNCHER_NONE);
    TestReport("devenv ignores empty and relative entries",
               !ResolveDevenvFromPath(";.;editors;;", &launcher));
    TestReport("devenv resolves as a native executable",
               ResolveDevenvFromPath(editors, &launcher) &&
               LauncherIs(&launcher, devenv_exe, CLAUDE_LAUNCHER_EXE));
    TestReport("the current directory is used only when explicitly on PATH",
               ResolveDevenvFromPath(cwd, &launcher) &&
               LauncherIs(&launcher, cwd_devenv, CLAUDE_LAUNCHER_EXE));

    // ---------------------------------------------------------------------
    // The version selector comes from a well-known directory, never PATH
    // ---------------------------------------------------------------------
    char msenv[MAX_PATH], shared[MAX_PATH], vslauncher[MAX_PATH];
    bool vs_ok =
        TestJoin(shared, sizeof(shared), vs, "Microsoft Shared") &&
        TestMakeDirectory(shared) &&
        TestJoin(msenv, sizeof(msenv), shared, "MSEnv") &&
        TestMakeDirectory(msenv) &&
        TestJoin(vslauncher, sizeof(vslauncher), msenv, "VSLauncher.exe") &&
        TestTouch(vslauncher);
    if (!vs_ok) {
        fprintf(stderr, "Could not create version-selector fixture.\n");
        return 1;
    }

    char saved_common[MAX_PATH];
    DWORD saved_len = GetEnvironmentVariable("CommonProgramFiles(x86)",
                                             saved_common, MAX_PATH);
    bool had_common = saved_len > 0 && saved_len < MAX_PATH;
    SetEnvironmentVariable("CommonProgramFiles(x86)", vs);
    TestReport("the version selector resolves from its shared-component path",
               ResolveVisualStudioLauncher(&launcher) &&
               LauncherIs(&launcher, vslauncher, CLAUDE_LAUNCHER_EXE));

    // A directory of that name must not be accepted as the launcher
    char decoy[MAX_PATH], decoy_msenv[MAX_PATH], decoy_shared[MAX_PATH];
    char decoy_launcher[MAX_PATH];
    bool decoy_ok =
        TestJoin(decoy, sizeof(decoy), root, "decoy") && TestMakeDirectory(decoy) &&
        TestJoin(decoy_shared, sizeof(decoy_shared), decoy, "Microsoft Shared") &&
        TestMakeDirectory(decoy_shared) &&
        TestJoin(decoy_msenv, sizeof(decoy_msenv), decoy_shared, "MSEnv") &&
        TestMakeDirectory(decoy_msenv) &&
        TestJoin(decoy_launcher, sizeof(decoy_launcher), decoy_msenv,
                 "VSLauncher.exe") &&
        TestMakeDirectory(decoy_launcher);
    SetEnvironmentVariable("CommonProgramFiles(x86)", decoy);
    TestReport("a directory named VSLauncher.exe is not accepted",
               decoy_ok && !ResolveVisualStudioLauncher(&launcher) &&
               launcher.kind == CLAUDE_LAUNCHER_NONE);

    SetEnvironmentVariable("CommonProgramFiles(x86)",
                           had_common ? saved_common : NULL);

    // ---------------------------------------------------------------------
    // DRIFT-001 regression guard: the delegation must not have changed a byte
    // ---------------------------------------------------------------------
    char system_dir[MAX_PATH], system_cmd[MAX_PATH];
    DWORD system_len = GetSystemDirectory(system_dir, MAX_PATH);
    bool system_ok = system_len > 0 && system_len < MAX_PATH &&
        snprintf(system_cmd, sizeof(system_cmd), "%s\\cmd.exe", system_dir) > 0;

    ClaudeProcessSpec process;
    char expected[CLAUDE_COMMAND_CAP];

    launcher.kind = CLAUDE_LAUNCHER_EXE;
    strcpy(launcher.path, devenv_exe);
    snprintf(expected, sizeof(expected), "\"%s\" --resume \"dead-beef\"",
             devenv_exe);
    TestReport("exe + session id builds the pre-refactor command line",
               BuildClaudeProcessSpec(&launcher, "dead-beef", &process) &&
               strcmp(process.command, expected) == 0 &&
               _stricmp(process.application, devenv_exe) == 0);

    snprintf(expected, sizeof(expected), "\"%s\"", devenv_exe);
    TestReport("exe without a session id builds the pre-refactor command line",
               BuildClaudeProcessSpec(&launcher, NULL, &process) &&
               strcmp(process.command, expected) == 0);

    launcher.kind = CLAUDE_LAUNCHER_CMD;
    strcpy(launcher.path, code_cmd);
    snprintf(expected, sizeof(expected),
             "\"%s\" /d /v:off /s /c \"\"%s\" --resume \"dead-beef\"\"",
             system_cmd, code_cmd);
    TestReport("cmd + session id builds the pre-refactor command line",
               system_ok &&
               BuildClaudeProcessSpec(&launcher, "dead-beef", &process) &&
               strcmp(process.command, expected) == 0 &&
               _stricmp(process.application, system_cmd) == 0);

    snprintf(expected, sizeof(expected), "\"%s\" /d /v:off /s /c \"\"%s\"\"",
             system_cmd, code_cmd);
    TestReport("cmd without a session id builds the pre-refactor command line",
               system_ok && BuildClaudeProcessSpec(&launcher, NULL, &process) &&
               strcmp(process.command, expected) == 0);

    TestReport("an unsafe session id is still rejected through the wrapper",
               !BuildClaudeProcessSpec(&launcher, "bad&id", &process));

    // ---------------------------------------------------------------------
    // Argument safety on the shared builder
    // ---------------------------------------------------------------------
    TestReport("a cmd launcher path subject to percent expansion is rejected",
               (strcpy(launcher.path, "C:\\unsafe%PATH%\\code.cmd"),
                !BuildLauncherProcessSpec(&launcher, "\"C:\\repo\"", &process)));

    strcpy(launcher.path, code_cmd);
    TestReport("a cmd argument subject to percent expansion is rejected",
               !BuildLauncherProcessSpec(&launcher, "\"C:\\%PATH%\\repo\"",
                                         &process));
    TestReport("cmd metacharacters inert inside quotes are accepted",
               BuildLauncherProcessSpec(&launcher, "\"C:\\a & b (x) ^y\"",
                                        &process) &&
               strstr(process.command, "a & b (x) ^y") != NULL);

    launcher.kind = CLAUDE_LAUNCHER_EXE;
    strcpy(launcher.path, devenv_exe);
    snprintf(expected, sizeof(expected), "\"%s\" \"C:\\a & b\"", devenv_exe);
    TestReport("an exe launcher passes its argument through unwrapped",
               BuildLauncherProcessSpec(&launcher, "\"C:\\a & b\"", &process) &&
               strcmp(process.command, expected) == 0);
    TestReport("a percent in an exe argument needs no rejection",
               BuildLauncherProcessSpec(&launcher, "\"C:\\100%\\repo\"",
                                        &process));
    TestReport("an empty launcher path is refused",
               (launcher.path[0] = '\0',
                !BuildLauncherProcessSpec(&launcher, NULL, &process)));

    // ---------------------------------------------------------------------
    // Solution discovery
    // ---------------------------------------------------------------------
    char solutions[MAX_SOLUTIONS][MAX_PATH];
    char sln[MAX_PATH];

    TestReport("a directory with no solution reports none",
               FindSolutionsIn(none, solutions, MAX_SOLUTIONS) == 0);
    TestReport("an empty directory name reports none",
               FindSolutionsIn("", solutions, MAX_SOLUTIONS) == 0);

    bool one_ok = TestJoin(sln, sizeof(sln), one, "OnlyOne.sln") && TestTouch(sln);
    TestReport("a single solution is found and fully qualified",
               one_ok && FindSolutionsIn(one, solutions, MAX_SOLUTIONS) == 1 &&
               _stricmp(solutions[0], sln) == 0);

    // Extensions that a wildcard can reach through 8.3 short names but which
    // are not solutions
    bool decoys_ok =
        TestJoin(sln, sizeof(sln), one, "NotOne.slnx") && TestTouch(sln) &&
        TestJoin(sln, sizeof(sln), one, "NotTwo.sln.bak") && TestTouch(sln);
    TestReport("only a real .sln extension counts",
               decoys_ok && FindSolutionsIn(one, solutions, MAX_SOLUTIONS) == 1 &&
               SolutionsContain(solutions, 1, "OnlyOne.sln"));

    bool many_ok = true;
    for (int i = 0; i < MAX_SOLUTIONS + 3; i++) {
        char leaf[64];
        snprintf(leaf, sizeof(leaf), "Solution%02d.sln", i);
        many_ok = many_ok && TestJoin(sln, sizeof(sln), many, leaf) &&
                  TestTouch(sln);
    }
    TestReport("more solutions than the picker can name are capped",
               many_ok &&
               FindSolutionsIn(many, solutions, MAX_SOLUTIONS) == MAX_SOLUTIONS);
    TestReport("a smaller cap is honoured",
               FindSolutionsIn(many, solutions, 2) == 2);

    // ---------------------------------------------------------------------
    // The generated VS Code multi-root workspace
    // ---------------------------------------------------------------------
    char sanitized[MAX_PATH];
    SanitizeWorkspaceFileName("my repo", sanitized, sizeof(sanitized));
    TestReport("an ordinary display name survives sanitizing",
               strcmp(sanitized, "my repo") == 0);
    SanitizeWorkspaceFileName("a/b\\c:d*e?f\"g<h>i|j%k", sanitized,
                              sizeof(sanitized));
    TestReport("path-reserved and command-line characters are replaced",
               strcmp(sanitized, "a-b-c-d-e-f-g-h-i-j-k") == 0);
    SanitizeWorkspaceFileName("trailing... ", sanitized, sizeof(sanitized));
    TestReport("trailing dots and spaces Win32 would strip are removed",
               strcmp(sanitized, "trailing") == 0);
    SanitizeWorkspaceFileName("...", sanitized, sizeof(sanitized));
    TestReport("a name that sanitizes away becomes empty",
               sanitized[0] == '\0');

    char workspace_file[MAX_PATH], generated[8192];
    strcpy(members[0], "C:\\repo\\one");
    strcpy(members[1], "C:\\repo\\two");
    member_count = 2;
    bool wrote = WriteCodeWorkspaceFile(one, "My Workspace", workspace_file) &&
                 TestRead(workspace_file, generated, sizeof(generated));
    TestReport("the workspace file is named for the display name",
               wrote && _stricmp(LeafName(workspace_file),
                                 "My Workspace.code-workspace") == 0);
    TestReport("the anchor itself leads the folder list",
               wrote && strstr(generated,
                   "{ \"path\": \".\", \"name\": \"My Workspace\" }") != NULL);
    TestReport("member paths are written with JSON-escaped separators",
               wrote &&
               strstr(generated, "{ \"path\": \"C:\\\\repo\\\\one\" }") != NULL &&
               strstr(generated, "{ \"path\": \"C:\\\\repo\\\\two\" }") != NULL);
    TestReport("the file declares itself generated",
               wrote && strstr(generated, "// Generated by drift") != NULL);
    DeleteFile(workspace_file);

    // Unsafe characters are replaced rather than dropped, so a name made only
    // of them still yields a usable filename
    wrote = WriteCodeWorkspaceFile(one, "***", workspace_file);
    TestReport("a name of only unsafe characters becomes replacements",
               wrote && _stricmp(LeafName(workspace_file),
                                 "---.code-workspace") == 0);
    DeleteFile(workspace_file);

    // Dots and spaces are individually safe but Win32 strips them from the end,
    // so a name made only of those empties out. It must still produce a file,
    // keyed to the folder id -- a rename alone cannot make a workspace unopenable
    wrote = WriteCodeWorkspaceFile(one, ". . .", workspace_file);
    TestReport("a name that sanitizes away falls back to the folder id",
               wrote && _stricmp(LeafName(workspace_file),
                                 "one-solution.code-workspace") == 0);
    DeleteFile(workspace_file);

    // A quote in a display name must not break out of the JSON string
    wrote = WriteCodeWorkspaceFile(one, "say \"hi\"", workspace_file) &&
            TestRead(workspace_file, generated, sizeof(generated));
    TestReport("a quote in the display name is escaped in the JSON",
               wrote && strstr(generated, "\\\"hi\\\"") != NULL);
    DeleteFile(workspace_file);
    member_count = 0;

    // ---------------------------------------------------------------------
    // Staying in step with settings.json, not with the last press of 'v'
    // ---------------------------------------------------------------------
    char sync_dir[MAX_PATH], found[MAX_PATH], handmade[MAX_PATH];
    bool sync_ok = TestJoin(sync_dir, sizeof(sync_dir), root, "sync") &&
                   TestMakeDirectory(sync_dir);

    // The generated file is always named for the workspace's current display
    // name, so the name overlay has to be real for this to mean anything.
    // SetWorkspaceName writes into .drift\, so point DRIFT_HOME at the test
    // root -- this must not reach the real workspace-names file
    char saved_home[MAX_PATH];
    DWORD home_len = GetEnvironmentVariable("DRIFT_HOME", saved_home, MAX_PATH);
    bool had_home = home_len > 0 && home_len < MAX_PATH;
    SetEnvironmentVariable("DRIFT_HOME", root);
    if (!SetWorkspaceName(AnchorFolder(sync_dir), "Before")) {
        printf("    (could not set the display name for the sync checks)\n");
    }

    TestReport("an anchor with no workspace file has nothing to refresh",
               sync_ok && !FindGeneratedCodeWorkspace(sync_dir, found));

    // A file drift did not write must never be adopted, rewritten or removed
    bool handmade_ok = sync_ok &&
        TestJoin(handmade, sizeof(handmade), sync_dir, "mine.code-workspace") &&
        TestWrite(handmade, "{ \"folders\": [ { \"path\": \"C:\\\\mine\" } ] }\n");
    TestReport("a hand-made workspace file is not claimed as generated",
               handmade_ok && !IsGeneratedCodeWorkspace(handmade) &&
               !FindGeneratedCodeWorkspace(sync_dir, found));

    strcpy(members[0], "C:\\repo\\one");
    member_count = 1;
    bool gen_ok = WriteCodeWorkspaceFile(sync_dir, "Before", workspace_file);
    TestReport("a generated workspace file is recognised beside a hand-made one",
               gen_ok && IsGeneratedCodeWorkspace(workspace_file) &&
               FindGeneratedCodeWorkspace(sync_dir, found) &&
               _stricmp(found, workspace_file) == 0);

    // A membership change refreshes it in place, without a new 'v'
    strcpy(members[1], "C:\\repo\\added");
    member_count = 2;
    RefreshCodeWorkspaceFile(sync_dir);
    bool refreshed = TestRead(workspace_file, generated, sizeof(generated));
    TestReport("a membership change is picked up without reopening",
               refreshed &&
               strstr(generated, "C:\\\\repo\\\\added") != NULL &&
               GetFileAttributes(handmade) != INVALID_FILE_ATTRIBUTES);

    // A rename moves it, because VS Code titles the window from the filename
    if (!SetWorkspaceName(AnchorFolder(sync_dir), "After")) {
        printf("    (could not set the display name for the rename check)\n");
    }
    RefreshCodeWorkspaceFile(sync_dir);
    char renamed[MAX_PATH];
    bool moved = FindGeneratedCodeWorkspace(sync_dir, renamed);
    TestReport("a rename moves the generated file and drops the old name",
               moved && _stricmp(LeafName(renamed), "After.code-workspace") == 0 &&
               GetFileAttributes(workspace_file) == INVALID_FILE_ATTRIBUTES);
    TestReport("the hand-made file survives every refresh",
               GetFileAttributes(handmade) != INVALID_FILE_ATTRIBUTES &&
               TestRead(handmade, generated, sizeof(generated)) &&
               strstr(generated, "C:\\\\mine") != NULL);

    SetWorkspaceName(AnchorFolder(sync_dir), "");
    char names_file[MAX_PATH], drift_dir[MAX_PATH];
    if (TestJoin(drift_dir, sizeof(drift_dir), root, ".drift") &&
        TestJoin(names_file, sizeof(names_file), drift_dir, "workspace-names")) {
        DeleteFile(names_file);
        RemoveDirectory(drift_dir);
    }
    SetEnvironmentVariable("DRIFT_HOME", had_home ? saved_home : NULL);
    workspace_names_loaded = false; // the cache now names a deleted file
    DeleteFile(renamed);
    DeleteFile(handmade);
    RemoveDirectory(sync_dir);
    member_count = 0;

    // ---------------------------------------------------------------------
    // Detached spawn: returns without waiting, and the child really runs
    // ---------------------------------------------------------------------
    char self[MAX_PATH], spawn_marker[MAX_PATH];
    DWORD self_len = GetModuleFileName(NULL, self, MAX_PATH);
    bool spawn_ok = self_len > 0 && self_len < MAX_PATH &&
        TestJoin(spawn_marker, sizeof(spawn_marker), root, "spawned.txt");
    launcher.kind = CLAUDE_LAUNCHER_EXE;
    strcpy(launcher.path, self);
    if (spawn_ok) {
        spawn_ok = BuildLauncherProcessSpec(&launcher, "\"detached\"", &process) &&
            SetEnvironmentVariable("DRIFT_EDITOR_TEST_MARKER", spawn_marker) &&
            SpawnDetached(&process, root);
        // SpawnDetached does not wait, so the marker appears asynchronously
        for (int i = 0; i < 100 && GetFileAttributes(spawn_marker) ==
                                       INVALID_FILE_ATTRIBUTES; i++) {
            Sleep(50);
        }
        SetEnvironmentVariable("DRIFT_EDITOR_TEST_MARKER", NULL);
    }
    TestReport("a detached child runs without the parent waiting on it",
               spawn_ok &&
               GetFileAttributes(spawn_marker) != INVALID_FILE_ATTRIBUTES);

    ClaudeProcessSpec empty = {{0}, {0}};
    TestReport("an empty spec is not spawned",
               !SpawnDetached(&empty, NULL) && !SpawnDetached(NULL, NULL));

    // ---------------------------------------------------------------------
    SetCurrentDirectory(original_dir);
    DeleteFile(code_cmd);
    DeleteFile(code_exe);
    DeleteFile(devenv_exe);
    DeleteFile(cwd_code);
    DeleteFile(cwd_devenv);
    DeleteFile(vslauncher);
    DeleteFile(spawn_marker);
    for (int i = 0; i < MAX_SOLUTIONS + 3; i++) {
        char leaf[64];
        snprintf(leaf, sizeof(leaf), "Solution%02d.sln", i);
        if (TestJoin(sln, sizeof(sln), many, leaf)) DeleteFile(sln);
    }
    if (TestJoin(sln, sizeof(sln), one, "OnlyOne.sln")) DeleteFile(sln);
    if (TestJoin(sln, sizeof(sln), one, "NotOne.slnx")) DeleteFile(sln);
    if (TestJoin(sln, sizeof(sln), one, "NotTwo.sln.bak")) DeleteFile(sln);
    RemoveDirectory(decoy_launcher);
    RemoveDirectory(decoy_msenv);
    RemoveDirectory(decoy_shared);
    RemoveDirectory(decoy);
    RemoveDirectory(msenv);
    RemoveDirectory(shared);
    RemoveDirectory(vs);
    RemoveDirectory(many);
    RemoveDirectory(one);
    RemoveDirectory(none);
    RemoveDirectory(editors);
    RemoveDirectory(cwd);
    RemoveDirectory(root);

    if (test_failures == 0) {
        printf("All editor launcher tests passed.\n");
        return 0;
    }
    printf("%d editor launcher test(s) failed.\n", test_failures);
    return 1;
}
