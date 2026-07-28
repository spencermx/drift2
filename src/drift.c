// A simple terminal file browser for Windows
// Controls:
// - Q        : Quit the program
// Navigation :
// - H        : Go to parent directory
// - L        : Enter selected directory
// - J        : Move selection down
// - K        : Move selection up
// - Shift + G: Move to bottom of the list
// - gg       : Move to top of the list
// - Ctrl + D : Half page down
// - Ctrl + U : Half page up
// - ` or ~   : Jump to the home directory (DRIFT_HOME, else %USERPROFILE%)
// - Enter    : Open file in vim (falls back to default application)
// Operations on marked files:
// - Space    : Toggle mark on selected file/directory
// - Y        : Mark selected files for copy (yank)
// - X        : Mark selected files for move (cut)
// - P        : Paste (copy/move) marked files to current directory
// - D        : Delete marked files (with confirmation)
// - Ctrl + A : Mark all files in the current Directory
// - Ctrl + [ : Clear all marks
// Misc:
// - O        : Show visited directories and jump to selected one
// - A        : Create new file/directory (append '\' to name for directory)
// - .        : Toggle showing hidden files (hidden by default)
// - V        : Open in an editor. A j/k-and-Enter menu offers VS Code (the
//              directory under the cursor, else the one being browsed) and
//              Visual Studio (a .sln in that directory -- named outright when
//              there is one, picked from a second menu when there are
//              several). Both are GUI programs, so they are spawned detached
//              and drift keeps running rather than suspending itself the way
//              Enter and claude do
// Claude workspaces:
// - C        : Toggle the claude workspace browser. Workspaces are anchor
//              directories under %USERPROFILE%\.drift\workspaces; l/Enter
//              opens a workspace's session list, h/Esc backs out. Sessions
//              are read from %USERPROFILE%\.claude\projects (override the
//              .claude location with DRIFT_CLAUDE_DIR).
// - E        : (in the workspace list) edit a workspace's folders by
//              browsing: Space toggles the directory under the cursor,
//              members show a '+', the third pane pins the folder list.
//              Tab focuses that list (x removes, Enter jumps the browser
//              there), Esc finishes. Folder membership is written to the
//              workspace's .claude\settings.json additionalDirectories.
// - F        : (in the workspace list) browse the workspace's own anchor
//              directory -- the folder claude runs in, so a CLAUDE.md or
//              notes placed there apply to the whole workspace. Every
//              workspace gets an empty CLAUDE.md to fill in; an existing one
//              is never rewritten. Ordinary browsing, and h at the anchor
//              root (or Esc/c) returns to the workspace list. Quitting with
//              q instead leaves the shell in the directory the browse had
//              reached -- the one place claude mode moves the directory
//              drift exits into, since nothing else leads to a folder
//              named for a timestamp
// - R        : (in the workspace list) rename a workspace's drift display
//              name (stored in .drift\workspace-names); the folder itself
//              is never renamed, so its sessions stay associated
// - V        : (in the workspace list) open the whole workspace as one VS Code
//              multi-root window. Drift regenerates a <name>.code-workspace in
//              the anchor from the member folders and opens that -- one
//              invocation, where the fileless route would have to race
//              "code --add" against whichever window was last active
// - Shift+W  : (while browsing) add the directory under the cursor to a
//              workspace picked from a popup
// - Enter/L  : (session list) resume the session in claude, anchored in the
//              workspace; N starts a new session (works from the workspace
//              list too). An absolute launcher is resolved from PATH; native
//              executables run directly and npm .cmd shims use System32 cmd.
// - R / D    : (session list) rename the session's drift display name
//              (stored in .drift\session-names, claude files untouched)
//              / delete the session transcript (recycle bin, confirmed)
// Run "drift -c" to start directly in the claude workspace browser.
//
// Layout: three panes when the window is at least 80 columns wide --
// parent | current | context. The context pane previews the selected item:
// first lines of a text file, the listing of a directory, or a size note
// for binary files. Narrower windows fall back to the two-pane layout.
//
// Known limitations: filenames outside the system ANSI codepage are not
// supported -- they list with '?' placeholders and file operations on them
// are refused, since the mangled name would act as a wildcard (full support
// requires FindFirstFileW conversion); UNC paths not supported.
//
// Compilation - x86_64-w64-mingw32-gcc drift.c -o drift.exe
//             - cl drift.c

#define _CRT_SECURE_NO_WARNINGS
#undef UNICODE
#undef _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <shellapi.h>
#include "settings_json.h"

#pragma comment(lib, "shell32.lib")

// =========================== Types and Constants ===========================
#define MAX_FILES 4096

// Unicode box-drawing characters (rendered via WriteConsoleOutputW,
// codepage-independent)
#define BOX_TOP_LEFT     L'\x250C'
#define BOX_TOP_RIGHT    L'\x2510'
#define BOX_BOTTOM_LEFT  L'\x2514'
#define BOX_BOTTOM_RIGHT L'\x2518'
#define BOX_HORIZONTAL   L'\x2500'
#define BOX_VERTICAL     L'\x2502'
#define BOX_T_DOWN       L'\x252C'

#define POPUP_WIDTH 35
#define POPUP_HEIGHT 15
#define CREATE_POPUP_WIDTH 50
#define COLUMN_DIVIDER_POSITION 28
#define MIN_WINDOW_WIDTH (COLUMN_DIVIDER_POSITION + 8)
// Row 0 is the header and row 1 the rule, so anything shorter than this has
// no room for a pane's first line at row 2. The per-site "row < height"
// guards are what actually keeps writes in bounds; this only rules out
// geometry where the panes would have nothing left to draw
#define MIN_WINDOW_HEIGHT 7
// Third (context/preview) pane appears when the window is at least this wide.
// The current pane takes two thirds of the remaining width on narrow windows
// but never grows past the cap -- past that, all extra width goes to the
// preview pane, where long text lines can actually use it
#define THREE_PANE_MIN_WIDTH 80
#define SECOND_DIVIDER_MAX (COLUMN_DIVIDER_POSITION + 58)
#define PREVIEW_BYTES 4096

enum MarkStatus {
    MARKED,
    YANKED,
    CUT
};
enum ClaudeMode {
    CM_OFF,        // normal file browsing
    CM_WORKSPACES, // browsing the workspaces root (a real directory)
    CM_SESSIONS    // viewing one workspace's session list
};
#define MAX_SESSIONS 256
#define SESSION_NAME_LEN 96
// Drift's own state lives in .drift\ beside workspaces\, never inside an
// anchor: an anchor is a directory the user opens and writes notes in, and
// workspaces\ is the list drawn in the workspace column, so a file there
// would draw as a workspace row. Both are "key <TAB> [key2 <TAB>] name".
#define WORKSPACE_NAMES_FILE "workspace-names"
#define SESSION_NAMES_FILE   "session-names"
typedef struct {
    char path[MAX_PATH];         // full path to the .jsonl transcript
    char id[48];                 // session uuid (filename without .jsonl)
    char name[SESSION_NAME_LEN]; // renamed, else its first user message
    FILETIME mtime;
} SessionEntry;
typedef struct {
    char path[MAX_PATH];
    int selected_row;
    int top_row;
} DirectoryState;
typedef struct {
    char path[MAX_PATH];
} MarkedFile;
typedef struct {
    int index;
    int distance;
} DistanceEntry;
enum ClaudeLauncherKind {
    CLAUDE_LAUNCHER_NONE,
    CLAUDE_LAUNCHER_EXE,
    CLAUDE_LAUNCHER_CMD
};
typedef struct {
    char path[MAX_PATH];
    enum ClaudeLauncherKind kind;
} ClaudeLauncher;
#define CLAUDE_COMMAND_CAP (MAX_PATH * 2 + 128)
typedef struct {
    char application[MAX_PATH];
    char command[CLAUDE_COMMAND_CAP];
} ClaudeProcessSpec;
// The solution picker is a single-keystroke list, so it can only offer as many
// solutions as there are digit keys to name them with
#define MAX_SOLUTIONS 9

#ifndef DRIFT_MEMBER_LOCK_TIMEOUT_MS
#define DRIFT_MEMBER_LOCK_TIMEOUT_MS 1500
#endif
#ifndef DRIFT_MEMBER_LOCK_RETRY_MS
#define DRIFT_MEMBER_LOCK_RETRY_MS 25
#endif
#define MEMBER_CONFLICT_REASON "(workspace folders changed; retry the edit)"
#define MEMBER_PATH_BLOCK_REASON "(a folder path cannot be resolved safely)"

enum MemberChangeAction {
    MEMBER_CHANGE_ADD,
    MEMBER_CHANGE_REMOVE
};
enum MemberChangeResult {
    MEMBER_CHANGE_SAVED,
    MEMBER_CHANGE_NO_CHANGE,
    MEMBER_CHANGE_FULL,
    MEMBER_CHANGE_SETTINGS_BLOCKED,
    MEMBER_CHANGE_BUSY,
    MEMBER_CHANGE_CONFLICT,
    MEMBER_CHANGE_IO_FAILED
};
enum MemberLockResult {
    MEMBER_LOCK_ACQUIRED,
    MEMBER_LOCK_BUSY,
    MEMBER_LOCK_FAILED
};
// =========================== Types and Constants ===========================

// =========================== Function Declarations =========================
void DrawScreen();
int HandleInput();
void ModifySelectedRow(int num);
void ChangeCurrentDirectory(char* path);
void JumpToHome();
int GetFilesInDirectory(char* path, WIN32_FIND_DATA files[]);
int CompareFiles(const void* a, const void* b);
void GetParentDirectory(char* path, char* parent);
bool IsDirectory(WIN32_FIND_DATA* file_data);
WORD FileColor(WIN32_FIND_DATA* file);
void DrawContextPane(CHAR_INFO* buffer, int width, int height, int divider2);
void LoadPreview();
void FormatSize(ULONGLONG size, char* out, int out_size);
void EnterClaudeMode();
void ExitClaudeMode();
void EnterSessionsView();
void LoadSessionsFor(const char* anchor);
void ReadSessionName(const char* path, char* out, int out_size);
int CompareSessions(const void* a, const void* b);
void FormatAge(FILETIME ft, char* out, int out_size);
void EncodeProjectPath(const char* path, char* out, bool keep_dots);
void DrawSessionsPanes(CHAR_INFO* buffer, int width, int height, int divider2, int list_height);
void DrawClaudeInfoPane(CHAR_INFO* buffer, int width, int height, int divider2);
void DrawClaudeHelpPane(CHAR_INFO* buffer, int width, int height);
bool GetDriftDir(char* out);
bool GetWorkspacesRoot(char* out);
const char* AnchorFolder(const char* anchor);
bool GetNameFile(char* out, const char* leaf);
bool SetNameEntry(const char* leaf, const char* key, const char* name);
void LoadWorkspaceNames();
void LoadMembersFrom(const char* anchor);
size_t AppendFmt(char* buf, size_t n, size_t cap, const char* fmt, ...);
bool SaveMembersTo(const char* anchor);
bool ResolveMemberPath(const char* anchor, const char* stored,
                       char out[MAX_PATH]);
int FindMember(const char* path);
enum MemberLockResult AcquireMemberLock(const char* anchor, HANDLE* lock);
enum MemberChangeResult ApplyMemberChange(
    const char* anchor, const char* path, enum MemberChangeAction action);
void RemoveMemberAt(int index);
bool JumpToMemberAt(int index);
void ToggleMemberUnderCursor();
void EnterEditMode();
void ExitEditMode();
void EnsureWorkspaceNotes(const char* anchor);
void EnterAnchorMode();
void ExitAnchorMode();
void HandleQuickAdd();
void HandleOpenWorkspaceInEditor();
bool ResolveClaudeLauncherFromPath(const char* path_value, ClaudeLauncher* out);
bool ResolveClaudeLauncher(ClaudeLauncher* out);
bool ResolveVimFromPath(const char* path_value, char out[MAX_PATH]);
bool ResolveVim(char out[MAX_PATH]);
bool ResolveVsCodeFromPath(const char* path_value, ClaudeLauncher* out);
bool ResolveVsCode(ClaudeLauncher* out);
bool ResolveDevenvFromPath(const char* path_value, ClaudeLauncher* out);
bool ResolveDevenv(ClaudeLauncher* out);
bool ResolveVisualStudioLauncher(ClaudeLauncher* out);
bool BuildLauncherProcessSpec(const ClaudeLauncher* launcher, const char* argument,
                              ClaudeProcessSpec* out);
bool BuildClaudeProcessSpec(const ClaudeLauncher* launcher, const char* session_id,
                            ClaudeProcessSpec* out);
bool SpawnDetached(ClaudeProcessSpec* spec, const char* working_dir);
int FindSolutionsIn(const char* dir, char names[][MAX_PATH], int max);
void HandleOpenInEditor();
int LaunchClaudeIn(const char* anchor, const char* session_id);
void ScrollOriginalScreen();
bool IsSafeSessionId(const char* id);
void ApplySessionNames(const char* anchor);
bool SetSessionName(const char* anchor, const char* id, const char* name);
void WorkspaceDisplayName(const char* folder, char* out, size_t out_size);
bool SetWorkspaceName(const char* folder, const char* display);
bool WorkspaceNameTaken(const char* name, const char* except_folder);
void NotifyAndWait(const char* text);
void NotifyNameTaken(const char* name);
void HandleRenameWorkspace();
void HandleRenameSession();
void HandleDeleteSession();
void DrawManifestPane(CHAR_INFO* buffer, int width, int height, int divider2);
bool GetSelectedRowPath(int selected_row, char* out_path);
bool GetFilePath(char* current_directory, WIN32_FIND_DATA* file_data, char* out_path);
int CalculateDistance(char* current, char* target);
int GetVisibleRows();
bool IsRootDirectory(char* path);
void SaveDirectoryState();
void RestoreDirectoryState();
void Cleanup();
void Initialize();
void ToggleMark();
bool MarkedFilesFull();
void SetMarkStatus(enum MarkStatus status);
void HandlePaste(int move);
bool IsMarkDirectorySet();
bool MarkDirEqualToCurrentDir();
void ConfirmDelete();
void DrawDeletePopup(int width, int height, CHAR_INFO* out_buffer);
void WriteToBuffer(CHAR_INFO* buffer, int width, int row, int col, const char* text, WORD color);
void WriteToBufferCP(CHAR_INFO* buffer, int width, int row, int col, const char* text, WORD color, UINT cp);
void AnsiToWide(const char* src, wchar_t* dst, int dst_count);
void BytesToWide(const char* src, wchar_t* dst, int dst_count, UINT cp);
int Utf8Prefix(const char* s, int cells, int max_bytes);
char* BuildFromList(size_t* out_size);
void OpenFileInEditor();

void LoadParentDirectory();
void LoadCurrentDirectory();
void ReloadCurrentDirectory();
void ClearMarkedFiles();
void DrawCreatePopup(int width, char* input_text, const char* placeholder, CHAR_INFO* out_buffer, UINT cp);
void ShowStatusBanner(const char* text);
void HandleCreate();
void HandleMarkOperation(enum MarkStatus new_status);
void HandleOldHistory();
void DrawOldHistoryPopup(int width, int height, DistanceEntry* distances, int display_count, CHAR_INFO* out_buffer);
// =========================== Function Declarations =========================

// =========================== Global Variables ==============================
WORD white = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
WORD blue = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
WORD red = FOREGROUND_RED | FOREGROUND_INTENSITY;
WORD green = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
WORD yellow = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
WORD gray = FOREGROUND_INTENSITY;
// Dark yellow renders as the console's golden/orange tone -- the closest
// 16-color match to Claude's brand color; used for the frame in claude mode
WORD orange = FOREGROUND_RED | FOREGROUND_GREEN;
// Selection bar: light-gray background; row foregrounds are remapped to
// their dark variants so the text stays readable on it
WORD bar_background = BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
WORD color;

HANDLE hIn;
HANDLE hOriginal;
HANDLE hAlt;
DWORD original_console_mode;

DirectoryState history[MAX_FILES];
MarkedFile marked_files[MAX_FILES];
WIN32_FIND_DATA current_directory_files[MAX_FILES];
WIN32_FIND_DATA parent_directory_files[MAX_FILES];

// Context (preview) pane state, cached by the selected path so the file or
// directory is only re-read when the cursor moves to a different item
WIN32_FIND_DATA preview_files[MAX_FILES];
int preview_file_count;
char preview_path[MAX_PATH];
char preview_bytes[PREVIEW_BYTES];
int preview_len;
ULONGLONG preview_size;
bool preview_is_dir;
bool preview_binary;
bool preview_unreadable;
// Which encoding the preview bytes arrived in. Filenames are always the ANSI
// codepage, but file contents mostly are not, and the frame buffer is UTF-16
// either way -- so this only decides how the bytes are decoded on the way in
bool preview_utf8;

char mark_directory[MAX_PATH];
char parent_directory[MAX_PATH];
char current_directory[MAX_PATH];

int history_count = 0;
int marked_files_count = 0;
int current_directory_file_count;
int parent_directory_file_count;

int selected_row = 0;
int top_row = 0;

bool pending_g = false;
bool show_hidden = false;

enum ClaudeMode claude_mode = CM_OFF;
SessionEntry sessions[MAX_SESSIONS];
int session_count = 0;
int session_selected = 0;
int session_top = 0;
char workspaces_root[MAX_PATH];
char claude_workspace[MAX_PATH];      // anchor path of the open workspace
char claude_workspace_name[MAX_PATH];
char claude_return_dir[MAX_PATH];     // where to go back to on exit

// Workspace editing ("armed") mode: browse normally while the third pane
// pins the workspace's folder list; Space toggles membership
#define MAX_MEMBERS 128
bool edit_armed = false;
char edit_workspace[MAX_PATH];
char edit_workspace_name[MAX_PATH];
char members[MAX_MEMBERS][MAX_PATH];
char member_anchor[MAX_PATH];
int member_count = 0;
bool manifest_focused = false;
int manifest_selected = 0;
int manifest_top = 0;
char json_buf[65536];
char* member_source_array = NULL;
size_t member_source_array_length = 0;
bool member_source_known = false;
bool member_source_has_array = false;
// Non-NULL when settings.json holds something that cannot be rewritten without
// losing information. Edits are refused rather than made lossy, and the string
// is what the manifest pane shows, so it carries the reason and its own parens
const char* json_block_reason = NULL;
char sessions_loaded_for[MAX_PATH]; // cache key for the sessions[] array

// Browsing a workspace's own anchor directory. Ordinary file browsing pinned
// inside one anchor, so the workspace's CLAUDE.md and notes are reachable
// without knowing (or being able to type) the timestamp path
bool anchor_armed = false;
char anchor_workspace[MAX_PATH];
char anchor_workspace_name[MAX_PATH];

// WorkspaceDisplayName runs for every drawn row, so the workspace-names file
// is held in memory rather than reopened per row. Writes drop the flag.
// Sized to the file rather than fixed: a read cut short at a fixed cap hid
// every row past it, so those workspaces fell back to their raw folder ids
// and WorkspaceNameTaken -- which resolves through WorkspaceDisplayName --
// stopped seeing their names and let duplicates through
char* workspace_names = NULL;
bool workspace_names_loaded = false;

enum MarkStatus mark_status = MARKED;
// True when the current set was created by y/x/d falling back to the cursor
// row rather than built deliberately with Space. Such a set is a transient
// "this file", so the next verb re-aims at the cursor instead of reusing it.
// Provenance rather than count: keying on "exactly one mark" would go back to
// discarding a deliberate single Space mark, which is what HandleMarkOperation
// stopped doing
bool implicit_mark = false;
// =========================== Global Variables ==============================
int main(int argc, char* argv[]) {
    Initialize();

    // drift -c boots straight into the claude workspace browser
    if (argc > 1 && strcmp(argv[1], "-c") == 0) {
        EnterClaudeMode();
    }

    do {
        DrawScreen();
    } while(HandleInput());

    Cleanup();

    return 0;
}

void Initialize() {
    hIn = GetStdHandle(STD_INPUT_HANDLE);
    hOriginal = GetStdHandle(STD_OUTPUT_HANDLE);

    // Create an alternate screen buffer
    hAlt = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, 0, NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
    if (hAlt == INVALID_HANDLE_VALUE) {
        // No console attached (launched detached, or from a GUI host). Every
        // console call would fail from here on, and DrawScreen would size its
        // frame buffer from indeterminate window dimensions
        fprintf(stderr, "drift: no console screen buffer available\n");
        exit(1);
    }
    SetConsoleActiveScreenBuffer(hAlt);

    // The selection bar marks the current row -- the blinking hardware
    // cursor is only shown inside the create-name popup
    CONSOLE_CURSOR_INFO cursor_info = { 25, FALSE };
    SetConsoleCursorInfo(hAlt, &cursor_info);

    // Set initial directory (copy into a separate buffer first --
    // ChangeCurrentDirectory strcpy's into current_directory, and
    // overlapping source/destination is undefined behavior)
    char start_dir[MAX_PATH];
    DWORD start_dir_len = GetCurrentDirectory(MAX_PATH, start_dir);
    if (start_dir_len == 0 || start_dir_len >= MAX_PATH) {
        strcpy(start_dir, "C:\\"); // failure leaves start_dir indeterminate
    }
    ChangeCurrentDirectory(start_dir);

    GetConsoleMode(hIn, &original_console_mode);
    // ENABLE_WINDOW_INPUT is disabled by default, and without it the console
    // never delivers resize events -- the screen would stay stale until the
    // next keypress
    SetConsoleMode(hIn, (original_console_mode & ~ENABLE_PROCESSED_INPUT) | ENABLE_WINDOW_INPUT);
}

void DrawScreen() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) {
        return; // info is indeterminate on failure -- never size a buffer from it
    }
    int width = info.srWindow.Right - info.srWindow.Left + 1;
    int height = info.srWindow.Bottom - info.srWindow.Top + 1;

    // Focus can only live in the manifest while there is a manifest on screen
    // with something in it: below THREE_PANE_MIN_WIDTH it is not drawn, and
    // empty it draws no rows. Either way the file list's selection bar is
    // suppressed for a cursor that never appears, while j/k still move and
    // Space still removes members nothing is showing. Cleared here, ahead of
    // the too-small bail below, because that path draws nothing at all --
    // and re-checked every frame, so narrowing the window also releases it
    if (width < THREE_PANE_MIN_WIDTH || member_count == 0) {
        manifest_focused = false;
    }

    // Window too small to draw the layout safely (header line, rule line, and
    // room for the panes' fixed rows -- see MIN_WINDOW_HEIGHT)
    if (width < MIN_WINDOW_WIDTH || height < MIN_WINDOW_HEIGHT) {
        return;
    }

    int list_height = height - 2; // row 0 is the header, row 1 the rule

    // Three panes (parent | current | context) when there is room; otherwise
    // divider2 sits at the right edge and the layout degrades to two panes
    bool three_pane = width >= THREE_PANE_MIN_WIDTH;
    int divider2 = width;
    if (three_pane) {
        divider2 = width - width / 3;
        if (divider2 > SECOND_DIVIDER_MAX) {
            divider2 = SECOND_DIVIDER_MAX;
        }
    }

    // Re-clamp scroll state against the *current* window size. The window may
    // have been resized since the last keypress, which would otherwise let the
    // cursor draw below write past the end of the buffer.
    if (claude_mode == CM_SESSIONS) {
        if (session_count == 0) {
            session_selected = 0;
            session_top = 0;
        } else {
            if (session_selected >= session_count) {
                session_selected = session_count - 1;
            }
            if (session_top > session_count - list_height) {
                session_top = session_count - list_height;
            }
            if (session_top < 0) {
                session_top = 0;
            }
            if (session_top > session_selected) {
                session_top = session_selected;
            }
            if (session_selected - session_top >= list_height) {
                session_top = session_selected - list_height + 1;
            }
        }
    } else if (current_directory_file_count == 0) {
        selected_row = 0;
        top_row = 0;
    } else {
        if (selected_row >= current_directory_file_count) {
            selected_row = current_directory_file_count - 1;
        }
        if (top_row > current_directory_file_count - list_height) {
            top_row = current_directory_file_count - list_height;
        }
        if (top_row < 0) {
            top_row = 0;
        }
        if (top_row > selected_row) {
            top_row = selected_row;
        }
        if (selected_row - top_row >= list_height) {
            top_row = selected_row - list_height + 1;
        }
    }

    color = white;

    // Persistent frame buffer, grown on demand
    static CHAR_INFO* buffer = NULL;
    static int buffer_capacity = 0;
    if (width * height > buffer_capacity) {
        CHAR_INFO* new_buffer = (CHAR_INFO*)realloc(buffer, width * height * sizeof(CHAR_INFO));
        if (new_buffer == NULL) {
            return;
        }
        buffer = new_buffer;
        buffer_capacity = width * height;
    }

    // Fill with spaces and default color
    for (int i = 0; i < width * height; i++) {
        buffer[i].Char.UnicodeChar = L' ';
        buffer[i].Attributes = color;
    }

    wchar_t wname[MAX_PATH];

    // ============================= Draw Header Line ==================================
    {
        char right_info[48];
        if (claude_mode == CM_SESSIONS) {
            int position = session_count == 0 ? 0 : session_selected + 1;
            snprintf(right_info, sizeof(right_info), "%d/%d", position, session_count);
        } else {
            int position = current_directory_file_count == 0 ? 0 : selected_row + 1;
            if (marked_files_count > 0) {
                char mark_char = mark_status == YANKED ? 'Y' : (mark_status == CUT ? 'X' : '*');
                snprintf(right_info, sizeof(right_info), "%c%d  %d/%d",
                         mark_char, marked_files_count, position, current_directory_file_count);
            } else {
                snprintf(right_info, sizeof(right_info), "%d/%d",
                         position, current_directory_file_count);
            }
        }

        int right_col = width - (int)strlen(right_info) - 1;
        if (right_col > 0) {
            WriteToBuffer(buffer, width, 0, right_col, right_info, white);
        }

        // Claude mode announces itself with a "Claude Mode" banner on the
        // left (yellow) plus a header over the column that holds the list, so
        // the column's contents are labeled. Ordinary browsing shows the path
        // on the left, truncated from the front so the deepest (most useful)
        // part stays visible.
        if (claude_mode != CM_OFF) {
            // Banner in the same golden tone as the frame/border lines
            WriteToBuffer(buffer, width, 0, 0, "Claude Mode", orange);
            // Header each list column with what it holds. In the workspace
            // view the second column is the workspace list and the third is a
            // preview of the highlighted workspace's sessions; in the session
            // view the second column is the session list. Each label aligns to
            // where that column's items begin and is skipped if it would run
            // into the right-hand counter.
            if (claude_mode == CM_WORKSPACES) {
                int wcol = COLUMN_DIVIDER_POSITION + 4;
                if (wcol + (int)strlen("Workspaces") < right_col) {
                    WriteToBuffer(buffer, width, 0, wcol, "Workspaces", gray);
                }
                int scol = divider2 + 2;
                if (three_pane && scol + (int)strlen("Sessions") < right_col) {
                    WriteToBuffer(buffer, width, 0, scol, "Sessions", gray);
                }
            } else { // CM_SESSIONS
                int scol = COLUMN_DIVIDER_POSITION + 2;
                if (scol + (int)strlen("Sessions") < right_col) {
                    WriteToBuffer(buffer, width, 0, scol, "Sessions", gray);
                }
            }
        } else {
            char* header_path = current_directory;
            int path_col = 0;
            // Both armed modes browse ordinary directories, so the header
            // names the workspace they belong to -- an anchor's folder is an
            // opaque timestamp, and nothing else on screen would say which
            // workspace these files are
            char tag[96];
            tag[0] = '\0';
            if (edit_armed) {
                snprintf(tag, sizeof(tag), "[editing workspace %s] ", edit_workspace_name);
            } else if (anchor_armed) {
                snprintf(tag, sizeof(tag), "[workspace %s] ", anchor_workspace_name);
            }
            if (tag[0] != '\0') {
                WriteToBuffer(buffer, width, 0, 0, tag, yellow);
                path_col = (int)strlen(tag);
            }
            int path_avail = right_col - 2 - path_col;
            int path_len = (int)strlen(header_path);
            char path_display[MAX_PATH + 4];
            if (path_len <= path_avail) {
                strcpy(path_display, header_path);
            } else if (path_avail > 3) {
                snprintf(path_display, sizeof(path_display), "...%s",
                         header_path + (path_len - (path_avail - 3)));
            } else {
                path_display[0] = '\0';
            }
            WriteToBuffer(buffer, width, 0, path_col, path_display, blue);
        }
    }

    // Horizontal rule under the header, with a junction where it crosses
    // the column divider
    // Frame follows the mode: blue for files, claude-orange in claude mode
    // and while a workspace edit is armed
    WORD frame_color = (claude_mode == CM_OFF && !edit_armed && !anchor_armed) ? blue : orange;
    for (int col = 0; col < width; col++) {
        int index = width + col;
        bool junction = col == COLUMN_DIVIDER_POSITION || (three_pane && col == divider2);
        buffer[index].Char.UnicodeChar = junction ? BOX_T_DOWN : BOX_HORIZONTAL;
        buffer[index].Attributes = frame_color;
    }
    // ============================= Draw Header Line ==================================

    // ============================= Draw Parent Directory =============================
    for (int i = 0; claude_mode == CM_OFF && i < list_height && i < parent_directory_file_count; i++) {
        int file_index = i;

        color = FileColor(&parent_directory_files[file_index]);

        AnsiToWide(parent_directory_files[file_index].cFileName, wname, MAX_PATH);
        int len = (int)wcslen(wname);

        for (int col = 0; col < len && col < COLUMN_DIVIDER_POSITION - 2; col++) {
            int index = (i + 2) * width + col;
            buffer[index].Char.UnicodeChar = wname[col];
            buffer[index].Attributes = color;
        }
    }
    // ============================= Draw Parent Directory =============================

    // ============================= Draw Column Divider ===============================
    for (int i = 2; i < height; i++) {
        int index = i * width + COLUMN_DIVIDER_POSITION;
        buffer[index].Char.UnicodeChar = BOX_VERTICAL;
        buffer[index].Attributes = frame_color;
    }
    if (three_pane) {
        for (int i = 2; i < height; i++) {
            int index = i * width + divider2;
            buffer[index].Char.UnicodeChar = BOX_VERTICAL;
            buffer[index].Attributes = frame_color;
        }
    }
    // ============================= Draw Column Divider ===============================

    // ============================= Draw Current Directory ============================
    for (int i = 0; claude_mode != CM_SESSIONS && i < list_height && top_row + i < current_directory_file_count; i++) {
        int file_index = top_row + i;
        bool is_selected = (file_index == selected_row);

        // Folders keep their normal drift color even in claude mode -- the
        // orange frame and yellow header already signal the mode, so the
        // list stays as legible as the ordinary browser
        color = FileColor(&current_directory_files[file_index]);

        // Only one selection bar is ever on screen -- it lives wherever the
        // focus is, so it vanishes here while the manifest list is focused
        if (is_selected && !manifest_focused) {
            // Remap the foreground to its dark variant and paint the whole
            // pane-width segment as the selection bar. White and yellow are
            // both too light on the silver bar once dimmed (dark yellow on
            // silver is barely legible), so collapse them to black instead --
            // matching the black-on-bar text the session list already uses
            WORD fg = color & (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            if (fg == (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE) ||
                fg == (FOREGROUND_RED | FOREGROUND_GREEN)) {
                fg = 0;
            }
            color = fg | bar_background;
            for (int col = COLUMN_DIVIDER_POSITION + 1; col < divider2; col++) {
                buffer[(i + 2) * width + col].Attributes = color;
            }
        }

        // In the workspace list, show the drift-side display name (folders
        // themselves are never renamed, so their sessions stay associated)
        if (claude_mode == CM_WORKSPACES && IsDirectory(&current_directory_files[file_index])) {
            char disp[MAX_PATH];
            WorkspaceDisplayName(current_directory_files[file_index].cFileName, disp, sizeof(disp));
            AnsiToWide(disp, wname, MAX_PATH);
        } else {
            AnsiToWide(current_directory_files[file_index].cFileName, wname, MAX_PATH);
        }
        int len = (int)wcslen(wname);

        for (int col = 0; col < len && col + COLUMN_DIVIDER_POSITION + 4 < divider2; col++) {
            int index = (i + 2) * width + COLUMN_DIVIDER_POSITION + 4 + col;
            buffer[index].Char.UnicodeChar = wname[col];
            buffer[index].Attributes = color;
        }

        // ================== Highlight marked files in the current directory ==================
        // Matched by path, not row index, so highlights survive directory
        // reloads and re-sorting
        if (!edit_armed && marked_files_count > 0 && MarkDirEqualToCurrentDir()) {
            char row_path[MAX_PATH];
            // A row whose path can't be built (mangled or over-long name)
            // can never be in marked_files, and row_path would be unset
            if (GetFilePath(current_directory, &current_directory_files[file_index], row_path)) {
                for (int j = 0; j < marked_files_count; j++) {
                    if (strcmp(marked_files[j].path, row_path) == 0) {
                        int mark_index = (i + 2) * width + COLUMN_DIVIDER_POSITION + 1;

                        WORD mark_color = white;
                        if (mark_status == MARKED) {
                            buffer[mark_index].Char.UnicodeChar = L'*';
                            mark_color = yellow;
                        } else if (mark_status == YANKED) {
                            buffer[mark_index].Char.UnicodeChar = L'Y';
                            mark_color = green;
                        } else if (mark_status == CUT) {
                            buffer[mark_index].Char.UnicodeChar = L'X';
                            mark_color = red;
                        }

                        // On the selection bar, dim the mark to its dark
                        // variant and keep the bar background beneath it
                        if (is_selected) {
                            mark_color = (mark_color & (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)) | bar_background;
                        }
                        buffer[mark_index].Attributes = mark_color;
                        break;
                    }
                }
            }
        }
        // ================== Highlight marked files in the current directory ==================

        // While a workspace edit is armed, member directories show a '+'
        if (edit_armed && claude_mode == CM_OFF && IsDirectory(&current_directory_files[file_index])) {
            char row_path[MAX_PATH];
            if (GetFilePath(current_directory, &current_directory_files[file_index], row_path) &&
                FindMember(row_path) >= 0) {
                int mark_index = (i + 2) * width + COLUMN_DIVIDER_POSITION + 1;
                buffer[mark_index].Char.UnicodeChar = L'+';
                buffer[mark_index].Attributes = (is_selected && !manifest_focused)
                    ? ((yellow & (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)) | bar_background)
                    : yellow;
            }
        }
    }
    // ============================= Draw Current Directory ============================

    // Empty directories get an explicit placeholder instead of a blank pane;
    // an empty workspace list also says what to do about it
    if (claude_mode != CM_SESSIONS && current_directory_file_count == 0) {
        if (claude_mode == CM_WORKSPACES) {
            WriteToBuffer(buffer, width, 2, COLUMN_DIVIDER_POSITION + 4, "(no workspaces yet)", gray);
            if (3 < height) {
                WriteToBuffer(buffer, width, 3, COLUMN_DIVIDER_POSITION + 4, "press a to create one", gray);
            }
        } else {
            WriteToBuffer(buffer, width, 2, COLUMN_DIVIDER_POSITION + 4, "(empty)", gray);
        }
    }

    if (three_pane && claude_mode == CM_OFF && !edit_armed && current_directory_file_count > 0) {
        DrawContextPane(buffer, width, height, divider2);
    }

    if (three_pane && claude_mode == CM_OFF && edit_armed) {
        DrawManifestPane(buffer, width, height, divider2);
    }

    if (claude_mode == CM_WORKSPACES) {
        DrawClaudeHelpPane(buffer, width, height);
    }
    if (three_pane && claude_mode == CM_WORKSPACES) {
        DrawClaudeInfoPane(buffer, width, height, divider2);
    }

    if (claude_mode == CM_SESSIONS) {
        DrawSessionsPanes(buffer, width, height, divider2, list_height);
    }

    COORD buffer_size = { (SHORT)width, (SHORT)height };
    COORD origin = { 0, 0 };
    // Anchor the write region to the visible window rather than the buffer
    // origin, so drawing stays correct even if the two ever diverge
    SMALL_RECT region = {
        info.srWindow.Left,
        info.srWindow.Top,
        (SHORT)(info.srWindow.Left + width - 1),
        (SHORT)(info.srWindow.Top + height - 1)
    };
    WriteConsoleOutputW(hAlt, buffer, buffer_size, origin, &region);
}

// ============================= Context Pane ======================================
// Reads the selected item into the preview cache: directory listing for
// directories, the first PREVIEW_BYTES for files (NUL byte anywhere in the
// head means binary)
void LoadPreview() {
    preview_len = 0;
    preview_file_count = 0;
    preview_binary = false;
    preview_unreadable = false;

    WIN32_FIND_DATA* sel = &current_directory_files[selected_row];
    preview_is_dir = IsDirectory(sel);
    preview_size = ((ULONGLONG)sel->nFileSizeHigh << 32) | sel->nFileSizeLow;

    char path[MAX_PATH];
    if (!GetSelectedRowPath(selected_row, path)) {
        preview_unreadable = true;
        return;
    }

    if (preview_is_dir) {
        preview_file_count = GetFilesInDirectory(path, preview_files);
        return;
    }

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        preview_unreadable = true;
        return;
    }
    preview_len = (int)fread(preview_bytes, 1, PREVIEW_BYTES, f);
    fclose(f);

    preview_binary = memchr(preview_bytes, '\0', preview_len) != NULL;

    // Strip a UTF-8 BOM so it doesn't render as garbage
    bool had_bom = preview_len >= 3 && (unsigned char)preview_bytes[0] == 0xEF &&
                   (unsigned char)preview_bytes[1] == 0xBB &&
                   (unsigned char)preview_bytes[2] == 0xBF;
    if (had_bom) {
        memmove(preview_bytes, preview_bytes + 3, preview_len - 3);
        preview_len -= 3;
    }

    // A BOM is a declaration; otherwise ask whether the bytes are valid UTF-8.
    // Pure ASCII satisfies both encodings and decodes identically, and real
    // codepage text essentially never forms valid UTF-8 by accident, so the
    // test is safe in both directions
    preview_utf8 = had_bom;
    if (!preview_utf8 && !preview_binary && preview_len > 0) {
        // The read stops at PREVIEW_BYTES and can land mid-character, so a
        // dangling lead byte would condemn the whole file to the wrong
        // decoder. Ignore a trailing sequence that is merely unfinished
        int test = preview_len;
        for (int back = 1; back <= 4 && back <= test; back++) {
            unsigned char c = (unsigned char)preview_bytes[test - back];
            if (c < 0x80) break;  // complete on its own
            if (c >= 0xC0) {      // lead byte: how long should it have been?
                int need = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2);
                if (back < need) test -= back;
                break;
            }
        }
        // Length passed explicitly: a full read fills preview_bytes exactly,
        // leaving it without a terminator for -1 to find
        preview_utf8 = test > 0 &&
                       MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           preview_bytes, test, NULL, 0) > 0;
    }
}

void DrawContextPane(CHAR_INFO* buffer, int width, int height, int divider2) {
    char sel_path[MAX_PATH];
    bool have_path = GetSelectedRowPath(selected_row, sel_path);
    if (!have_path) {
        sel_path[0] = '\0';
    }
    // "" means this row has no usable path, but it is also the sentinel the
    // load/reload paths write to force a refresh, so comparing the two would
    // match and skip the very reload that was being asked for -- leaving the
    // previous row's preview on screen under this row's name. Never cache-hit
    // on it; LoadPreview re-derives the path itself and reports it unreadable
    if (!have_path || strcmp(sel_path, preview_path) != 0) {
        strcpy(preview_path, sel_path);
        LoadPreview();
    }

    int col_start = divider2 + 2;

    if (preview_unreadable) {
        WriteToBuffer(buffer, width, 2, col_start, "(cannot preview)", gray);
        return;
    }

    if (preview_is_dir) {
        if (preview_file_count == 0) {
            WriteToBuffer(buffer, width, 2, col_start, "(empty)", gray);
            return;
        }
        for (int i = 0; i + 2 < height && i < preview_file_count; i++) {
            WriteToBuffer(buffer, width, 2 + i, col_start,
                          preview_files[i].cFileName, FileColor(&preview_files[i]));
        }
        return;
    }

    if (preview_binary) {
        char size_str[32];
        char msg[64];
        FormatSize(preview_size, size_str, sizeof(size_str));
        snprintf(msg, sizeof(msg), "(binary - %s)", size_str);
        WriteToBuffer(buffer, width, 2, col_start, msg, gray);
        return;
    }

    if (preview_len == 0) {
        WriteToBuffer(buffer, width, 2, col_start, "(empty file)", gray);
        return;
    }

    // Text file: one source line per row, tabs expanded, control chars blanked
    int row = 2;
    int line_start = 0;
    for (int i = 0; i <= preview_len && row < height; i++) {
        if (i == preview_len || preview_bytes[i] == '\n') {
            char line[256];
            int n = 0;
            for (int j = line_start; j < i && n < (int)sizeof(line) - 1; j++) {
                unsigned char ch = (unsigned char)preview_bytes[j];
                if (ch == '\r') continue;
                if (ch == '\t') {
                    for (int t = 0; t < 4 && n < (int)sizeof(line) - 1; t++) line[n++] = ' ';
                } else if (ch < 32) {
                    line[n++] = ' ';
                } else {
                    line[n++] = (char)ch;
                }
            }
            line[n] = '\0';
            // The one call site in the file carrying file bytes rather than a
            // filename or a literal
            WriteToBufferCP(buffer, width, row, col_start, line, white,
                            preview_utf8 ? CP_UTF8 : CP_ACP);
            row++;
            line_start = i + 1;
        }
    }
}

void FormatSize(ULONGLONG size, char* out, int out_size) {
    if (size < 1024ULL) {
        snprintf(out, out_size, "%u B", (unsigned)size);
    } else if (size < 1024ULL * 1024) {
        snprintf(out, out_size, "%.1f KB", size / 1024.0);
    } else if (size < 1024ULL * 1024 * 1024) {
        snprintf(out, out_size, "%.1f MB", size / (1024.0 * 1024));
    } else {
        snprintf(out, out_size, "%.1f GB", size / (1024.0 * 1024 * 1024));
    }
}
// ============================= Context Pane ======================================

// ============================= Claude Workspaces =================================
// %DRIFT_HOME%|%USERPROFILE%\.drift -- everything drift stores about itself
bool GetDriftDir(char* out) {
    char home[MAX_PATH];
    DWORD len = GetEnvironmentVariable("DRIFT_HOME", home, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        len = GetEnvironmentVariable("USERPROFILE", home, MAX_PATH);
    }
    if (len == 0 || len >= MAX_PATH) return false;
    if (snprintf(out, MAX_PATH, "%s\\.drift", home) >= MAX_PATH) return false;
    CreateDirectory(out, NULL); // ok if it already exists
    return true;
}

bool GetWorkspacesRoot(char* out) {
    char dir[MAX_PATH];
    if (!GetDriftDir(dir)) return false;
    if (snprintf(out, MAX_PATH, "%s\\workspaces", dir) >= MAX_PATH) return false;
    CreateDirectory(out, NULL);
    return true;
}

// The folder name under workspaces_root. It is the key both name files use --
// never the display name, which changes, and never the full path, which
// differs between the Wine wrapper and a native run
const char* AnchorFolder(const char* anchor) {
    const char* slash = strrchr(anchor, '\\');
    return slash != NULL ? slash + 1 : anchor;
}

bool GetNameFile(char* out, const char* leaf) {
    char dir[MAX_PATH];
    if (!GetDriftDir(dir)) return false;
    return snprintf(out, MAX_PATH, "%s\\%s", dir, leaf) < MAX_PATH;
}

// Replaces (or, with an empty name, drops) the row under `key`. Streams the
// old file through a temp copy rather than rewriting from memory, so the file
// is never bounded by a buffer. Every I/O boundary is fail-closed: the temp is
// published only after the old file was read and both streams closed cleanly.
// `key` is one field for workspace names, "folder<TAB>session" for session
// names; a row matches when the line starts with the key and a tab.
bool SetNameEntry(const char* leaf, const char* key, const char* name) {
    char file[MAX_PATH];
    if (!GetNameFile(file, leaf)) return false;
    char tmp[MAX_PATH];
    int tmp_len = snprintf(tmp, MAX_PATH, "%s.tmp", file);
    if (tmp_len < 0 || tmp_len >= MAX_PATH) return false;

    FILE* in = fopen(file, "rb");
    bool existing = in != NULL;
    if (!existing) {
        DWORD attr = GetFileAttributes(file);
        DWORD err = GetLastError();
        bool absent = attr == INVALID_FILE_ATTRIBUTES &&
                      (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
        if (!absent) return false;
    }

    FILE* out = fopen(tmp, "wb");
    if (out == NULL) {
        if (in != NULL) fclose(in);
        return false;
    }

    bool ok = true;
    size_t klen = strlen(key);
    if (in != NULL) {
        char line[MAX_PATH + SESSION_NAME_LEN + 64];
        bool at_line_start = true;
        bool dropping_line = false;
        while (fgets(line, sizeof(line), in) != NULL) {
            size_t chunk_len = strlen(line);
            bool ends_line = strchr(line, '\n') != NULL;
            if (at_line_start) {
                dropping_line = chunk_len > klen &&
                    _strnicmp(line, key, klen) == 0 && line[klen] == '\t';
            }
            if (!dropping_line && fputs(line, out) == EOF) {
                ok = false;
                break;
            }
            at_line_start = ends_line;
            if (ends_line) dropping_line = false;
        }
        if (ferror(in)) ok = false;
        if (fclose(in) != 0) ok = false;
    }
    if (ok && name[0] != '\0' && fprintf(out, "%s\t%s\n", key, name) < 0) {
        ok = false;
    }
    if (fclose(out) != 0) ok = false;

    if (!ok) {
        DeleteFile(tmp);
        return false;
    }

    DWORD flags = existing ? MOVEFILE_REPLACE_EXISTING : 0;
    if (!MoveFileEx(tmp, file, flags)) {
        DeleteFile(tmp);
        return false;
    }

    workspace_names_loaded = false;
    return true;
}

void EnterClaudeMode() {
    char root[MAX_PATH];
    if (!GetWorkspacesRoot(root)) return;
    strcpy(claude_return_dir, current_directory);
    strcpy(workspaces_root, root);
    sessions_loaded_for[0] = '\0'; // sessions may have changed since last visit
    ChangeCurrentDirectory(workspaces_root);
    claude_mode = CM_WORKSPACES;
}

void ExitClaudeMode() {
    claude_mode = CM_OFF;
    ChangeCurrentDirectory(claude_return_dir);
}

// A session id is the transcript's filename, so it is only as trustworthy as
// whatever wrote into .claude\projects. Claude Code names them as uuids;
// anything else means a hand-crafted file. This matters because the id reaches
// a "cmd /c" command line, and '&', '^', '(' and ')' -- all command separators
// to cmd -- are perfectly legal in Windows filenames
bool IsSafeSessionId(const char* id) {
    if (id[0] == '\0') return false;
    for (const char* p = id; *p != '\0'; p++) {
        bool ok = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
                  (*p >= 'A' && *p <= 'F') || *p == '-';
        if (!ok) return false;
    }
    return true;
}

// Push whatever the shell left on screen up out of the viewport, so a program
// launched out of drift starts at the top of a blank one instead of partway
// down. Newlines rather than a clear: the old output stays intact and
// scrollable. One newline per visible row is exactly the amount needed --
// reaching the bottom row costs the rows below the cursor, and the rows at
// and above it then scroll off one apiece, so only real content crosses the
// top edge and no blank filler lands in the scrollback ahead of it.
void ScrollOriginalScreen() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hOriginal, &info)) {
        return;
    }
    int remaining = info.srWindow.Bottom - info.srWindow.Top + 1;

    char newlines[128];
    while (remaining > 0) {
        int chunk = remaining < (int)sizeof(newlines) ? remaining : (int)sizeof(newlines);
        memset(newlines, '\n', (size_t)chunk);
        DWORD written;
        if (!WriteConsole(hOriginal, newlines, (DWORD)chunk, &written, NULL)) {
            return;
        }
        remaining -= chunk;
    }

    // The viewport has moved down the buffer (or the buffer scrolled under
    // it), so re-read it to find where the now-blank screen begins
    if (GetConsoleScreenBufferInfo(hOriginal, &info)) {
        COORD top = {0, info.srWindow.Top};
        SetConsoleCursorPosition(hOriginal, top);
    }
}

static bool IsPathSlash(char c) {
    return c == '\\' || c == '/';
}

// Deliberately narrower than GetFullPathName: that API turns relative input
// into an absolute path using the process cwd, which is precisely the search
// location this resolver must exclude. Drive-rooted and UNC/device paths are
// self-contained; drive-relative (C:dir) and root-relative (\dir) paths are not.
static bool IsAbsolutePathEntry(const char* path, size_t len) {
    bool drive_rooted = len >= 3 &&
        ((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && IsPathSlash(path[2]);
    bool unc_or_device = len >= 3 && IsPathSlash(path[0]) &&
        IsPathSlash(path[1]) && !IsPathSlash(path[2]);
    return drive_rooted || unc_or_device;
}

static bool FindAllowedFileInPathEntry(const char* entry, size_t len,
                                       const char* const names[],
                                       size_t name_count, char out[MAX_PATH],
                                       size_t* out_name_index) {
    if (!IsAbsolutePathEntry(entry, len) || len >= MAX_PATH) return false;

    char dir[MAX_PATH];
    memcpy(dir, entry, len);
    dir[len] = '\0';
    for (size_t i = 0; i < len; i++) {
        if (dir[i] == '"') return false;
    }

    const char* separator = len > 0 && IsPathSlash(dir[len - 1]) ? "" : "\\";
    for (size_t i = 0; i < name_count; i++) {
        char candidate[MAX_PATH];
        int written = snprintf(candidate, sizeof(candidate), "%s%s%s",
                               dir, separator, names[i]);
        if (written < 0 || written >= (int)sizeof(candidate)) continue;
        DWORD attributes = GetFileAttributes(candidate);
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            strcpy(out, candidate);
            if (out_name_index != NULL) *out_name_index = i;
            return true;
        }
    }
    return false;
}

// Resolve only an explicit allowlist of filenames from fully-qualified PATH
// entries. Empty and relative entries have current-directory semantics in
// Windows command search and must never reintroduce the workspace as an
// executable source.
static bool ResolveAllowedFileFromPath(const char* path_value,
                                       const char* const names[],
                                       size_t name_count, char out[MAX_PATH],
                                       size_t* out_name_index) {
    out[0] = '\0';
    if (out_name_index != NULL) *out_name_index = name_count;
    if (path_value == NULL) return false;

    const char* start = path_value;
    while (true) {
        const char* end = start;
        bool in_quotes = false;
        while (*end != '\0') {
            if (*end == '"') {
                in_quotes = !in_quotes;
            } else if (*end == ';' && !in_quotes) {
                break;
            }
            end++;
        }

        while (start < end && (*start == ' ' || *start == '\t')) start++;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if (end - start >= 2 && start[0] == '"' && end[-1] == '"') {
            start++;
            end--;
        }
        if (FindAllowedFileInPathEntry(start, (size_t)(end - start), names,
                                       name_count, out, out_name_index)) {
            return true;
        }

        if (*end == '\0') break;
        // end can point before the actual separator after whitespace trimming;
        // resume from the separator found by the scan, not the trimmed end.
        const char* separator = end;
        while (*separator != '\0' && *separator != ';') separator++;
        if (*separator == '\0') break;
        start = separator + 1;
    }
    return false;
}

static bool ResolveAllowedFile(const char* const names[], size_t name_count,
                               char out[MAX_PATH], size_t* out_name_index) {
    out[0] = '\0';
    if (out_name_index != NULL) *out_name_index = name_count;
    DWORD required = GetEnvironmentVariable("PATH", NULL, 0);
    if (required == 0) return false;

    char* path_value = (char*)malloc(required);
    if (path_value == NULL) return false;
    DWORD length = GetEnvironmentVariable("PATH", path_value, required);
    bool found = length > 0 && length < required &&
        ResolveAllowedFileFromPath(path_value, names, name_count, out,
                                   out_name_index);
    free(path_value);
    return found;
}

bool ResolveClaudeLauncherFromPath(const char* path_value, ClaudeLauncher* out) {
    const char* names[] = { "claude.exe", "claude.cmd" };
    size_t name_index;
    out->path[0] = '\0';
    out->kind = CLAUDE_LAUNCHER_NONE;
    if (!ResolveAllowedFileFromPath(path_value, names, 2, out->path,
                                    &name_index)) {
        return false;
    }
    out->kind = name_index == 0 ? CLAUDE_LAUNCHER_EXE : CLAUDE_LAUNCHER_CMD;
    return true;
}

bool ResolveClaudeLauncher(ClaudeLauncher* out) {
    const char* names[] = { "claude.exe", "claude.cmd" };
    size_t name_index;
    out->path[0] = '\0';
    out->kind = CLAUDE_LAUNCHER_NONE;
    if (!ResolveAllowedFile(names, 2, out->path, &name_index)) return false;
    out->kind = name_index == 0 ? CLAUDE_LAUNCHER_EXE : CLAUDE_LAUNCHER_CMD;
    return true;
}

bool ResolveVimFromPath(const char* path_value, char out[MAX_PATH]) {
    const char* names[] = { "vim.exe" };
    return ResolveAllowedFileFromPath(path_value, names, 1, out, NULL);
}

bool ResolveVim(char out[MAX_PATH]) {
    const char* names[] = { "vim.exe" };
    return ResolveAllowedFile(names, 1, out, NULL);
}

// VS Code ships a batch shim (code.cmd) in the bin\ directory its installer
// puts on PATH; code.exe is listed too for installs that expose one. Same
// absolute-PATH-entry rule as every other launcher here
bool ResolveVsCodeFromPath(const char* path_value, ClaudeLauncher* out) {
    const char* names[] = { "code.cmd", "code.exe" };
    size_t name_index;
    out->path[0] = '\0';
    out->kind = CLAUDE_LAUNCHER_NONE;
    if (!ResolveAllowedFileFromPath(path_value, names, 2, out->path,
                                    &name_index)) {
        return false;
    }
    out->kind = name_index == 0 ? CLAUDE_LAUNCHER_CMD : CLAUDE_LAUNCHER_EXE;
    return true;
}

bool ResolveVsCode(ClaudeLauncher* out) {
    const char* names[] = { "code.cmd", "code.exe" };
    size_t name_index;
    out->path[0] = '\0';
    out->kind = CLAUDE_LAUNCHER_NONE;
    if (!ResolveAllowedFile(names, 2, out->path, &name_index)) return false;
    out->kind = name_index == 0 ? CLAUDE_LAUNCHER_CMD : CLAUDE_LAUNCHER_EXE;
    return true;
}

// Naming devenv.exe outright also sidesteps PATHEXT, where the devenv.com
// sitting beside it outranks the .exe and would give the console variant
bool ResolveDevenvFromPath(const char* path_value, ClaudeLauncher* out) {
    const char* names[] = { "devenv.exe" };
    out->path[0] = '\0';
    out->kind = CLAUDE_LAUNCHER_NONE;
    if (!ResolveAllowedFileFromPath(path_value, names, 1, out->path, NULL)) {
        return false;
    }
    out->kind = CLAUDE_LAUNCHER_EXE;
    return true;
}

bool ResolveDevenv(ClaudeLauncher* out) {
    const char* names[] = { "devenv.exe" };
    out->path[0] = '\0';
    out->kind = CLAUDE_LAUNCHER_NONE;
    if (!ResolveAllowedFile(names, 1, out->path, NULL)) return false;
    out->kind = CLAUDE_LAUNCHER_EXE;
    return true;
}

// The Visual Studio Version Selector, at the fixed shared-component location
// every Visual Studio since 2010 installs it to. It reads the solution's own
// "# Visual Studio Version" header and starts the matching install, which is
// what a double-click in Explorer does -- so a machine with several Visual
// Studios opens each solution in the one it was written for. A single devenv
// resolved from PATH cannot do that: it is one fixed install for every
// solution, which is why it is the fallback rather than the first choice.
//
// Built from a well-known directory rather than searched for, exactly like the
// System32 cmd.exe resolution in BuildLauncherProcessSpec: PATH never
// participates, so nothing planted on it can answer for Visual Studio.
bool ResolveVisualStudioLauncher(ClaudeLauncher* out) {
    out->path[0] = '\0';
    out->kind = CLAUDE_LAUNCHER_NONE;

    char common[MAX_PATH];
    DWORD length = GetEnvironmentVariable("CommonProgramFiles(x86)", common, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        // 32-bit Windows has no (x86) split
        length = GetEnvironmentVariable("CommonProgramFiles", common, MAX_PATH);
    }
    if (length == 0 || length >= MAX_PATH) return false;

    if (snprintf(out->path, MAX_PATH, "%s\\Microsoft Shared\\MSEnv\\VSLauncher.exe",
                 common) >= MAX_PATH) {
        out->path[0] = '\0';
        return false;
    }
    DWORD attributes = GetFileAttributes(out->path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        out->path[0] = '\0';
        return false;
    }
    out->kind = CLAUDE_LAUNCHER_EXE;
    return true;
}

// Shared by every launcher drift spawns. `argument` is the already-formatted
// argument tail, or NULL for none -- the caller owns whatever quoting its own
// argument needs, this owns the launcher's quoting and the cmd wrapper.
bool BuildLauncherProcessSpec(const ClaudeLauncher* launcher, const char* argument,
                              ClaudeProcessSpec* out) {
    out->application[0] = '\0';
    out->command[0] = '\0';
    if (launcher == NULL || launcher->path[0] == '\0') {
        return false;
    }

    int written;
    if (launcher->kind == CLAUDE_LAUNCHER_EXE) {
        strcpy(out->application, launcher->path);
        written = argument != NULL
            ? snprintf(out->command, sizeof(out->command),
                       "\"%s\" %s", launcher->path, argument)
            : snprintf(out->command, sizeof(out->command),
                       "\"%s\"", launcher->path);
    } else if (launcher->kind == CLAUDE_LAUNCHER_CMD) {
        // Percent expansion happens even inside cmd's double quotes. Reject a
        // launcher path containing '%' rather than let its absolute spelling
        // be rewritten into another command. /d disables AutoRun registry
        // hooks and /v:off prevents delayed !variable! expansion.
        if (strchr(launcher->path, '%') != NULL) return false;
        // The argument rides the same command line, so it is rewritten by the
        // same expansion. '%' is legal in a Windows filename, which makes this
        // reachable from an ordinary folder rather than only a crafted one.
        // '&', '^', '(' and ')' need no rejection: they are inert inside the
        // double quotes the caller wraps a path in
        if (argument != NULL && strchr(argument, '%') != NULL) return false;
        char system_dir[MAX_PATH];
        DWORD length = GetSystemDirectory(system_dir, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return false;
        written = snprintf(out->application, sizeof(out->application),
                           "%s\\cmd.exe", system_dir);
        if (written < 0 || written >= (int)sizeof(out->application)) return false;
        DWORD attributes = GetFileAttributes(out->application);
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;

        written = argument != NULL
            ? snprintf(out->command, sizeof(out->command),
                       "\"%s\" /d /v:off /s /c \"\"%s\" %s\"",
                       out->application, launcher->path, argument)
            : snprintf(out->command, sizeof(out->command),
                       "\"%s\" /d /v:off /s /c \"\"%s\"\"",
                       out->application, launcher->path);
    } else {
        return false;
    }
    return written >= 0 && written < (int)sizeof(out->command);
}

// Claude's own spelling of the above. Kept as the entry point the launcher is
// built through so the session-id validation stays impossible to bypass, and
// so the command lines it produces are byte-for-byte what they were before the
// generalization -- which is what tests/claude_launcher_test.c pins.
bool BuildClaudeProcessSpec(const ClaudeLauncher* launcher, const char* session_id,
                            ClaudeProcessSpec* out) {
    out->application[0] = '\0';
    out->command[0] = '\0';
    if (session_id == NULL) {
        return BuildLauncherProcessSpec(launcher, NULL, out);
    }
    if (!IsSafeSessionId(session_id)) return false;
    // The id field's own capacity plus the fixed "--resume " and its two quotes
    char argument[sizeof(sessions[0].id) + 16];
    if (snprintf(argument, sizeof(argument), "--resume \"%s\"", session_id) >=
        (int)sizeof(argument)) {
        return false;
    }
    return BuildLauncherProcessSpec(launcher, argument, out);
}

// Hand a GUI program the request and return immediately. Every other launch in
// drift (vim, claude) is a console program that takes over the terminal, so it
// swaps to the original screen buffer and blocks; an editor with its own window
// must do neither, or drift would sit frozen behind it until it was closed.
// CREATE_NO_WINDOW matters for the cmd shims: without it the intermediate
// cmd.exe gets a console and paints over the alternate buffer drift is drawing.
bool SpawnDetached(ClaudeProcessSpec* spec, const char* working_dir) {
    if (spec == NULL || spec->application[0] == '\0') return false;
    STARTUPINFO si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcess(spec->application, spec->command, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, working_dir, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// Suspend the TUI, run claude anchored in the workspace, resume when it exits.
// Native launch first resolves an absolute executable from absolute PATH
// entries, so neither Drift's launch directory nor the workspace participates
// implicitly. session_id NULL starts a new session.
// Returns 0 when drift should exit because a wrapper script is taking over
// the launch (see below).
int LaunchClaudeIn(const char* anchor, const char* session_id) {
    // Handoff mode for Wine/dev: a Windows process can't spawn the host's
    // claude, so when DRIFT_LAUNCH_FILE is set we write the request there
    // and exit; the wrapper (run.sh) launches claude and restarts drift
    char launch_file[MAX_PATH];
    DWORD lf = GetEnvironmentVariable("DRIFT_LAUNCH_FILE", launch_file, MAX_PATH);
    if (lf > 0 && lf < MAX_PATH) {
        FILE* f = fopen(launch_file, "wb");
        if (f != NULL) {
            // Deliberately unquoted: run.sh word-splits this line into argv,
            // so quotes would arrive as literal characters. IsSafeSessionId is
            // what keeps this line benign
            if (session_id != NULL) {
                fprintf(f, "%s\n--resume %s\n", anchor, session_id);
            } else {
                fprintf(f, "%s\n\n", anchor);
            }
            fclose(f);
            return 0; // exit drift; the wrapper takes over
        }
        return 1;
    }

    ClaudeLauncher launcher;
    ClaudeProcessSpec process;
    if (!ResolveClaudeLauncher(&launcher) ||
        !BuildClaudeProcessSpec(&launcher, session_id, &process)) {
        return 1;
    }

    SetConsoleActiveScreenBuffer(hOriginal);
    SetConsoleMode(hIn, original_console_mode);
    ScrollOriginalScreen();

    STARTUPINFO si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (CreateProcess(process.application, process.command, NULL, NULL, FALSE, 0,
                      NULL, anchor, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    SetConsoleMode(hIn, (original_console_mode & ~ENABLE_PROCESSED_INPUT) | ENABLE_WINDOW_INPUT);
    SetConsoleActiveScreenBuffer(hAlt);
    FlushConsoleInputBuffer(hIn); // discard keys typed into the dead moment

    // The session list is stale the moment claude exits: the session that
    // just ran (or was just created) belongs at the top
    sessions_loaded_for[0] = '\0';
    LoadSessionsFor(anchor);
    session_selected = 0;
    session_top = 0;
    return 1;
}

void EnterSessionsView() {
    char* name = current_directory_files[selected_row].cFileName;
    if (snprintf(claude_workspace, MAX_PATH, "%s\\%s", workspaces_root, name) >= MAX_PATH) {
        return;
    }
    snprintf(claude_workspace_name, MAX_PATH, "%s", name);
    LoadSessionsFor(claude_workspace);
    session_selected = 0;
    session_top = 0;
    claude_mode = CM_SESSIONS;
}

// Claude Code stores each project's session transcripts in a folder named
// after the launch directory with special characters flattened to dashes.
// The scheme is undocumented, so LoadSessions tries both dot variants.
void EncodeProjectPath(const char* path, char* out, bool keep_dots) {
    int n = 0;
    for (int i = 0; path[i] != '\0' && n < MAX_PATH - 1; i++) {
        unsigned char ch = (unsigned char)path[i];
        bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || (keep_dots && ch == '.');
        out[n++] = keep ? (char)ch : '-';
    }
    out[n] = '\0';
}

void LoadSessionsFor(const char* anchor) {
    if (_stricmp(anchor, sessions_loaded_for) == 0) return; // already loaded
    snprintf(sessions_loaded_for, MAX_PATH, "%s", anchor);
    session_count = 0;

    char claude_root[MAX_PATH];
    DWORD len = GetEnvironmentVariable("DRIFT_CLAUDE_DIR", claude_root, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        char home[MAX_PATH];
        len = GetEnvironmentVariable("USERPROFILE", home, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return;
        if (snprintf(claude_root, MAX_PATH, "%s\\.claude", home) >= MAX_PATH) return;
    }

    // Try both dot variants of the encoding, and -- for Wine, where the host
    // claude saw the anchor without its drive letter -- the drive-less forms
    char enc[MAX_PATH];
    char dir[MAX_PATH];
    const char* sources[4];
    bool dots[4];
    int candidates = 2;
    sources[0] = anchor; dots[0] = false;
    sources[1] = anchor; dots[1] = true;
    if (anchor[0] != '\0' && anchor[1] == ':') {
        sources[2] = anchor + 2; dots[2] = false;
        sources[3] = anchor + 2; dots[3] = true;
        candidates = 4;
    }
    bool found = false;
    for (int i = 0; i < candidates && !found; i++) {
        EncodeProjectPath(sources[i], enc, dots[i]);
        if (snprintf(dir, MAX_PATH, "%s\\projects\\%s", claude_root, enc) >= MAX_PATH) return;
        found = GetFileAttributes(dir) != INVALID_FILE_ATTRIBUTES;
    }
    if (!found) return; // no sessions yet

    char search[MAX_PATH];
    if (snprintf(search, MAX_PATH, "%s\\*.jsonl", dir) >= MAX_PATH) return;

    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        SessionEntry* s = &sessions[session_count];
        if (snprintf(s->path, MAX_PATH, "%s\\%s", dir, fd.cFileName) >= MAX_PATH) continue;
        // A name too long for the field would keep only its first 47 bytes,
        // and losing the ".jsonl" that way leaves something the check below
        // still reads as a valid id -- hex and dashes, any length -- so the
        // row would be listed and resumed against an id that does not exist
        if (snprintf(s->id, sizeof(s->id), "%s", fd.cFileName) >= (int)sizeof(s->id)) continue;
        char* dot = strrchr(s->id, '.');
        if (dot != NULL) *dot = '\0';
        // Not a uuid: a file someone crafted to smuggle shell metacharacters
        // into the launch. It cannot be resumed, so don't list it (delete it
        // from the browser)
        if (!IsSafeSessionId(s->id)) continue;
        s->mtime = fd.ftLastWriteTime;
        ReadSessionName(s->path, s->name, sizeof(s->name));
        session_count++;
    } while (FindNextFile(hFind, &fd) && session_count < MAX_SESSIONS);
    FindClose(hFind);

    qsort(sessions, session_count, sizeof(SessionEntry), CompareSessions);
    ApplySessionNames(anchor);
}

// Drift-side session names live in .drift\session-names as tab-separated
// "folder<TAB>id<TAB>name" lines -- claude's own files are never modified
void ApplySessionNames(const char* anchor) {
    char file[MAX_PATH];
    if (!GetNameFile(file, SESSION_NAMES_FILE)) return;
    FILE* f = fopen(file, "rb");
    if (f == NULL) return;

    const char* folder = AnchorFolder(anchor);
    size_t flen = strlen(folder);

    char line[MAX_PATH + SESSION_NAME_LEN + 64];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (_strnicmp(line, folder, flen) != 0 || line[flen] != '\t') continue;
        char* id = line + flen + 1;
        char* tab = strchr(id, '\t');
        if (tab == NULL) continue;
        *tab = '\0';
        char* name = tab + 1;
        size_t nl = strlen(name);
        while (nl > 0 && (name[nl - 1] == '\n' || name[nl - 1] == '\r')) {
            name[--nl] = '\0';
        }
        if (nl == 0) continue;
        for (int i = 0; i < session_count; i++) {
            if (_stricmp(sessions[i].id, id) == 0) {
                snprintf(sessions[i].name, SESSION_NAME_LEN, "%s", name);
                break;
            }
        }
    }
    fclose(f);
}

// Replaces (or, with an empty name, drops) one session's name
bool SetSessionName(const char* anchor, const char* id, const char* name) {
    char key[MAX_PATH];
    int written = snprintf(key, sizeof(key), "%s\t%s", AnchorFolder(anchor), id);
    if (written < 0 || written >= (int)sizeof(key)) return false;
    return SetNameEntry(SESSION_NAMES_FILE, key, name);
}

void LoadWorkspaceNames() {
    free(workspace_names);
    workspace_names = NULL;
    workspace_names_loaded = true;

    char file[MAX_PATH];
    if (!GetNameFile(file, WORKSPACE_NAMES_FILE)) return;
    FILE* f = fopen(file, "rb");
    if (f == NULL) return;

    // Measure first, so the whole file is read however long it has grown
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return;
    }

    char* buf = (char*)malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f); // names fall back to folder ids rather than being wrong
        return;
    }
    size_t len = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[len] = '\0';
    workspace_names = buf;
}

// A workspace's shown name: its row in .drift\workspace-names if it has one,
// otherwise the folder name itself. The folder is never renamed -- that path
// is the key Claude files its sessions under, so renaming it would orphan
// them -- so this name is a pure drift-side overlay, exactly like the session
// names above.
void WorkspaceDisplayName(const char* folder, char* out, size_t out_size) {
    snprintf(out, out_size, "%s", folder); // default: the folder's own name
    if (!workspace_names_loaded) LoadWorkspaceNames();
    if (workspace_names == NULL) return; // no overlay file, or it could not be read

    size_t flen = strlen(folder);
    const char* p = workspace_names;
    while (*p != '\0') {
        const char* eol = strchr(p, '\n');
        size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
        if (_strnicmp(p, folder, flen) == 0 && flen < len && p[flen] == '\t') {
            const char* name = p + flen + 1;
            size_t nl = len - flen - 1;
            while (nl > 0 && name[nl - 1] == '\r') nl--;
            if (nl > 0) snprintf(out, out_size, "%.*s", (int)nl, name);
            return;
        }
        if (eol == NULL) break;
        p = eol + 1;
    }
}

// Store (or, with an empty name, clear) a workspace's display-name overlay
bool SetWorkspaceName(const char* folder, const char* display) {
    // An empty name drops the row, reverting to the folder name
    return SetNameEntry(WORKSPACE_NAMES_FILE, folder, display);
}

// True if some workspace other than except_folder already presents `name` as
// its effective (shown) name -- overlay or folder name alike. Because folder
// ids are opaque and always unique, the shown name is the only namespace the
// user touches, so this single check catches every collision they could see.
// Case-insensitive, to match the Windows filesystem.
bool WorkspaceNameTaken(const char* name, const char* except_folder) {
    char pattern[MAX_PATH];
    if (snprintf(pattern, MAX_PATH, "%s\\*", workspaces_root) >= MAX_PATH) return false;
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool taken = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (except_folder != NULL && _stricmp(fd.cFileName, except_folder) == 0) continue;
        char disp[MAX_PATH];
        WorkspaceDisplayName(fd.cFileName, disp, sizeof(disp));
        if (_stricmp(disp, name) == 0) {
            taken = true;
            break;
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
    return taken;
}

// Report something and wait for a keypress: the banner is painted once and the
// next DrawScreen wipes it, so it needs an acknowledgement to be seen at all
void NotifyAndWait(const char* text) {
    ShowStatusBanner(text);
    INPUT_RECORD ir;
    DWORD ev;
    while (ReadConsoleInput(hIn, &ir, 1, &ev) &&
           (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown)) {
    }
}

void NotifyNameTaken(const char* name) {
    char msg[160];
    snprintf(msg, sizeof(msg), "\"%s\" is already in use -- press a key", name);
    NotifyAndWait(msg);
}

// 'r' in the session list: edit the drift-side display title in the same
// input popup used for file creation, pre-filled with the current title.
// Enter with an emptied field reverts to the parsed first-prompt title.
void HandleRenameSession() {
    if (session_count == 0) return;
    SessionEntry* sel = &sessions[session_selected];

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

    int popup_h = 3;
    int popup_w = CREATE_POPUP_WIDTH;
    if (popup_w > screen_width - 2) popup_w = screen_width - 2;
    if (popup_w < 14 || screen_height < popup_h) return;

    // Allocated at the maximum width, so a resize only has to recompute the
    // geometry inside the loop and never reallocate: popup_w is
    // CREATE_POPUP_WIDTH clamped down to the screen, never above it
    CHAR_INFO* popup_buffer = (CHAR_INFO*)malloc(CREATE_POPUP_WIDTH * popup_h * sizeof(CHAR_INFO));
    if (popup_buffer == NULL) return;

    char name[MAX_PATH];
    // Fixed for the life of the popup even if the window is resized, so text
    // already typed can never exceed what the field still accepts
    int max_len = popup_w - 10;
    // The field holds max_len characters but sel->name holds up to
    // SESSION_NAME_LEN, and titles parsed from a first message routinely run
    // longer. Keep what was shown so confirming an untouched field can be
    // told apart from an edit, rather than saving the visible prefix over the
    // real title
    char prefill[MAX_PATH];
    // Cut on a character boundary: a byte-precision "%.*s" would leave a
    // half-written character at the end, which an edit would then save into
    // the names file
    int keep = Utf8Prefix(sel->name, max_len, (int)sizeof(prefill) - 1);
    memcpy(prefill, sel->name, keep);
    prefill[keep] = '\0';
    snprintf(name, sizeof(name), "%s", prefill);
    int pos = (int)strlen(name);
    bool cancelled = false;

    CONSOLE_CURSOR_INFO cursor_info = { 25, TRUE };
    SetConsoleCursorInfo(hAlt, &cursor_info);

    while (1) {
        // Re-measured every pass rather than once before the loop: the console
        // reflows on a resize, and drawing at the original geometry would put
        // the field and its caret somewhere other than where they appear
        CONSOLE_SCREEN_BUFFER_INFO cur;
        if (!GetConsoleScreenBufferInfo(hAlt, &cur)) {
            cancelled = true;
            break;
        }
        int cur_w = cur.srWindow.Right - cur.srWindow.Left + 1;
        int cur_h = cur.srWindow.Bottom - cur.srWindow.Top + 1;
        popup_w = CREATE_POPUP_WIDTH;
        if (popup_w > cur_w - 2) popup_w = cur_w - 2;
        if (popup_w < 14 || cur_h < popup_h) {
            cancelled = true; // shrunk too far to show the field
            break;
        }
        int start_col = cur.srWindow.Left + (cur_w - popup_w) / 2;
        int start_row = cur.srWindow.Top + (cur_h - popup_h) / 2;

        DrawCreatePopup(popup_w, name, NULL, popup_buffer, CP_UTF8);
        COORD buffer_size = { (SHORT)popup_w, (SHORT)popup_h };
        COORD origin = { 0, 0 };
        SMALL_RECT region = { (SHORT)start_col, (SHORT)start_row,
                              (SHORT)(start_col + popup_w - 1), (SHORT)(start_row + popup_h - 1) };
        WriteConsoleOutputW(hAlt, popup_buffer, buffer_size, origin, &region);
        COORD cursor_pos = { (SHORT)(start_col + 8 + pos), (SHORT)(start_row + 1) };
        SetConsoleCursorPosition(hAlt, cursor_pos);

        INPUT_RECORD input;
        DWORD events;
        if (!ReadConsoleInput(hIn, &input, 1, &events)) {
            cancelled = true;
            break;
        }
        if (input.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            DrawScreen(); // restore what the popup sits on top of
            continue;     // the loop re-measures and repaints above
        }
        if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) continue;

        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
        char ch = input.Event.KeyEvent.uChar.AsciiChar;
        if (vk == VK_RETURN) break;
        if (vk == VK_ESCAPE) {
            cancelled = true;
            break;
        }
        if (vk == VK_BACK && pos > 0) {
            pos--;
            name[pos] = '\0';
        } else if (ch >= 32 && ch < 127 && pos < max_len) {
            name[pos] = ch;
            pos++;
            name[pos] = '\0';
        }
    }
    free(popup_buffer);
    cursor_info.bVisible = FALSE;
    SetConsoleCursorInfo(hAlt, &cursor_info);
    if (cancelled) return;
    // Nothing typed: leave the stored name alone. Emptying the field is still
    // an edit, and still clears the override below
    if (strcmp(name, prefill) == 0) return;

    if (!SetSessionName(claude_workspace, sel->id, name)) {
        NotifyAndWait("Session name was not saved -- press a key");
        return;
    }
    if (name[0] != '\0') {
        snprintf(sel->name, sizeof(sel->name), "%s", name);
    } else {
        ReadSessionName(sel->path, sel->name, sizeof(sel->name));
    }
}

// 'r' in the workspace list: edit the workspace's display name in the same
// popup, pre-filled with the current name. Enter on an emptied field (or the
// folder's own name) clears the overlay and reverts to the folder name. Only
// the .driftworkspace-names row changes -- the folder and its sessions are not.
void HandleRenameWorkspace() {
    if (current_directory_file_count == 0) return;
    WIN32_FIND_DATA* sel = &current_directory_files[selected_row];
    if (!IsDirectory(sel)) return;
    char folder[MAX_PATH];
    snprintf(folder, sizeof(folder), "%s", sel->cFileName);

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

    int popup_h = 3;
    int popup_w = CREATE_POPUP_WIDTH;
    if (popup_w > screen_width - 2) popup_w = screen_width - 2;
    if (popup_w < 14 || screen_height < popup_h) return;

    // Allocated at the maximum width, so a resize only has to recompute the
    // geometry inside the loop and never reallocate: popup_w is
    // CREATE_POPUP_WIDTH clamped down to the screen, never above it
    CHAR_INFO* popup_buffer = (CHAR_INFO*)malloc(CREATE_POPUP_WIDTH * popup_h * sizeof(CHAR_INFO));
    if (popup_buffer == NULL) return;

    char name[MAX_PATH];
    // Fixed for the life of the popup even if the window is resized, so text
    // already typed can never exceed what the field still accepts
    int max_len = popup_w - 10;
    char current[MAX_PATH];
    WorkspaceDisplayName(folder, current, sizeof(current));
    snprintf(name, sizeof(name), "%.*s", max_len, current);
    int pos = (int)strlen(name);
    bool cancelled = false;

    CONSOLE_CURSOR_INFO cursor_info = { 25, TRUE };
    SetConsoleCursorInfo(hAlt, &cursor_info);

    while (1) {
        // Re-measured every pass rather than once before the loop: the console
        // reflows on a resize, and drawing at the original geometry would put
        // the field and its caret somewhere other than where they appear
        CONSOLE_SCREEN_BUFFER_INFO cur;
        if (!GetConsoleScreenBufferInfo(hAlt, &cur)) {
            cancelled = true;
            break;
        }
        int cur_w = cur.srWindow.Right - cur.srWindow.Left + 1;
        int cur_h = cur.srWindow.Bottom - cur.srWindow.Top + 1;
        popup_w = CREATE_POPUP_WIDTH;
        if (popup_w > cur_w - 2) popup_w = cur_w - 2;
        if (popup_w < 14 || cur_h < popup_h) {
            cancelled = true; // shrunk too far to show the field
            break;
        }
        int start_col = cur.srWindow.Left + (cur_w - popup_w) / 2;
        int start_row = cur.srWindow.Top + (cur_h - popup_h) / 2;

        DrawCreatePopup(popup_w, name, NULL, popup_buffer, CP_ACP);
        COORD buffer_size = { (SHORT)popup_w, (SHORT)popup_h };
        COORD origin = { 0, 0 };
        SMALL_RECT region = { (SHORT)start_col, (SHORT)start_row,
                              (SHORT)(start_col + popup_w - 1), (SHORT)(start_row + popup_h - 1) };
        WriteConsoleOutputW(hAlt, popup_buffer, buffer_size, origin, &region);
        COORD cursor_pos = { (SHORT)(start_col + 8 + pos), (SHORT)(start_row + 1) };
        SetConsoleCursorPosition(hAlt, cursor_pos);

        INPUT_RECORD input;
        DWORD events;
        if (!ReadConsoleInput(hIn, &input, 1, &events)) {
            cancelled = true;
            break;
        }
        if (input.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            DrawScreen(); // restore what the popup sits on top of
            continue;     // the loop re-measures and repaints above
        }
        if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) continue;

        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
        char ch = input.Event.KeyEvent.uChar.AsciiChar;
        if (vk == VK_RETURN) break;
        if (vk == VK_ESCAPE) {
            cancelled = true;
            break;
        }
        if (vk == VK_BACK && pos > 0) {
            pos--;
            name[pos] = '\0';
        } else if (ch >= 32 && ch < 127 && pos < max_len) {
            name[pos] = ch;
            pos++;
            name[pos] = '\0';
        }
    }
    free(popup_buffer);
    cursor_info.bVisible = FALSE;
    SetConsoleCursorInfo(hAlt, &cursor_info);
    if (cancelled) return;

    // A name equal to the folder's own name needs no overlay -- clear it
    if (_stricmp(name, folder) == 0) name[0] = '\0';
    if (name[0] != '\0' && WorkspaceNameTaken(name, folder)) {
        NotifyNameTaken(name);
        return;
    }
    if (!SetWorkspaceName(folder, name)) {
        NotifyAndWait("Workspace name was not saved -- press a key");
    }
}

// 'd' in the session list: delete the transcript (recycle bin) after a
// confirmation popup
void HandleDeleteSession() {
    if (session_count == 0) return;
    SessionEntry* sel = &sessions[session_selected];

    // Keep the destructive key armed only while a complete prompt has been
    // painted for the current window. Console reflow wipes one-shot overlays;
    // the nested input loop owns resize events, so the main loop cannot repair
    // the screen until this handler returns.
    bool repaint = true;
    while (1) {
        if (repaint) {
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
            int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
            int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

            int popup_w = 44;
            const int popup_h = 7;
            if (popup_w > screen_width) popup_w = screen_width;
            // Below this width even the useful confirmation labels no longer
            // fit. Cancel rather than accept Y behind missing content.
            if (popup_w < 20 || screen_height < popup_h) return;

            CHAR_INFO* popup = (CHAR_INFO*)malloc(
                popup_w * popup_h * sizeof(CHAR_INFO));
            if (popup == NULL) return;

            for (int i = 0; i < popup_w * popup_h; i++) {
                popup[i].Char.UnicodeChar = L' ';
                popup[i].Attributes = white;
            }
            popup[0].Char.UnicodeChar = BOX_TOP_LEFT;
            popup[popup_w - 1].Char.UnicodeChar = BOX_TOP_RIGHT;
            int bottom = (popup_h - 1) * popup_w;
            popup[bottom].Char.UnicodeChar = BOX_BOTTOM_LEFT;
            popup[bottom + popup_w - 1].Char.UnicodeChar = BOX_BOTTOM_RIGHT;
            for (int c = 1; c < popup_w - 1; c++) {
                popup[c].Char.UnicodeChar = BOX_HORIZONTAL;
                popup[bottom + c].Char.UnicodeChar = BOX_HORIZONTAL;
            }
            for (int r = 1; r < popup_h - 1; r++) {
                popup[r * popup_w].Char.UnicodeChar = BOX_VERTICAL;
                popup[r * popup_w + popup_w - 1].Char.UnicodeChar = BOX_VERTICAL;
            }
            WriteToBuffer(popup, popup_w, 1, 2, "Delete session?", red);
            WriteToBufferCP(popup, popup_w, 3, 2, sel->name, white, CP_UTF8);
            WriteToBuffer(popup, popup_w, 5, 2,
                          "[Y] Yes - Recycle Bin  [N] No", white);

            int start_col = info.srWindow.Left + (screen_width - popup_w) / 2;
            int start_row = info.srWindow.Top + (screen_height - popup_h) / 2;
            COORD buffer_size = { (SHORT)popup_w, (SHORT)popup_h };
            COORD origin = { 0, 0 };
            SMALL_RECT requested = {
                (SHORT)start_col,
                (SHORT)start_row,
                (SHORT)(start_col + popup_w - 1),
                (SHORT)(start_row + popup_h - 1)
            };
            SMALL_RECT written = requested;
            BOOL output_ok = WriteConsoleOutputW(
                hAlt, popup, buffer_size, origin, &written);
            free(popup);

            // WriteConsoleOutputW reports its actual (possibly clipped)
            // destination through written. Partial output is not a visible
            // confirmation and must fail closed just like a write failure.
            if (!output_ok ||
                written.Left != requested.Left ||
                written.Top != requested.Top ||
                written.Right != requested.Right ||
                written.Bottom != requested.Bottom) {
                return;
            }
            repaint = false;
        }

        INPUT_RECORD input;
        DWORD events;
        if (!ReadConsoleInput(hIn, &input, 1, &events)) break;
        if (input.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            DrawScreen();
            repaint = true;
            continue;
        }
        if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) continue;

        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
        if (vk == 'Y') {
            bool name_cleanup_failed = false;
            char from[MAX_PATH + 2];
            int len = snprintf(from, MAX_PATH, "%s", sel->path);
            if (len > 0 && len < MAX_PATH) {
                from[len + 1] = '\0'; // double-null for SHFileOperation

                SHFILEOPSTRUCT op = {0};
                op.wFunc = FO_DELETE;
                op.pFrom = from;
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_WANTNUKEWARNING;

                if (SHFileOperation(&op) == 0 && !op.fAnyOperationsAborted) {
                    // The transcript stays deleted even if this optional
                    // metadata cleanup fails; report that partial result below.
                    name_cleanup_failed =
                        !SetSessionName(claude_workspace, sel->id, "");
                }
                FlushConsoleInputBuffer(hIn);

                sessions_loaded_for[0] = '\0';
                LoadSessionsFor(claude_workspace);
                if (session_selected >= session_count) {
                    session_selected = session_count > 0 ? session_count - 1 : 0;
                }
                if (name_cleanup_failed) {
                    NotifyAndWait("Session deleted; saved name was not removed -- press a key");
                }
            }
            break;
        }
        if (vk == 'N' || vk == VK_ESCAPE) break;
    }
}

int CompareSessions(const void* a, const void* b) {
    const SessionEntry* sa = (const SessionEntry*)a;
    const SessionEntry* sb = (const SessionEntry*)b;
    return CompareFileTime(&sb->mtime, &sa->mtime); // newest first
}

// Shallow, read-only scan of a transcript for the first real user message.
// The format is undocumented; on any surprise the title just stays generic.
void ReadSessionName(const char* path, char* out, int out_size) {
    snprintf(out, out_size, "(unnamed)");

    FILE* f = fopen(path, "rb");
    if (f == NULL) return;

    char line[8192];
    int scanned = 0;
    while (scanned < 50 && fgets(line, sizeof(line), f) != NULL) {
        size_t line_len = strlen(line);
        bool complete = line_len > 0 && line[line_len - 1] == '\n';

        if (strstr(line, "\"type\":\"user\"") != NULL) {
            char* p = strstr(line, "\"content\":\"");
            int skip = 11;
            if (p == NULL) {
                p = strstr(line, "\"text\":\"");
                skip = 8;
            }
            if (p != NULL) {
                p += skip;
                int n = 0;
                while (*p != '\0' && *p != '"' && n < out_size - 1) {
                    if (*p == '\\') {
                        p++;
                        if (*p == 'n' || *p == 't') {
                            out[n++] = ' ';
                            p++;
                        } else if (*p == 'u') {
                            out[n++] = '?';
                            p++;
                            for (int k = 0; k < 4 && *p != '\0'; k++) p++;
                        } else if (*p != '\0') {
                            out[n++] = *p++;
                        }
                    } else {
                        out[n++] = *p++;
                    }
                }
                out[n] = '\0';
                // Skip command wrappers and the session-start caveat notice
                if (n > 0 && out[0] != '<' && strncmp(out, "Caveat", 6) != 0) {
                    fclose(f);
                    return;
                }
                snprintf(out, out_size, "(unnamed)");
            }
        }

        if (!complete) { // consume the rest of an over-long line
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
        }
        scanned++;
    }
    fclose(f);
}

void FormatAge(FILETIME ft, char* out, int out_size) {
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULONGLONG a = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ULONGLONG b = ((ULONGLONG)now_ft.dwHighDateTime << 32) | now_ft.dwLowDateTime;
    ULONGLONG s = b > a ? (b - a) / 10000000ULL : 0;
    if (s < 60) snprintf(out, out_size, "now");
    else if (s < 3600) snprintf(out, out_size, "%um", (unsigned)(s / 60));
    else if (s < 86400) snprintf(out, out_size, "%uh", (unsigned)(s / 3600));
    else if (s < 86400ULL * 14) snprintf(out, out_size, "%ud", (unsigned)(s / 86400));
    else snprintf(out, out_size, "%uw", (unsigned)(s / (86400ULL * 7)));
}

// Session view: workspaces on the left for context, sessions in the middle,
// details of the selected session in the third pane
void DrawSessionsPanes(CHAR_INFO* buffer, int width, int height, int divider2, int list_height) {
    wchar_t wname[MAX_PATH];

    // Left pane: workspace list in the normal drift folder color, the open
    // one in white so it still stands out
    for (int i = 0; i < list_height && top_row + i < current_directory_file_count; i++) {
        WIN32_FIND_DATA* w = &current_directory_files[top_row + i];
        WORD c = strcmp(w->cFileName, claude_workspace_name) == 0 ? white : blue;
        if (!IsDirectory(w)) c = gray;
        if (IsDirectory(w)) {
            char disp[MAX_PATH];
            WorkspaceDisplayName(w->cFileName, disp, sizeof(disp));
            AnsiToWide(disp, wname, MAX_PATH);
        } else {
            AnsiToWide(w->cFileName, wname, MAX_PATH);
        }
        int len = (int)wcslen(wname);
        for (int col = 0; col < len && col < COLUMN_DIVIDER_POSITION - 2; col++) {
            int index = (i + 2) * width + col;
            buffer[index].Char.UnicodeChar = wname[col];
            buffer[index].Attributes = c;
        }
    }

    // Middle pane: sessions, newest first
    if (session_count == 0) {
        WriteToBuffer(buffer, width, 2, COLUMN_DIVIDER_POSITION + 2, "(no sessions)", gray);
    }
    for (int i = 0; i < list_height && session_top + i < session_count; i++) {
        int idx = session_top + i;
        bool sel = (idx == session_selected);
        WORD attr = white;
        if (sel) {
            attr = bar_background; // black text on the bar
            for (int col = COLUMN_DIVIDER_POSITION + 1; col < divider2; col++) {
                buffer[(i + 2) * width + col].Attributes = bar_background;
            }
        }
        char age[16];
        FormatAge(sessions[idx].mtime, age, sizeof(age));
        char row_text[160];
        snprintf(row_text, sizeof(row_text), "%-4s %s", age, sessions[idx].name);
        // The name came out of a .jsonl transcript, which is UTF-8. The age
        // prefix is ASCII, which both encodings agree on
        BytesToWide(row_text, wname, MAX_PATH, CP_UTF8);
        int len = (int)wcslen(wname);
        for (int col = 0; col < len && COLUMN_DIVIDER_POSITION + 2 + col < divider2; col++) {
            int index = (i + 2) * width + COLUMN_DIVIDER_POSITION + 2 + col;
            buffer[index].Char.UnicodeChar = wname[col];
            buffer[index].Attributes = attr;
        }
    }

    // Third pane: the keymap, then details of the selected session.
    //
    // The keymap leads. In the workspace view it lives top-left, but here that
    // pane holds the workspace list, so this is the only place it fits -- and
    // on the bottom line it went unread. It also has to be what a workspace
    // with no sessions shows, since "n  new session" is the answer to the only
    // question an empty session list raises. Each verb names its object: "new"
    // and "delete" alone read as being about the workspace.
    if (divider2 >= width) return;
    int col_start = divider2 + 2;
    int row = 2;
    if (row < height) WriteToBuffer(buffer, width, row++, col_start, "Enter  resume session", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, col_start, "n      new session", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, col_start, "r      rename session", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, col_start, "d      delete session", gray);
    row++;

    char ws_disp[MAX_PATH];
    WorkspaceDisplayName(claude_workspace_name, ws_disp, sizeof(ws_disp));
    if (row < height) WriteToBuffer(buffer, width, row++, col_start, ws_disp, yellow);
    char meta[64];
    snprintf(meta, sizeof(meta), "%d session%s", session_count, session_count == 1 ? "" : "s");
    if (row < height) WriteToBuffer(buffer, width, row++, col_start, meta, white);

    if (session_count == 0) return;
    SessionEntry* sel = &sessions[session_selected];
    row++;

    char age[16];
    FormatAge(sel->mtime, age, sizeof(age));
    snprintf(meta, sizeof(meta), "last active: %s", age);
    if (row < height) WriteToBuffer(buffer, width, row++, col_start, meta, white);
    snprintf(meta, sizeof(meta), "id: %s", sel->id);
    if (row < height) WriteToBuffer(buffer, width, row++, col_start, meta, gray);
    row++;

    // The session's name, wrapped to the pane
    int pane_w = width - 1 - col_start;
    if (pane_w < 8) return;
    int off = 0;
    while (sel->name[off] != '\0' && row < height) {
        char chunk[200];
        // Sliced by cells on a character boundary, not by byte offset: this is
        // UTF-8, so cutting at pane_w bytes would split a character on every
        // wrapped row and leave each row short of the pane
        int c = Utf8Prefix(sel->name + off, pane_w, (int)sizeof(chunk) - 1);
        if (c <= 0) break; // no progress possible -- don't spin
        memcpy(chunk, sel->name + off, c);
        chunk[c] = '\0';
        WriteToBufferCP(buffer, width, row, col_start, chunk, white, CP_UTF8);
        off += c;
        row++;
    }
}
// ============================= Workspace Designer ================================
// Membership lives in <anchor>\.claude\settings.json under
// permissions.additionalDirectories -- the file Claude Code natively reads.
// Editing splices only that array so any other settings in the file survive.

static void ClearMemberSource(void) {
    free(member_source_array);
    member_source_array = NULL;
    member_source_array_length = 0;
    member_source_known = false;
    member_source_has_array = false;
}

static bool RememberMemberSource(const char* array, size_t length,
                                 bool has_array) {
    ClearMemberSource();
    if (has_array) {
        member_source_array = (char*)malloc(length);
        if (member_source_array == NULL) return false;
        memcpy(member_source_array, array, length);
        member_source_array_length = length;
    }
    member_source_has_array = has_array;
    member_source_known = true;
    return true;
}

static bool MemberSourceMatches(const char* json, bool file_exists,
                                const DriftSettingsJsonTarget* target) {
    if (!member_source_known) return true;
    bool fresh_has_array = file_exists &&
        target->action == DRIFT_SETTINGS_JSON_REPLACE_ARRAY;
    if (member_source_has_array != fresh_has_array) return false;
    if (!fresh_has_array) return true;
    size_t fresh_length = (size_t)(target->array_end - target->array_start + 1);
    return fresh_length == member_source_array_length &&
           memcmp(json + target->array_start, member_source_array,
                  fresh_length) == 0;
}

static bool IsMemberPathSeparator(char c) {
    return c == '\\' || c == '/';
}

static bool HasMemberDrivePrefix(const char* path) {
    unsigned char drive = path == NULL ? 0 : (unsigned char)path[0];
    return ((drive >= 'A' && drive <= 'Z') ||
            (drive >= 'a' && drive <= 'z')) &&
           path[1] == ':';
}

static bool IsFullyQualifiedMemberPath(const char* path) {
    if (path == NULL) return false;
    if (HasMemberDrivePrefix(path)) return IsMemberPathSeparator(path[2]);
    if (!IsMemberPathSeparator(path[0]) ||
        !IsMemberPathSeparator(path[1])) return false;

    // A UNC path needs both a server and a share. GetFullPathName accepts some
    // incomplete forms, but they do not identify one deterministic folder.
    const char* server = path + 2;
    if (*server == '\0' || IsMemberPathSeparator(*server)) return false;
    const char* separator = server;
    while (*separator != '\0' && !IsMemberPathSeparator(*separator)) separator++;
    if (*separator == '\0') return false;
    const char* share = separator + 1;
    return *share != '\0' && !IsMemberPathSeparator(*share);
}

// Resolve a configured additionalDirectories value without consulting the
// process working directory. Relative values belong to the workspace anchor,
// because that is the working directory passed to Claude for this workspace.
// The configured spelling remains in members[]; this form is only for folder
// identity and navigation.
bool ResolveMemberPath(const char* anchor, const char* stored,
                       char out[MAX_PATH]) {
    if (out == NULL) return false;
    out[0] = '\0';
    if (stored == NULL || stored[0] == '\0' || strlen(stored) >= MAX_PATH) {
        return false;
    }

    char value[MAX_PATH];
    strcpy(value, stored);
    for (char* p = value; *p != '\0'; p++) {
        if (*p == '/') *p = '\\';
    }

    char candidate[MAX_PATH * 2 + 2];
    if (HasMemberDrivePrefix(value) && !IsMemberPathSeparator(value[2])) {
        // C:folder depends on Windows' hidden per-drive working directory.
        return false;
    }

    if (IsFullyQualifiedMemberPath(value)) {
        if (snprintf(candidate, sizeof(candidate), "%s", value) >=
            (int)sizeof(candidate)) return false;
    } else if (IsMemberPathSeparator(value[0])) {
        // A single leading separator is rooted on a drive. It is deterministic
        // only when the workspace anchor supplies that drive.
        if (anchor == NULL || !HasMemberDrivePrefix(anchor) ||
            !IsMemberPathSeparator(anchor[2])) return false;
        if (snprintf(candidate, sizeof(candidate), "%c:%s", anchor[0], value) >=
            (int)sizeof(candidate)) return false;
    } else {
        if (anchor == NULL || strlen(anchor) >= MAX_PATH ||
            !IsFullyQualifiedMemberPath(anchor)) return false;
        if (snprintf(candidate, sizeof(candidate), "%s\\%s", anchor, value) >=
            (int)sizeof(candidate)) return false;
    }

    for (char* p = candidate; *p != '\0'; p++) {
        if (*p == '/') *p = '\\';
    }
    if (!IsFullyQualifiedMemberPath(candidate)) return false;

    DWORD length = GetFullPathNameA(candidate, MAX_PATH, out, NULL);
    if (length == 0 || length >= MAX_PATH) {
        out[0] = '\0';
        return false;
    }

    // Keep the separator required by drive roots, but make every other
    // trailing-separator spelling compare the same (including UNC shares).
    while (length > 0 && IsMemberPathSeparator(out[length - 1])) {
        bool drive_root = length == 3 && HasMemberDrivePrefix(out) &&
                          IsMemberPathSeparator(out[2]);
        bool device_drive_root = length == 7 &&
            IsMemberPathSeparator(out[0]) && IsMemberPathSeparator(out[1]) &&
            (out[2] == '?' || out[2] == '.') &&
            IsMemberPathSeparator(out[3]) && HasMemberDrivePrefix(out + 4) &&
            IsMemberPathSeparator(out[6]);
        if (drive_root || device_drive_root) break;
        out[--length] = '\0';
    }
    return out[0] != '\0';
}

static bool MemberPathsEqual(const char* anchor, const char* left,
                             const char* right) {
    if (left == NULL || right == NULL) return false;
    if (_stricmp(left, right) == 0) return true;
    char resolved_left[MAX_PATH];
    char resolved_right[MAX_PATH];
    return ResolveMemberPath(anchor, left, resolved_left) &&
           ResolveMemberPath(anchor, right, resolved_right) &&
           _stricmp(resolved_left, resolved_right) == 0;
}

void LoadMembersFrom(const char* anchor) {
    member_count = 0;
    json_block_reason = NULL;
    ClearMemberSource();
    member_anchor[0] = '\0';
    if (anchor == NULL || snprintf(member_anchor, sizeof(member_anchor), "%s", anchor) >=
        (int)sizeof(member_anchor)) {
        json_block_reason = MEMBER_PATH_BLOCK_REASON;
        return;
    }

    // Under the Wine wrapper, entries are stored host-style ("/Users/...")
    // so the host claude can read them; internally we use the drive form
    char host_drive[8];
    DWORD hd = GetEnvironmentVariable("DRIFT_HOST_DRIVE", host_drive, sizeof(host_drive));
    bool host = hd > 0 && hd < sizeof(host_drive);

    char file[MAX_PATH];
    if (snprintf(file, MAX_PATH, "%s\\.claude\\settings.json", anchor) >= MAX_PATH) return;
    FILE* f = fopen(file, "rb");
    if (f == NULL) {
        DWORD attr = GetFileAttributes(file);
        DWORD err = GetLastError();
        bool absent = attr == INVALID_FILE_ATTRIBUTES &&
                      (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
        if (absent) RememberMemberSource(NULL, 0, false);
        return;
    }
    int len = (int)fread(json_buf, 1, sizeof(json_buf) - 1, f);
    int extra = fgetc(f); // anything left means the file exceeds our buffer
    fclose(f);
    json_buf[len] = '\0';
    if (extra != EOF) {
        // A partial parse could corrupt the file on save
        json_block_reason = "(settings.json too large to edit)";
        return;
    }

    DriftSettingsJsonTarget target;
    if (!DriftLocateSettingsJsonTarget(json_buf, (size_t)len, &target)) {
        json_block_reason = "(settings.json structure cannot be edited safely)";
        return;
    }
    bool has_array = target.action == DRIFT_SETTINGS_JSON_REPLACE_ARRAY;
    size_t source_length = has_array ?
        (size_t)(target.array_end - target.array_start + 1) : 0;
    if (!RememberMemberSource(has_array ? json_buf + target.array_start : NULL,
                              source_length, has_array)) {
        json_block_reason = "(not enough memory to edit settings.json)";
        return;
    }
    if (target.action != DRIFT_SETTINGS_JSON_REPLACE_ARRAY) return;
    int s = target.array_start;
    int e = target.array_end;
    const char* p = json_buf + s + 1;
    const char* stop = json_buf + e;
    while (p < stop && member_count < MAX_MEMBERS) {
        if (*p != '"') {
            p++;
            continue;
        }
        p++;
        char* out = members[member_count];
        int n = 0;
        while (p < stop && *p != '"' && n < MAX_PATH - 1) {
            if (*p == '\\' && p + 1 < stop) {
                p++;
                if (*p == 'u') {
                    // '?' stands in for display only. Saving would write that
                    // '?' back as the real path, so an entry drift never
                    // touched is replaced by one that cannot exist
                    json_block_reason = "(unsupported \\u escape in settings)";
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
        // The copy stopping on the buffer bound rather than on the closing
        // quote means this path was cut short. Saving would write the prefix
        // back over the real entry, and a cut landing on a separator would
        // widen a grant on one subdirectory into the whole parent
        bool truncated = n >= MAX_PATH - 1 && p < stop && *p != '"';
        if (truncated) {
            json_block_reason = "(a folder path is too long to edit)";
        }
        if (!truncated && n == 0) {
            json_block_reason = MEMBER_PATH_BLOCK_REASON;
        }
        if (!truncated && n > 0) {
            if (host && out[0] == '/') {
                char tmp[MAX_PATH];
                if (snprintf(tmp, sizeof(tmp), "%s%s", host_drive, out) < (int)sizeof(tmp)) {
                    for (char* q = tmp; *q != '\0'; q++) {
                        if (*q == '/') *q = '\\';
                    }
                    strcpy(out, tmp);
                } else {
                    json_block_reason = MEMBER_PATH_BLOCK_REASON;
                }
            }
            char resolved[MAX_PATH];
            if (!ResolveMemberPath(member_anchor, out, resolved)) {
                // Keep the original text available for display, but refuse a
                // rewrite that could act on a different folder than Claude.
                json_block_reason = MEMBER_PATH_BLOCK_REASON;
            }
            member_count++;
        }
        while (p < stop && *p != '"') p++; // resync if the copy was truncated
        if (p < stop) p++;
    }

    // Stopping on the array bound rather than the end of the span leaves
    // entries unread, and a save rewrites the whole span from what was read --
    // silently deleting every entry past the last one that fit
    if (member_count == MAX_MEMBERS) {
        for (const char* q = p; q < stop; q++) {
            if (*q == '"') {
                json_block_reason = "(too many folders to edit safely)";
                break;
            }
        }
    }
}

// Bounded append. snprintf returns the length it *would* have written, so the
// bare "n += snprintf(buf + n, cap - n, ...)" idiom lets n run past cap on
// truncation -- after which "cap - n" wraps to a huge size_t and the next call
// writes with no bound at all. The current constants leave ~1KB of headroom so
// this never triggers today; clamping means raising MAX_MEMBERS can't arm it.
size_t AppendFmt(char* buf, size_t n, size_t cap, const char* fmt, ...) {
    if (cap == 0 || n >= cap - 1) return n;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (written < 0) return n;
    return (size_t)written >= cap - n ? cap - 1 : n + (size_t)written;
}

// Returns whether settings.json now holds the member list. False means the
// file was deliberately left alone, so a caller showing the list is showing
// something the file does not contain
bool SaveMembersTo(const char* anchor) {
    if (json_block_reason != NULL) return false;

    char dir[MAX_PATH];
    if (snprintf(dir, MAX_PATH, "%s\\.claude", anchor) >= MAX_PATH) return false;
    CreateDirectory(dir, NULL);
    char file[MAX_PATH];
    if (snprintf(file, MAX_PATH, "%s\\.claude\\settings.json", anchor) >= MAX_PATH) return false;

    // Build the replacement array text, JSON-escaping backslashes
    size_t cap = (size_t)MAX_MEMBERS * (MAX_PATH * 2 + 16) + 64;
    char* arr = (char*)malloc(cap);
    if (arr == NULL) return false;
    // Under the Wine wrapper, write entries host-style so the host claude
    // can actually resolve them
    char host_drive[8];
    DWORD hd = GetEnvironmentVariable("DRIFT_HOST_DRIVE", host_drive, sizeof(host_drive));
    bool host = hd > 0 && hd < sizeof(host_drive);
    size_t hdl = host ? strlen(host_drive) : 0;

    size_t n = 0;
    n = AppendFmt(arr, n, cap, "[");
    for (int i = 0; i < member_count; i++) {
        char written[MAX_PATH];
        const char* src = members[i];
        if (host && _strnicmp(src, host_drive, hdl) == 0 && src[hdl] == '\\') {
            snprintf(written, sizeof(written), "%s", src + hdl);
            for (char* q = written; *q != '\0'; q++) {
                if (*q == '\\') *q = '/';
            }
            src = written;
        }
        n = AppendFmt(arr, n, cap, "%s\n      \"", i == 0 ? "" : ",");
        for (const char* p = src; *p != '\0' && n < cap - 8; p++) {
            if (*p == '\\' || *p == '"') arr[n++] = '\\';
            arr[n++] = *p;
        }
        arr[n] = '\0';
        n = AppendFmt(arr, n, cap, "\"");
    }
    n = AppendFmt(arr, n, cap, member_count > 0 ? "\n    ]" : "]");

    // Re-read the file so its other keys survive the rewrite. "Not there yet"
    // and "there but unreadable" both leave len == 0, and treating the second
    // as the first would drop the create-from-scratch skeleton below over a
    // settings.json full of hooks, env and permissions. A lock (antivirus, or
    // claude holding the file) fails fopen but not GetFileAttributes, which
    // asks about metadata rather than opening anything
    FILE* f = fopen(file, "rb");
    bool file_exists = f != NULL;
    int len = 0;
    if (f != NULL) {
        len = (int)fread(json_buf, 1, sizeof(json_buf) - 1, f);
        bool read_failed = ferror(f) != 0;
        // json_block_reason was decided when the file was loaded; it may have
        // grown past json_buf since. Splicing a truncated copy back would
        // discard everything past the cut and leave the file mid-token
        bool too_big = fgetc(f) != EOF;
        fclose(f);
        if (too_big) json_block_reason = "(settings.json too large to edit)";
        if (read_failed || too_big) {
            free(arr);
            return false;
        }
    } else {
        DWORD attr = GetFileAttributes(file);
        DWORD err = GetLastError();
        bool absent = attr == INVALID_FILE_ATTRIBUTES &&
                      (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
        if (!absent) {
            free(arr);
            return false; // leave whatever is on disk alone
        }
    }
    json_buf[len] = '\0';

    DriftSettingsJsonTarget target = {0};
    if (file_exists &&
        !DriftLocateSettingsJsonTarget(json_buf, (size_t)len, &target)) {
        json_block_reason = "(settings.json structure cannot be edited safely)";
        free(arr);
        return false;
    }
    if (!MemberSourceMatches(json_buf, file_exists, &target)) {
        json_block_reason = MEMBER_CONFLICT_REASON;
        free(arr);
        return false;
    }

    char* out = (char*)malloc((size_t)len + cap + 256);
    if (out == NULL) {
        free(arr);
        return false;
    }

    if (file_exists && target.action == DRIFT_SETTINGS_JSON_REPLACE_ARRAY) {
        // Replace just the array; everything else in the file survives
        memcpy(out, json_buf, target.array_start);
        out[target.array_start] = '\0';
        strcat(out, arr);
        strcat(out, json_buf + target.array_end + 1);
    } else if (file_exists) {
        // The shared structural locator proved both the parent object and the
        // exact insertion brace. Text equal to either key elsewhere cannot
        // influence this offset
        bool in_permissions =
            target.action == DRIFT_SETTINGS_JSON_INSERT_IN_PERMISSIONS;
        int at = target.insert_at;
        memcpy(out, json_buf, at);
        out[at] = '\0';
        char insert[512];
        if (in_permissions) {
            snprintf(insert, sizeof(insert), "\n    \"additionalDirectories\": ");
        } else {
            snprintf(insert, sizeof(insert), "\n  \"permissions\": {\n    \"additionalDirectories\": ");
        }
        strcat(out, insert);
        strcat(out, arr);
        if (!in_permissions) strcat(out, "\n  }");
        if (target.needs_comma) strcat(out, ",");
        strcat(out, json_buf + at);
    } else {
        snprintf(out, cap + 256, "{\n  \"permissions\": {\n    \"additionalDirectories\": ");
        strcat(out, arr);
        strcat(out, "\n  }\n}\n");
    }

    // Write via temp + rename so a crash can't leave a half-written file
    bool saved = false;
    char tmp[MAX_PATH];
    if (snprintf(tmp, MAX_PATH, "%s.tmp", file) < MAX_PATH) {
        FILE* w = fopen(tmp, "wb");
        if (w != NULL) {
            size_t want = strlen(out);
            bool ok = fwrite(out, 1, want, w) == want;
            // Most of the file is still sitting in the stdio buffer, so a
            // full disk reports the error here rather than from fwrite
            if (fclose(w) != 0) ok = false;
            // Renaming a short write over the real file would destroy it --
            // drop the temp instead and leave settings.json as it was
            if (!ok || !MoveFileEx(tmp, file, MOVEFILE_REPLACE_EXISTING)) {
                DeleteFile(tmp);
            } else {
                saved = true;
            }
        }
    }
    if (saved) RememberMemberSource(arr, strlen(arr), true);
    free(arr);
    free(out);
    return saved;
}

// A display name becomes a filename here, so it has to survive being one.
// Names are typed as printable ASCII, which still admits every character Win32
// reserves -- and the result reaches a command line, so '%' and '"' have to go
// too. Anything outside the safe set becomes '-'; an empty result means the
// caller falls back to the folder id.
static void SanitizeWorkspaceFileName(const char* name, char* out, size_t out_size) {
    size_t n = 0;
    for (const char* p = name; *p != '\0' && n + 1 < out_size; p++) {
        unsigned char c = (unsigned char)*p;
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
                    c == '_' || c == '-';
        out[n++] = safe ? (char)c : '-';
    }
    // Win32 silently strips trailing dots and spaces, so a name ending in one
    // would not be the file that was created
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
    out[n] = '\0';
}

static size_t AppendJsonEscaped(char* buf, size_t n, size_t cap, const char* text) {
    for (const char* p = text; *p != '\0' && n + 2 < cap; p++) {
        if (*p == '\\' || *p == '"') buf[n++] = '\\';
        buf[n++] = *p;
    }
    if (n < cap) buf[n] = '\0';
    return n;
}

// Write the workspace's folder set as a VS Code multi-root workspace, beside
// the anchor's CLAUDE.md. Requires members[] to already hold this anchor's list.
//
// Regenerated in full every time, unlike settings.json, which is spliced. That
// is not an inconsistency: settings.json is Claude's file and drift refuses to
// be lossy with it, while this one is drift's own output. The header comment
// says so in the file itself -- .code-workspace is JSONC, so VS Code reads it.
static bool WriteCodeWorkspaceFile(const char* anchor, const char* display,
                                   char out_path[MAX_PATH]) {
    char leaf[MAX_PATH];
    SanitizeWorkspaceFileName(display, leaf, sizeof(leaf));
    if (leaf[0] == '\0') snprintf(leaf, sizeof(leaf), "%s", AnchorFolder(anchor));
    if (snprintf(out_path, MAX_PATH, "%s\\%s.code-workspace", anchor, leaf) >= MAX_PATH) {
        return false;
    }

    size_t cap = (size_t)MAX_MEMBERS * (MAX_PATH * 2 + 32) + 1024;
    char* text = (char*)malloc(cap);
    if (text == NULL) return false;

    size_t n = 0;
    n = AppendFmt(text, n, cap,
                  "{\n"
                  "  // Generated by drift from .claude\\settings.json.\n"
                  "  // Edits here are overwritten. Put your own settings in a\n"
                  "  // member folder's .vscode\\settings.json instead.\n"
                  "  \"folders\": [\n");
    // The anchor first: it is where claude runs and where CLAUDE.md lives, so
    // the workspace's own notes belong in the tree. "." resolves against this
    // file's directory, which is why the file has to sit at the anchor root.
    // Its folder name is a bare timestamp, so give it the display name instead
    n = AppendFmt(text, n, cap, "    { \"path\": \".\", \"name\": \"");
    n = AppendJsonEscaped(text, n, cap, display);
    n = AppendFmt(text, n, cap, "\" }");

    for (int i = 0; i < member_count; i++) {
        char resolved[MAX_PATH];
        // The configured spelling may be relative; VS Code would resolve it
        // against this file rather than against the anchor claude runs in
        if (!ResolveMemberPath(anchor, members[i], resolved)) continue;
        n = AppendFmt(text, n, cap, ",\n    { \"path\": \"");
        n = AppendJsonEscaped(text, n, cap, resolved);
        n = AppendFmt(text, n, cap, "\" }");
    }
    n = AppendFmt(text, n, cap, "\n  ]\n}\n");

    // Temp + rename, so an interrupted write cannot leave VS Code a half file
    bool saved = false;
    char tmp[MAX_PATH];
    if (snprintf(tmp, MAX_PATH, "%s.tmp", out_path) < MAX_PATH) {
        FILE* w = fopen(tmp, "wb");
        if (w != NULL) {
            size_t want = strlen(text);
            bool ok = fwrite(text, 1, want, w) == want;
            if (fclose(w) != 0) ok = false;
            if (!ok || !MoveFileEx(tmp, out_path, MOVEFILE_REPLACE_EXISTING)) {
                DeleteFile(tmp);
            } else {
                saved = true;
            }
        }
    }
    free(text);
    return saved;
}

int FindMember(const char* path) {
    if (path == NULL) return -1;
    // Prefer the exact configured spelling when duplicate legacy entries are
    // present; the second pass supplies operational path identity.
    for (int i = 0; i < member_count; i++) {
        if (_stricmp(members[i], path) == 0) return i;
    }
    for (int i = 0; i < member_count; i++) {
        if (MemberPathsEqual(member_anchor, members[i], path)) return i;
    }
    return -1;
}

enum MemberLockResult AcquireMemberLock(const char* anchor, HANDLE* lock) {
    *lock = INVALID_HANDLE_VALUE;
    char dir[MAX_PATH];
    if (snprintf(dir, sizeof(dir), "%s\\.claude", anchor) >= (int)sizeof(dir)) {
        return MEMBER_LOCK_FAILED;
    }
    if (!CreateDirectory(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return MEMBER_LOCK_FAILED;
    }
    DWORD attr = GetFileAttributes(dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return MEMBER_LOCK_FAILED;
    }

    char path[MAX_PATH];
    if (snprintf(path, sizeof(path), "%s\\.drift-members.lock", dir) >=
        (int)sizeof(path)) {
        return MEMBER_LOCK_FAILED;
    }

    ULONGLONG started = GetTickCount64();
    while (true) {
        HANDLE candidate = CreateFile(
            path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, NULL);
        if (candidate != INVALID_HANDLE_VALUE) {
            *lock = candidate;
            return MEMBER_LOCK_ACQUIRED;
        }
        if (GetLastError() != ERROR_SHARING_VIOLATION) {
            return MEMBER_LOCK_FAILED;
        }
        if (GetTickCount64() - started >= DRIFT_MEMBER_LOCK_TIMEOUT_MS) {
            return MEMBER_LOCK_BUSY;
        }
        Sleep(DRIFT_MEMBER_LOCK_RETRY_MS);
    }
}

enum MemberChangeResult ApplyMemberChange(
    const char* anchor, const char* path, enum MemberChangeAction action) {
    if (anchor == NULL || path == NULL || path[0] == '\0' ||
        strlen(path) >= MAX_PATH) {
        return MEMBER_CHANGE_IO_FAILED;
    }

    HANDLE lock;
    enum MemberLockResult lock_result = AcquireMemberLock(anchor, &lock);
    if (lock_result != MEMBER_LOCK_ACQUIRED) {
        // The destination is stable until another Drift reaches its final
        // rename, so a best-effort refresh is still better than retaining the
        // old edit-mode snapshot after a visible refusal.
        LoadMembersFrom(anchor);
        return lock_result == MEMBER_LOCK_BUSY ?
            MEMBER_CHANGE_BUSY : MEMBER_CHANGE_IO_FAILED;
    }

    enum MemberChangeResult result = MEMBER_CHANGE_IO_FAILED;
    LoadMembersFrom(anchor);
    if (json_block_reason != NULL) {
        result = MEMBER_CHANGE_SETTINGS_BLOCKED;
        goto done;
    }

    char resolved_path[MAX_PATH];
    if (!ResolveMemberPath(anchor, path, resolved_path)) {
        json_block_reason = MEMBER_PATH_BLOCK_REASON;
        result = MEMBER_CHANGE_SETTINGS_BLOCKED;
        goto done;
    }

    int index = FindMember(path);
    if (action == MEMBER_CHANGE_ADD) {
        if (index >= 0) {
            result = MEMBER_CHANGE_NO_CHANGE;
            goto done;
        }
        if (member_count >= MAX_MEMBERS) {
            result = MEMBER_CHANGE_FULL;
            goto done;
        }
        strcpy(members[member_count], path);
        member_count++;
    } else {
        if (index < 0) {
            result = MEMBER_CHANGE_NO_CHANGE;
            goto done;
        }
        // One explicit removal revokes every spelling of the same operational
        // folder so a hidden relative/absolute duplicate cannot retain access.
        int write = 0;
        for (int i = 0; i < member_count; i++) {
            if (MemberPathsEqual(anchor, members[i], path)) continue;
            if (write != i) strcpy(members[write], members[i]);
            write++;
        }
        member_count = write;
    }

    if (SaveMembersTo(anchor)) {
        result = MEMBER_CHANGE_SAVED;
    } else {
        bool conflict = json_block_reason != NULL &&
                        strcmp(json_block_reason, MEMBER_CONFLICT_REASON) == 0;
        bool blocked = json_block_reason != NULL;
        // Restore the pane to what the destination actually contains. This
        // also clears a transaction-only source-conflict reason after its
        // typed result has been captured for the caller.
        LoadMembersFrom(anchor);
        result = conflict ? MEMBER_CHANGE_CONFLICT :
                 blocked ? MEMBER_CHANGE_SETTINGS_BLOCKED :
                           MEMBER_CHANGE_IO_FAILED;
    }

done:
    CloseHandle(lock);
    return result;
}

static void NormalizeManifestSelection(void) {
    if (manifest_selected >= member_count && manifest_selected > 0) {
        manifest_selected = member_count - 1;
    }
}

static void ReportMemberChangeFailure(enum MemberChangeResult result) {
    if (result == MEMBER_CHANGE_FULL) {
        NotifyAndWait("that workspace is full -- press a key");
    } else if (result == MEMBER_CHANGE_SETTINGS_BLOCKED) {
        NotifyAndWait("settings.json cannot be edited safely -- press a key");
    } else if (result == MEMBER_CHANGE_BUSY) {
        NotifyAndWait("settings.json is busy in another Drift -- press a key");
    } else if (result == MEMBER_CHANGE_CONFLICT) {
        NotifyAndWait("workspace folders changed during the edit -- try again -- press a key");
    } else if (result == MEMBER_CHANGE_IO_FAILED) {
        NotifyAndWait("settings.json was not updated -- press a key");
    }
}

void RemoveMemberAt(int index) {
    if (index < 0 || index >= member_count) return;
    char path[MAX_PATH];
    strcpy(path, members[index]);
    enum MemberChangeResult result = ApplyMemberChange(
        edit_workspace, path, MEMBER_CHANGE_REMOVE);
    NormalizeManifestSelection();
    ReportMemberChangeFailure(result);
}

bool JumpToMemberAt(int index) {
    if (index < 0 || index >= member_count) return false;
    char jump[MAX_PATH];
    if (!ResolveMemberPath(edit_workspace, members[index], jump)) return false;
    ChangeCurrentDirectory(jump);
    manifest_focused = false;
    return true;
}

void ToggleMemberUnderCursor() {
    if (current_directory_file_count == 0) return;
    if (!IsDirectory(&current_directory_files[selected_row])) return;
    char path[MAX_PATH];
    if (!GetSelectedRowPath(selected_row, path)) return;

    int i = FindMember(path);
    if (i >= 0) {
        RemoveMemberAt(i);
        return;
    }
    enum MemberChangeResult result = ApplyMemberChange(
        edit_workspace, path, MEMBER_CHANGE_ADD);
    NormalizeManifestSelection();
    ReportMemberChangeFailure(result);
}

void EnterEditMode() {
    if (current_directory_file_count == 0) return;
    if (!IsDirectory(&current_directory_files[selected_row])) return;
    char* name = current_directory_files[selected_row].cFileName;
    if (snprintf(edit_workspace, MAX_PATH, "%s\\%s", workspaces_root, name) >= MAX_PATH) return;
    // edit_workspace keeps the real folder path; the shown label uses the
    // display name so editing reads the same as everywhere else
    WorkspaceDisplayName(name, edit_workspace_name, MAX_PATH);

    LoadMembersFrom(edit_workspace);
    manifest_focused = false;
    manifest_selected = 0;
    manifest_top = 0;
    edit_armed = true;
    claude_mode = CM_OFF;
    ChangeCurrentDirectory(claude_return_dir);
}

void ExitEditMode() {
    edit_armed = false;
    manifest_focused = false;
    ChangeCurrentDirectory(workspaces_root);
    claude_mode = CM_WORKSPACES;
}

// Every workspace gets a CLAUDE.md. Claude runs with the anchor as its working
// directory, so this file is the workspace's own project memory -- notes here
// reach every folder in the workspace at once, while each member folder can
// still carry its own. Created empty on purpose: it belongs to the user, who
// knows what a CLAUDE.md is for, and template text in a file drift will never
// rewrite would just be noise every session pays for. CREATE_NEW means an
// existing file is left exactly as it is.
void EnsureWorkspaceNotes(const char* anchor) {
    char file[MAX_PATH];
    if (snprintf(file, MAX_PATH, "%s\\CLAUDE.md", anchor) >= MAX_PATH) return;
    HANDLE h = CreateFile(file, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
}

// 'f' on the workspace list: browse the workspace's own anchor directory.
// The anchor is a real folder that nothing previously led to, yet it is where
// a workspace's own notes belong -- claude runs with the anchor as its working
// directory, so a CLAUDE.md there is the workspace's project memory, covering
// every member folder at once. Its name is an opaque timestamp, so reaching it
// by typing a path is not realistic; this is the way in.
//
// The mode is the ordinary file browser, unrestricted -- only the exits are
// rerouted (see HandleInput), so the browse cannot wander out of the anchor
// while the header still claims to be showing a workspace.
void EnterAnchorMode() {
    if (current_directory_file_count == 0) return;
    if (!IsDirectory(&current_directory_files[selected_row])) return;
    char* name = current_directory_files[selected_row].cFileName;
    if (snprintf(anchor_workspace, MAX_PATH, "%s\\%s", workspaces_root, name) >= MAX_PATH) return;
    WorkspaceDisplayName(name, anchor_workspace_name, MAX_PATH);

    // Workspaces made before this existed have no CLAUDE.md; opening their
    // files is the natural moment to give them one
    EnsureWorkspaceNotes(anchor_workspace);

    anchor_armed = true;
    claude_mode = CM_OFF;
    ChangeCurrentDirectory(anchor_workspace);

    // Land on CLAUDE.md -- the file this mode exists for, one Enter from the
    // editor

    for (int i = 0; i < current_directory_file_count; i++) {
        if (_stricmp(current_directory_files[i].cFileName, "CLAUDE.md") == 0) {
            selected_row = i;
            break;
        }
    }
}

void ExitAnchorMode() {
    anchor_armed = false;
    ChangeCurrentDirectory(workspaces_root);
    claude_mode = CM_WORKSPACES;
}

// Shift+W while browsing: add the directory under the cursor to a workspace
// picked from a small popup, without entering claude mode at all
void HandleQuickAdd() {
    // Inert inside an anchor browse too -- the only directory there is
    // .claude, and adding it as a member folder is never what was meant
    if (claude_mode != CM_OFF || edit_armed || anchor_armed) return;
    if (current_directory_file_count == 0) return;
    if (!IsDirectory(&current_directory_files[selected_row])) return;
    char target[MAX_PATH];
    if (!GetSelectedRowPath(selected_row, target)) return;
    char root[MAX_PATH];
    if (!GetWorkspacesRoot(root)) return;

    preview_path[0] = '\0'; // reusing the preview array trashes its cache
    int total = GetFilesInDirectory(root, preview_files);
    // names[] holds folder ids -- the key the anchor path is built from --
    // while labels[] holds what the user actually calls each workspace. The
    // ids are minted timestamps, so a popup listing them names nothing.
    char names[9][MAX_PATH];
    char labels[9][MAX_PATH];
    int count = 0;
    for (int i = 0; i < total && count < 9; i++) {
        if (IsDirectory(&preview_files[i])) {
            strcpy(names[count], preview_files[i].cFileName);
            WorkspaceDisplayName(names[count], labels[count], MAX_PATH);
            count++;
        }
    }
    if (count == 0) return; // no workspaces yet -- create one via c, a

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;
    int popup_w = 44;
    int popup_h = count + 5;
    if (popup_w > screen_width) popup_w = screen_width;
    if (popup_w < 20 || screen_height < popup_h) return;

    CHAR_INFO* popup = (CHAR_INFO*)malloc(popup_w * popup_h * sizeof(CHAR_INFO));
    if (popup == NULL) return;

    for (int i = 0; i < popup_w * popup_h; i++) {
        popup[i].Char.UnicodeChar = L' ';
        popup[i].Attributes = white;
    }
    popup[0].Char.UnicodeChar = BOX_TOP_LEFT;
    popup[popup_w - 1].Char.UnicodeChar = BOX_TOP_RIGHT;
    int bottom = (popup_h - 1) * popup_w;
    popup[bottom].Char.UnicodeChar = BOX_BOTTOM_LEFT;
    popup[bottom + popup_w - 1].Char.UnicodeChar = BOX_BOTTOM_RIGHT;
    for (int c = 1; c < popup_w - 1; c++) {
        popup[c].Char.UnicodeChar = BOX_HORIZONTAL;
        popup[bottom + c].Char.UnicodeChar = BOX_HORIZONTAL;
    }
    for (int r = 1; r < popup_h - 1; r++) {
        popup[r * popup_w].Char.UnicodeChar = BOX_VERTICAL;
        popup[r * popup_w + popup_w - 1].Char.UnicodeChar = BOX_VERTICAL;
    }
    WriteToBuffer(popup, popup_w, 1, 2, "Add folder to workspace:", yellow);
    for (int i = 0; i < count; i++) {
        char line[64];
        snprintf(line, sizeof(line), "[%d] %s", i + 1, labels[i]);
        WriteToBuffer(popup, popup_w, 3 + i, 2, line, white);
    }

    int start_col = info.srWindow.Left + (screen_width - popup_w) / 2;
    int start_row = info.srWindow.Top + (screen_height - popup_h) / 2;
    COORD buffer_size = { (SHORT)popup_w, (SHORT)popup_h };
    COORD origin = { 0, 0 };
    SMALL_RECT region = { (SHORT)start_col, (SHORT)start_row,
                          (SHORT)(start_col + popup_w - 1), (SHORT)(start_row + popup_h - 1) };
    WriteConsoleOutputW(hAlt, popup, buffer_size, origin, &region);
    free(popup);

    while (1) {
        INPUT_RECORD input;
        DWORD events;
        if (!ReadConsoleInput(hIn, &input, 1, &events)) break;
        if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) continue;

        char ch = input.Event.KeyEvent.uChar.AsciiChar;
        if (ch >= '1' && ch < '1' + count) {
            char anchor[MAX_PATH];
            if (snprintf(anchor, MAX_PATH, "%s\\%s", root, names[ch - '1']) < MAX_PATH) {
                enum MemberChangeResult result = ApplyMemberChange(
                    anchor, target, MEMBER_CHANGE_ADD);
                const char* refused = NULL;
                if (result == MEMBER_CHANGE_NO_CHANGE) {
                    refused = "it is already there";
                } else if (result == MEMBER_CHANGE_FULL) {
                    refused = "that workspace is full";
                } else if (result == MEMBER_CHANGE_SETTINGS_BLOCKED) {
                    refused = "its settings cannot be edited";
                } else if (result == MEMBER_CHANGE_BUSY) {
                    refused = "another Drift is editing it";
                } else if (result == MEMBER_CHANGE_CONFLICT) {
                    refused = "its folders changed; try again";
                } else if (result == MEMBER_CHANGE_IO_FAILED) {
                    refused = "settings.json could not be written";
                }
                char msg[160];
                if (refused == NULL) {
                    snprintf(msg, sizeof(msg), "Added to %s", labels[ch - '1']);
                    ShowStatusBanner(msg);
                } else {
                    snprintf(msg, sizeof(msg), "Not added: %s -- press a key", refused);
                    NotifyAndWait(msg);
                }
            }
            break;
        }
        if (input.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE ||
            input.Event.KeyEvent.wVirtualKeyCode == 'Q') {
            break;
        }
    }
}
// ============================= Workspace Designer ================================

// The workspace list has no parent level, so the left pane holds the
// identity and keymap. Every line stays under the 28-column divider.
void DrawClaudeHelpPane(CHAR_INFO* buffer, int width, int height) {
    int row = 2;
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "A workspace is a set", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "of folders Claude", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "opens together.", gray);
    row++;
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "l    open sessions", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "n    new session", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "e    edit workspace", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "f    workspace files", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "v    open in VS Code", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "a    new workspace", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "r    rename workspace", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "y/p  duplicate", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "d    delete workspace", gray);
    if (row < height) WriteToBuffer(buffer, width, row++, 0, "c    back to files", gray);
}

// Third pane on the workspace list: live sessions preview for the
// highlighted workspace
void DrawClaudeInfoPane(CHAR_INFO* buffer, int width, int height, int divider2) {
    int col = divider2 + 2;
    if (col >= width - 8) return;

    // The zero-workspace guidance lives in the workspaces column itself
    if (current_directory_file_count == 0) return;

    WIN32_FIND_DATA* sel = &current_directory_files[selected_row];
    if (!IsDirectory(sel)) return;

    char anchor[MAX_PATH];
    if (snprintf(anchor, MAX_PATH, "%s\\%s", workspaces_root, sel->cFileName) >= MAX_PATH) return;
    LoadSessionsFor(anchor);

    char meta[64];
    snprintf(meta, sizeof(meta), "%d session%s", session_count, session_count == 1 ? "" : "s");
    WriteToBuffer(buffer, width, 2, col, meta, white);

    if (session_count == 0) {
        if (4 < height) WriteToBuffer(buffer, width, 4, col, "press e to select the folders", gray);
        if (5 < height) WriteToBuffer(buffer, width, 5, col, "this workspace opens in Claude", gray);
        return;
    }
    // No keymap line down here: this view already carries the full one in the
    // left pane, where it reads at the top rather than along the bottom edge
    int rows = height - 4;
    for (int i = 0; i < rows && i < session_count; i++) {
        char age[16];
        FormatAge(sessions[i].mtime, age, sizeof(age));
        char row_text[160];
        snprintf(row_text, sizeof(row_text), "%-4s %s", age, sessions[i].name);
        WriteToBuffer(buffer, width, 4 + i, col, row_text, white);
    }
}

// The pinned folder list while a workspace edit is armed: the whole design
// stays visible as you browse and toggle
void DrawManifestPane(CHAR_INFO* buffer, int width, int height, int divider2) {
    int col = divider2 + 2;
    if (col >= width - 8) return;

    char title[128];
    snprintf(title, sizeof(title), "%sediting workspace: %s",
             manifest_focused ? "> " : "", edit_workspace_name);
    WriteToBuffer(buffer, width, 2, col, title, yellow);

    // Keymap stacked one shortcut per line, same style as the main page
    int row = 4;
    if (manifest_focused) {
        // Space, not x: it means "act on the folder under the cursor" in both
        // states, so one key covers the whole page. x stays as an alias.
        if (row < height) WriteToBuffer(buffer, width, row++, col, "Space  remove folder", gray);
        if (row < height) WriteToBuffer(buffer, width, row++, col, "Enter  jump to folder", gray);
    } else {
        if (row < height) WriteToBuffer(buffer, width, row++, col, "Space  add/remove folder", gray);
        if (row < height) WriteToBuffer(buffer, width, row++, col, "Tab    focus this list", gray);
        if (row < height) WriteToBuffer(buffer, width, row++, col, "Esc    done", gray);
    }

    if (json_block_reason != NULL) {
        if (9 < height) WriteToBuffer(buffer, width, 9, col, json_block_reason, red);
        return;
    }

    // Count sits directly above the list it counts; rows are fixed so the
    // list doesn't jump when the keymap swaps on focus change
    char meta[64];
    snprintf(meta, sizeof(meta), "%d folder%s", member_count, member_count == 1 ? "" : "s");
    if (9 < height) WriteToBuffer(buffer, width, 9, col, meta, white);

    if (member_count == 0) {
        if (10 < height) WriteToBuffer(buffer, width, 10, col, "(Space on a directory adds it)", gray);
        return;
    }

    int first_row = 10;
    int rows = height - first_row;
    if (rows < 1) return;
    if (manifest_selected >= member_count) manifest_selected = member_count - 1;
    if (manifest_top > manifest_selected) manifest_top = manifest_selected;
    if (manifest_selected - manifest_top >= rows) manifest_top = manifest_selected - rows + 1;
    if (manifest_top < 0) manifest_top = 0;

    int pane_w = width - 1 - col;
    for (int i = 0; i < rows && manifest_top + i < member_count; i++) {
        int idx = manifest_top + i;
        bool sel = manifest_focused && idx == manifest_selected;
        WORD attr = white;
        if (sel) {
            attr = bar_background;
            for (int c2 = divider2 + 1; c2 < width; c2++) {
                buffer[(first_row + i) * width + c2].Attributes = bar_background;
            }
        }
        // Show the tail of the path -- the deep end is the useful part
        const char* path = members[idx];
        int plen = (int)strlen(path);
        char disp[MAX_PATH + 4];
        if (plen <= pane_w) {
            snprintf(disp, sizeof(disp), "%s", path);
        } else if (pane_w > 3) {
            snprintf(disp, sizeof(disp), "...%s", path + (plen - (pane_w - 3)));
        } else {
            disp[0] = '\0';
        }
        WriteToBuffer(buffer, width, first_row + i, col, disp, attr);
    }
}
// ============================= Claude Workspaces =================================

int HandleInput() {
    INPUT_RECORD input;
    DWORD events;
    if (!ReadConsoleInput(hIn, &input, 1, &events)) {
        return 0; // Console input unavailable (e.g. redirected stdin) -- exit
    }
    // Documented to block until a record is available, so this should not
    // happen. Guarded only here, of the nine read sites: everywhere else an
    // uninitialised record is one ignored loop iteration, while here it would
    // be dispatched as whatever command the stack happened to hold
    if (events == 0) {
        return 1;
    }

    if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) {
        return 1; // Ignore non-key events (window resize lands here and triggers a redraw)
    }

    BOOL ctrl = input.Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
    BOOL shift= input.Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED;

    // Consumed once here for every key, rather than in each branch that
    // happens to remember. The mode blocks below return early without
    // clearing it, so a 'g' pressed in the workspace list used to survive an
    // intervening verb and pair with the next 'g' as though it were "gg"
    bool was_g = pending_g;
    pending_g = false;

    // Session list has its own small keymap; everything else is inert there
    if (claude_mode == CM_SESSIONS) {
        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
        // This view binds no Ctrl chords, but it tested the key code alone, so
        // the ones bound elsewhere landed on whatever shared the letter:
        // Ctrl+D (half-page down everywhere else) raised the delete prompt,
        // Ctrl+N launched claude, Ctrl+C left the mode and Ctrl+Q quit drift.
        // ENABLE_PROCESSED_INPUT is off, so Ctrl+C arrives as a key event, and
        // AltGr on non-US layouts reports as Ctrl too
        if (ctrl) return 1;
        if (vk == 'Q') {
            return 0;
        } else if (vk == 'J' && session_selected < session_count - 1) {
            session_selected++;
        } else if (vk == 'K' && session_selected > 0) {
            session_selected--;
        } else if (vk == 'G' && shift && session_count > 0) {
            session_selected = session_count - 1;
        } else if (vk == 'H' || vk == VK_ESCAPE) {
            claude_mode = CM_WORKSPACES;
        } else if (vk == 'C') {
            ExitClaudeMode();
        } else if ((vk == VK_RETURN || vk == 'L') && session_count > 0) {
            return LaunchClaudeIn(claude_workspace, sessions[session_selected].id);
        } else if (vk == 'N') {
            return LaunchClaudeIn(claude_workspace, NULL);
        } else if (vk == 'R') {
            HandleRenameSession();
        } else if (vk == 'D') {
            HandleDeleteSession();
        }
        return 1;
    }

    // Workspace list is a real directory browsed normally, with a few verbs
    // rerouted: l/Enter opens the session view instead of entering the dir,
    // h/c/Esc leave the mode, and jumps that would teleport away are inert
    if (claude_mode == CM_WORKSPACES) {
        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
        // Checked ahead of the modifier gate below, and without consulting
        // shift: '`' and '~' are one key (VK_OEM_3, differing only by shift),
        // so gating this would let '~' through to JumpToHome while the mode
        // stayed on -- leaving the list drawing the home directory as though
        // those folders were workspaces, with every workspace verb aimed at
        // a path under workspaces_root that does not exist. 'o' teleports by
        // absolute path for the same reason. These two are the only keys that
        // can move the browser out of the workspaces folder from here.
        if (vk == VK_OEM_3 || vk == 'O') {
            return 1;
        }
        // The rest are unmodified verbs; Ctrl and Shift keys fall through to
        // the cursor and marking handlers below
        if (!ctrl && !shift) {
            // 'p' duplicates a workspace marked here with 'y', which works
            // because the copy lands in the directory the marks came from.
            // Marks carried in from an ordinary directory instead paste
            // unrelated files *into* the workspaces folder -- moving them
            // outright when the set was cut -- where they would then draw as
            // workspace rows. Only marks made here can be pasted here
            if (vk == 'P' && !MarkDirEqualToCurrentDir()) {
                return 1;
            }
            if (vk == 'L' || vk == VK_RETURN) {
                if (current_directory_file_count > 0 &&
                    IsDirectory(&current_directory_files[selected_row])) {
                    EnterSessionsView();
                }
                return 1;
            }
            if (vk == 'H' || vk == 'C' || vk == VK_ESCAPE) {
                ExitClaudeMode();
                return 1;
            }
            if (vk == 'E') {
                EnterEditMode();
                return 1;
            }
            if (vk == 'F') {
                EnterAnchorMode();
                return 1;
            }
            // Not the ordinary browser 'v'. Falling through would open the
            // anchor -- a lone timestamp folder -- when the useful answer in
            // this view is the workspace's whole member set as one window
            if (vk == 'V') {
                HandleOpenWorkspaceInEditor();
                return 1;
            }
            if (vk == 'R') {
                HandleRenameWorkspace();
                return 1;
            }
            if (vk == 'N') {
                // New session in the workspace under the cursor
                if (current_directory_file_count > 0 &&
                    IsDirectory(&current_directory_files[selected_row])) {
                    char anchor[MAX_PATH];
                    if (snprintf(anchor, MAX_PATH, "%s\\%s", workspaces_root,
                                 current_directory_files[selected_row].cFileName) < MAX_PATH) {
                        return LaunchClaudeIn(anchor, NULL);
                    }
                }
                return 1;
            }
        }
    }

    // Armed workspace editing: Space toggles membership, Tab focuses the
    // manifest list, Esc finishes; the copy/cut/paste/delete verbs are
    // suspended so Space can only ever mean one thing here
    if (edit_armed && claude_mode == CM_OFF) {
        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
        if (manifest_focused) {
            if (vk == 'Q') return 0;
            if (vk == 'J' && manifest_selected < member_count - 1) manifest_selected++;
            else if (vk == 'K' && manifest_selected > 0) manifest_selected--;
            else if ((vk == 'X' || vk == VK_SPACE) && member_count > 0) RemoveMemberAt(manifest_selected);
            else if (vk == VK_RETURN && member_count > 0) {
                // Jump the browser to this member for inspection
                if (!JumpToMemberAt(manifest_selected)) {
                    NotifyAndWait("folder path cannot be resolved safely -- press a key");
                }
            }
            else if (vk == VK_TAB || vk == VK_ESCAPE || vk == 'H') manifest_focused = false;
            else if (vk == 'C' || vk == VK_BACK) ExitEditMode();
            return 1;
        }
        if (!ctrl && !shift) {
            if (vk == VK_ESCAPE || vk == 'C' || vk == VK_BACK) {
                ExitEditMode(); // back to the Claude Workspaces page
                return 1;
            }
            if (vk == 'L') {
                // 'l' means "go right": with nothing to enter under the
                // cursor, the pane to the right is the manifest
                if (current_directory_file_count == 0 ||
                    !IsDirectory(&current_directory_files[selected_row])) {
                    if (member_count > 0) manifest_focused = true;
                    return 1;
                }
                // on a directory it falls through and navigates as usual
            }
            if (vk == VK_TAB) {
                if (member_count > 0) manifest_focused = true;
                return 1;
            }
            if (vk == VK_SPACE) {
                ToggleMemberUnderCursor();
                return 1;
            }
            if (vk == 'Y' || vk == 'X' || vk == 'P' || vk == 'D') {
                return 1; // file operations are suspended while editing
            }
        }
    }

    // Browsing a workspace's anchor: every file verb stays live (it is a real
    // directory and drift is a file manager), but the ways *out* are rerouted
    // so the browse stays inside the workspace named in the header
    if (anchor_armed && claude_mode == CM_OFF) {
        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
        // '`'/'~' and the history popup teleport by absolute path -- they
        // would leave the anchor with the mode still armed
        if (vk == VK_OEM_3 || vk == 'O') {
            return 1;
        }
        if (!ctrl && !shift) {
            if (vk == VK_ESCAPE || vk == 'C' || vk == VK_BACK) {
                ExitAnchorMode();
                return 1;
            }
            // 'h' walks up as usual inside the anchor; at its root "up" is
            // the workspace list, not the raw workspaces folder full of
            // timestamp names
            if (vk == 'H' && _stricmp(current_directory, anchor_workspace) == 0) {
                ExitAnchorMode();
                return 1;
            }
        }
    }

    if (shift) {
        if (input.Event.KeyEvent.wVirtualKeyCode == 'G') {
            // Jump to bottom: any magnitude >= file count clamps to the last row
            ModifySelectedRow(current_directory_file_count);
        }
        else if (input.Event.KeyEvent.wVirtualKeyCode == VK_OEM_3) {
            JumpToHome(); // '~' -- same key as backtick, kept for muscle memory
        }
        else if (input.Event.KeyEvent.wVirtualKeyCode == 'W') {
            HandleQuickAdd();
        }
    }
    else if (ctrl) {

        int half_page = GetVisibleRows() / 2;
        if (half_page < 1) half_page = 1;

        switch (input.Event.KeyEvent.wVirtualKeyCode) {
            case 'D': {
                ModifySelectedRow(half_page);
                break;
            }
            case 'U': {
                ModifySelectedRow(-half_page);
                break;
            }
            case 'A': {
                if (current_directory_file_count == 0) break;

                ClearMarkedFiles();
                strcpy(mark_directory, current_directory);

                // GetFilesInDirectory caps the listing at MAX_FILES, so this
                // cannot overflow marked_files. Files whose full path would
                // truncate are skipped rather than marked wrong.
                int n = 0;
                for (int i = 0; i < current_directory_file_count; i++) {
                    if (GetFilePath(current_directory, &current_directory_files[i], marked_files[n].path)) {
                        n++;
                    }
                }
                marked_files_count = n;
                break;
            }
            case VK_OEM_4: {
                ClearMarkedFiles();
                break;
            }
        }
    }
    else {
        switch (input.Event.KeyEvent.wVirtualKeyCode) {
            case 'A': {
                HandleCreate();
                break;
            }
            case 'H': {
                if (parent_directory[0] != '\0') {
                    ChangeCurrentDirectory(parent_directory);
                }
                break;
            }
            case 'L': {
                if (current_directory_file_count == 0) break;

                if (IsDirectory(&current_directory_files[selected_row])) {
                    char selected_directory_path[MAX_PATH];
                    if (GetSelectedRowPath(selected_row, selected_directory_path)) {
                        ChangeCurrentDirectory(selected_directory_path);
                    }
                }
                break;
            }
            case 'J': {
                ModifySelectedRow(1);
                break;
            }
            case 'K': {
                ModifySelectedRow(-1);
                break;
            }
            case 'G': {
                if (was_g) {
                    ModifySelectedRow(-current_directory_file_count);
                } else {
                    pending_g = true;
                }
                break;
            }
            case 'Y': {
                HandleMarkOperation(YANKED);
                break;
            }
            case 'X': {
                HandleMarkOperation(CUT);
                break;
            }
            case 'P': {
                if (mark_status == YANKED) {
                    HandlePaste(0); // Paste (copy)
                }
                else if (mark_status == CUT) {
                    HandlePaste(1); // Paste (move)
                }
                break;
            }
            case 'D': {
                if (current_directory_file_count == 0) break;

                // marked_files_count == 0 matches HandleMarkOperation: after a
                // Ctrl+A in which every entry was rejected by GetFilePath,
                // mark_directory is set but the set is empty, and without this
                // 'd' took the "marks exist here" path and did nothing at all
                if (!IsMarkDirectorySet() || !MarkDirEqualToCurrentDir() ||
                    marked_files_count == 0 || implicit_mark) {
                    ClearMarkedFiles();
                    strcpy(mark_directory, current_directory);
                    ToggleMark();
                    implicit_mark = true;
                }
                ConfirmDelete();
                break;
            }
            case 'O': {
                HandleOldHistory();
                break;
            }
            case VK_SPACE: {
                if (current_directory_file_count == 0) break;

                if (!IsMarkDirectorySet() || !MarkDirEqualToCurrentDir()) {
                    ClearMarkedFiles();
                    strcpy(mark_directory, current_directory);
                }

                ToggleMark();
                implicit_mark = false; // curated by hand from here on
                // Advance, but do not wrap the way 'j' does: coming back
                // around to row 0 would toggle marks the user just made
                // back off, one per keypress, until the set was gone
                if (selected_row < current_directory_file_count - 1) {
                    ModifySelectedRow(1);
                }
                break;
            }
            case VK_RETURN: {
                if (current_directory_file_count == 0) break;

                if (!IsDirectory(&current_directory_files[selected_row])) {
                    OpenFileInEditor();
                }
                break;
            }
            case VK_ESCAPE: {
                ClearMarkedFiles();
                break;
            }
            case VK_OEM_3: {
                JumpToHome(); // '`'
                break;
            }
            case VK_OEM_PERIOD: {
                // Toggle visibility of hidden files; both panes reload so
                // they stay consistent
                show_hidden = !show_hidden;
                ReloadCurrentDirectory();
                if (parent_directory[0] != '\0') {
                    LoadParentDirectory();
                }
                break;
            }
            case 'V': {
                HandleOpenInEditor();
                break;
            }
            case 'C': {
                EnterClaudeMode();
                break;
            }
            case 'Q': {
                return 0; // Exit
            }
        }
    }
    return 1;
}

void OpenFileInEditor() {
    char file_path[MAX_PATH];
    if (!GetSelectedRowPath(selected_row, file_path)) {
        return; // mangled or over-long name -- could act on the wrong file
    }

    // Resolve vim only from explicit absolute PATH entries. Naming the
    // executable outright makes CreateProcess search nothing at all, while
    // empty or relative PATH entries cannot reintroduce the workspace or the
    // directory drift was launched from as an executable source.
    char vim_exe[MAX_PATH];
    bool have_vim = ResolveVim(vim_exe);

    char command[MAX_PATH + 16];
    snprintf(command, sizeof(command), "vim \"%s\"", file_path);

    // Temporarily switch to the original buffer and restore the console mode
    // for the child process
    SetConsoleActiveScreenBuffer(hOriginal);
    SetConsoleMode(hIn, original_console_mode);

    // CreateProcess instead of system(): avoids cmd.exe %VAR% expansion
    // inside quoted paths
    STARTUPINFO si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;

    // current_directory as the child's working directory: drift never calls
    // SetCurrentDirectory, so the process cwd is wherever it was launched, and
    // the editor's :pwd, relative :e and file-browsing plugins would all
    // resolve there rather than in the directory on screen. LaunchClaudeIn
    // anchors its child the same way
    if (have_vim && CreateProcess(vim_exe, command, NULL, NULL, FALSE, 0, NULL,
                                  current_directory, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        // vim not found -- fall back to the default application
        ShellExecute(NULL, "open", file_path, NULL, NULL, SW_SHOWNORMAL);
    }

    SetConsoleMode(hIn, (original_console_mode & ~ENABLE_PROCESSED_INPUT) | ENABLE_WINDOW_INPUT);
    SetConsoleActiveScreenBuffer(hAlt);

    // The editor may have written new files
    ReloadCurrentDirectory();
    if (parent_directory[0] != '\0') {
        LoadParentDirectory();
    }
}

// Solutions directly inside `dir`, one level only. A solution lives at the root
// of the thing it builds, and drift is a browser -- if it is one level down,
// step into that folder and press v again rather than have this guess.
int FindSolutionsIn(const char* dir, char names[][MAX_PATH], int max) {
    if (dir == NULL || dir[0] == '\0' || max <= 0) return 0;
    char search[MAX_PATH];
    if (snprintf(search, MAX_PATH, "%s\\*.sln", dir) >= MAX_PATH) return 0;

    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // A wildcard pattern also matches against the 8.3 short name, so
        // "*.sln" can return a .slnx or .sln-backup. Confirm the real extension
        const char* ext = strrchr(fd.cFileName, '.');
        if (ext == NULL || _stricmp(ext, ".sln") != 0) continue;
        // Same contract as GetFilePath: '?' and '*' mean FindFirstFileA
        // mangled a name it could not represent in the ANSI codepage, and the
        // mangled spelling would name the wrong file
        if (strpbrk(fd.cFileName, "?*") != NULL) continue;
        if (snprintf(names[count], MAX_PATH, "%s\\%s", dir, fd.cFileName) >= MAX_PATH) {
            continue;
        }
        count++;
    } while (FindNextFile(hFind, &fd) && count < max);
    FindClose(hFind);
    return count;
}

static const char* LeafName(const char* path) {
    const char* slash = strrchr(path, '\\');
    // A drive root keeps its separator, so its "leaf" is the empty string --
    // name the root itself rather than head a popup with nothing
    return (slash != NULL && slash[1] != '\0') ? slash + 1 : path;
}

// `path` is a folder or a .code-workspace file -- VS Code takes either as its
// single positional argument, which is the whole reason the workspace case can
// be one invocation rather than a sequence racing --add against the last
// active window
static bool OpenPathInVsCode(const char* path, const char* working_dir) {
    ClaudeLauncher launcher;
    if (!ResolveVsCode(&launcher)) return false;
    char argument[MAX_PATH + 4];
    if (snprintf(argument, sizeof(argument), "\"%s\"", path) >=
        (int)sizeof(argument)) {
        return false;
    }
    ClaudeProcessSpec spec;
    if (!BuildLauncherProcessSpec(&launcher, argument, &spec)) return false;
    return SpawnDetached(&spec, working_dir);
}

static bool OpenFolderInVsCode(const char* folder) {
    return OpenPathInVsCode(folder, folder);
}

// Three rungs, best first. The version selector opens each solution in the
// Visual Studio it was written for; devenv from PATH is one fixed install for
// all of them; the association is whatever a double-click would do. A machine
// missing the first two still works, which is the point of having all three.
static bool OpenSolutionInVisualStudio(const char* solution) {
    char argument[MAX_PATH + 4];
    if (snprintf(argument, sizeof(argument), "\"%s\"", solution) >=
        (int)sizeof(argument)) {
        return false;
    }

    ClaudeLauncher launcher;
    ClaudeProcessSpec spec;
    if (ResolveVisualStudioLauncher(&launcher) &&
        BuildLauncherProcessSpec(&launcher, argument, &spec) &&
        SpawnDetached(&spec, NULL)) {
        return true;
    }
    if (ResolveDevenv(&launcher) &&
        BuildLauncherProcessSpec(&launcher, argument, &spec) &&
        SpawnDetached(&spec, NULL)) {
        return true;
    }
    // ShellExecute returns a value above 32 on success -- the small returns are
    // the legacy WinExec error codes, not a handle
    return (INT_PTR)ShellExecute(NULL, "open", solution, NULL, NULL,
                                 SW_SHOWNORMAL) > 32;
}

// Frame, fill and edges for the small centered popups this verb uses. The
// buffer is the caller's own allocation, sized from popup_h -- not the shared
// frame buffer -- so popup_h is the only bound on the rows written here.
static void DrawEditorPopupFrame(CHAR_INFO* popup, int popup_w, int popup_h) {
    for (int i = 0; i < popup_w * popup_h; i++) {
        popup[i].Char.UnicodeChar = L' ';
        popup[i].Attributes = white;
    }
    popup[0].Char.UnicodeChar = BOX_TOP_LEFT;
    popup[popup_w - 1].Char.UnicodeChar = BOX_TOP_RIGHT;
    int bottom = (popup_h - 1) * popup_w;
    popup[bottom].Char.UnicodeChar = BOX_BOTTOM_LEFT;
    popup[bottom + popup_w - 1].Char.UnicodeChar = BOX_BOTTOM_RIGHT;
    for (int c = 1; c < popup_w - 1; c++) {
        popup[c].Char.UnicodeChar = BOX_HORIZONTAL;
        popup[bottom + c].Char.UnicodeChar = BOX_HORIZONTAL;
    }
    for (int r = 1; r < popup_h - 1; r++) {
        popup[r * popup_w].Char.UnicodeChar = BOX_VERTICAL;
        popup[r * popup_w + popup_w - 1].Char.UnicodeChar = BOX_VERTICAL;
    }
}

// One selectable row: what it opens, and a dimmed note about what it will act
// on -- the folder's name, or the solution's
#define EDITOR_MENU_MAX (MAX_SOLUTIONS > 2 ? MAX_SOLUTIONS : 2)
typedef struct {
    char label[64];
    char detail[64];
} EditorMenuItem;

// A menu driven the way the rest of drift is: j/k or the arrow keys move,
// Enter or l opens, Esc or q backs out. Returns the chosen index, or -1.
//
// Repainted every pass rather than drawn once, both because the selection bar
// moves and because the console reflows on a resize -- so unlike the delete
// prompt, which must fail closed rather than leave Y armed behind a wiped
// screen, this one can simply put itself back. Nothing here destroys anything.
static int RunEditorMenu(const char* title, const EditorMenuItem* items,
                         int count, const char* note) {
    if (count <= 0) return -1;
    const int popup_w = 52;
    int popup_h = count + (note != NULL ? 1 : 0) + 6;

    CHAR_INFO* popup = (CHAR_INFO*)malloc(popup_w * popup_h * sizeof(CHAR_INFO));
    if (popup == NULL) return -1;

    int selected = 0;
    int chosen = -1;
    while (1) {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (!GetConsoleScreenBufferInfo(hAlt, &info)) break;
        int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
        int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;
        // Shrunk too far to show the menu: back out rather than leave keys
        // live behind nothing
        if (screen_width < popup_w || screen_height < popup_h) break;

        DrawEditorPopupFrame(popup, popup_w, popup_h);
        WriteToBuffer(popup, popup_w, 1, 2, title, yellow);

        for (int i = 0; i < count; i++) {
            int row = 3 + i;
            WORD attr = white;
            WORD detail_attr = gray;
            if (i == selected) {
                // Black on the silver bar, matching the session list -- and the
                // dimmed detail has to come up to it or it vanishes
                attr = bar_background;
                detail_attr = bar_background;
                for (int c = 1; c < popup_w - 1; c++) {
                    popup[row * popup_w + c].Attributes = bar_background;
                }
            }
            WriteToBuffer(popup, popup_w, row, 3, items[i].label, attr);
            if (items[i].detail[0] != '\0') {
                WriteToBuffer(popup, popup_w, row, 21, items[i].detail, detail_attr);
            }
        }
        if (note != NULL) {
            WriteToBuffer(popup, popup_w, 3 + count, 3, note, gray);
        }
        WriteToBuffer(popup, popup_w, popup_h - 2, 3,
                      "j/k move   Enter open   Esc cancel", gray);

        int start_col = info.srWindow.Left + (screen_width - popup_w) / 2;
        int start_row = info.srWindow.Top + (screen_height - popup_h) / 2;
        COORD buffer_size = { (SHORT)popup_w, (SHORT)popup_h };
        COORD origin = { 0, 0 };
        SMALL_RECT region = { (SHORT)start_col, (SHORT)start_row,
                              (SHORT)(start_col + popup_w - 1),
                              (SHORT)(start_row + popup_h - 1) };
        if (!WriteConsoleOutputW(hAlt, popup, buffer_size, origin, &region)) break;

        INPUT_RECORD input;
        DWORD events;
        if (!ReadConsoleInput(hIn, &input, 1, &events)) break;
        if (input.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            DrawScreen(); // restore what the menu sits on top of
            continue;     // the loop re-measures and repaints above
        }
        if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) continue;

        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
        if (vk == VK_ESCAPE || vk == 'Q') break;
        if (vk == VK_DOWN || vk == 'J') {
            selected = (selected + 1) % count; // wraps, as j does in the list
        } else if (vk == VK_UP || vk == 'K') {
            selected = (selected + count - 1) % count;
        } else if (vk == VK_RETURN || vk == 'L') {
            chosen = selected;
            break;
        } else {
            // Digit accelerators stay, so a long solution list is still one
            // keystroke away rather than a scroll
            char ch = input.Event.KeyEvent.uChar.AsciiChar;
            if (ch >= '1' && ch < '1' + count) {
                chosen = ch - '1';
                break;
            }
        }
    }

    free(popup);
    return chosen;
}

// Second step, only when one directory holds several solutions
static bool PickSolution(char solutions[][MAX_PATH], int count, char out[MAX_PATH]) {
    EditorMenuItem items[EDITOR_MENU_MAX];
    if (count > EDITOR_MENU_MAX) count = EDITOR_MENU_MAX;
    for (int i = 0; i < count; i++) {
        snprintf(items[i].label, sizeof(items[i].label), "%s",
                 LeafName(solutions[i]));
        items[i].detail[0] = '\0';
    }
    int chosen = RunEditorMenu("Which solution?", items, count, NULL);
    if (chosen < 0) return false;
    snprintf(out, MAX_PATH, "%s", solutions[chosen]);
    return true;
}

// 'v' in the workspace list: open the whole workspace as one VS Code window.
// The folder set is already curated in settings.json, and a multi-root
// workspace is the same idea in VS Code's own vocabulary, so this is a format
// translation rather than a second source of truth.
void HandleOpenWorkspaceInEditor() {
    if (current_directory_file_count == 0) return;
    WIN32_FIND_DATA* sel = &current_directory_files[selected_row];
    if (!IsDirectory(sel)) return;

    char anchor[MAX_PATH];
    if (snprintf(anchor, MAX_PATH, "%s\\%s", workspaces_root, sel->cFileName) >= MAX_PATH) {
        return;
    }
    char display[MAX_PATH];
    WorkspaceDisplayName(sel->cFileName, display, sizeof(display));

    // Safe to load into the shared member globals here: edit mode owns them
    // while it is armed, and it always leaves CM_WORKSPACES to arm itself
    LoadMembersFrom(anchor);
    if (json_block_reason != NULL) {
        NotifyAndWait("that workspace's settings cannot be read safely -- press a key");
        return;
    }
    if (member_count == 0) {
        NotifyAndWait("that workspace has no folders yet -- press e to add some");
        return;
    }

    char file[MAX_PATH];
    if (!WriteCodeWorkspaceFile(anchor, display, file)) {
        NotifyAndWait("the VS Code workspace file could not be written -- press a key");
        return;
    }
    if (!OpenPathInVsCode(file, anchor)) {
        NotifyAndWait("VS Code was not found on PATH -- press a key");
        return;
    }
    char msg[MAX_PATH + 32];
    snprintf(msg, sizeof(msg), "Opening %s in VS Code...", display);
    ShowStatusBanner(msg);
}

// 'v': open the thing under the cursor in a real editor. The menu adapts to
// what is actually there, so the ordinary case -- one solution, or none -- is
// v then a single key with no second prompt.
void HandleOpenInEditor() {
    if (current_directory_file_count == 0) return;
    WIN32_FIND_DATA* sel = &current_directory_files[selected_row];

    // A directory under the cursor is the subject; on a file it is the
    // directory being browsed, since "open this folder" is still the useful
    // answer when the cursor happens to be resting on a README
    char folder[MAX_PATH];
    char solutions[MAX_SOLUTIONS][MAX_PATH];
    int solution_count = 0;
    if (IsDirectory(sel)) {
        if (!GetSelectedRowPath(selected_row, folder)) return;
        solution_count = FindSolutionsIn(folder, solutions, MAX_SOLUTIONS);
    } else {
        snprintf(folder, sizeof(folder), "%s", current_directory);
        // The cursor resting on a solution names it outright -- no reason to
        // scan for it, or to offer the others beside it
        const char* ext = strrchr(sel->cFileName, '.');
        if (ext != NULL && _stricmp(ext, ".sln") == 0 &&
            GetSelectedRowPath(selected_row, solutions[0])) {
            solution_count = 1;
        } else {
            solution_count = FindSolutionsIn(folder, solutions, MAX_SOLUTIONS);
        }
    }

    EditorMenuItem items[EDITOR_MENU_MAX];
    int item_count = 0;
    snprintf(items[item_count].label, sizeof(items[0].label), "VS Code");
    snprintf(items[item_count].detail, sizeof(items[0].detail), "folder");
    item_count++;
    if (solution_count == 1) {
        snprintf(items[item_count].label, sizeof(items[0].label), "Visual Studio");
        snprintf(items[item_count].detail, sizeof(items[0].detail), "%s",
                 LeafName(solutions[0]));
        item_count++;
    } else if (solution_count > 1) {
        snprintf(items[item_count].label, sizeof(items[0].label), "Visual Studio");
        snprintf(items[item_count].detail, sizeof(items[0].detail),
                 "%d solutions", solution_count);
        item_count++;
    }

    char title[80];
    snprintf(title, sizeof(title), "Open %s in:", LeafName(folder));
    int chosen = RunEditorMenu(title, items, item_count,
                               solution_count == 0 ? "(no solution here)" : NULL);
    if (chosen < 0) return;

    if (chosen == 0) {
        if (!OpenFolderInVsCode(folder)) {
            NotifyAndWait("VS Code was not found on PATH -- press a key");
            return;
        }
        ShowStatusBanner("Opening in VS Code...");
        return;
    }

    char solution[MAX_PATH];
    if (solution_count == 1) {
        snprintf(solution, sizeof(solution), "%s", solutions[0]);
    } else if (!PickSolution(solutions, solution_count, solution)) {
        return; // cancelled at the second menu
    }
    if (!OpenSolutionInVisualStudio(solution)) {
        NotifyAndWait("Visual Studio could not be started -- press a key");
        return;
    }
    ShowStatusBanner("Opening in Visual Studio...");
}

void HandleMarkOperation(enum MarkStatus new_status) {
    if (current_directory_file_count == 0) return;

    // If marks already exist in this directory, operate on them as-is.
    // Otherwise mark the cursor line. (No special case for a single mark --
    // that previously cleared the marked file and yanked the cursor line
    // instead.)
    if (!IsMarkDirectorySet() || !MarkDirEqualToCurrentDir() ||
        marked_files_count == 0 || implicit_mark) {
        ClearMarkedFiles();
        strcpy(mark_directory, current_directory);
        ToggleMark();
        implicit_mark = true;
    }
    SetMarkStatus(new_status);
}

// Builds a double-null-terminated list of all marked paths for
// SHFileOperation. Caller frees. Batching into one call gives a single
// operation (and a single undo unit) instead of one per file.
char* BuildFromList(size_t* out_size) {
    size_t size = 1; // trailing extra null
    for (int i = 0; i < marked_files_count; i++) {
        size += strlen(marked_files[i].path) + 1;
    }

    char* from = (char*)malloc(size);
    if (from == NULL) return NULL;

    size_t pos = 0;
    for (int i = 0; i < marked_files_count; i++) {
        size_t len = strlen(marked_files[i].path);
        memcpy(from + pos, marked_files[i].path, len + 1);
        pos += len + 1;
    }
    from[pos] = '\0';

    if (out_size) *out_size = size;
    return from;
}

void HandlePaste(int move) {
    if (marked_files_count == 0) return;

    // Moving files into their own directory is a no-op
    if (move && MarkDirEqualToCurrentDir()) {
        ClearMarkedFiles();
        return;
    }

    char* from = BuildFromList(NULL);
    if (from == NULL) return;

    // Destination is the directory itself, double-null-terminated
    char to[MAX_PATH + 2];
    strcpy(to, current_directory);
    to[strlen(to) + 1] = '\0';

    char banner[64];
    snprintf(banner, sizeof(banner), "%s %d item(s)...", move ? "Moving" : "Copying", marked_files_count);
    ShowStatusBanner(banner);

    SHFILEOPSTRUCT op = {0};
    op.wFunc = move ? FO_MOVE : FO_COPY;
    op.pFrom = from;
    op.pTo = to;
    // FOF_RENAMEONCOLLISION: name collisions produce "Copy of ..." instead of
    // silently overwriting the existing file. FOF_SILENT is deliberately not
    // set: long operations get the shell's progress dialog (with cancel)
    // instead of looking like a hang
    op.fFlags = FOF_NOCONFIRMATION | FOF_RENAMEONCOLLISION;

    int result = SHFileOperation(&op);
    free(from);

    // Keys mashed while the operation blocked the input loop would replay
    // as commands afterwards -- discard them
    FlushConsoleInputBuffer(hIn);

    ReloadCurrentDirectory(); // partial work may have happened either way
    if (parent_directory[0] != '\0') {
        LoadParentDirectory(); // a move out of the parent leaves it listing ghosts
    }

    // Failure keeps the marks, same as cancelling the delete popup -- the
    // user built that set on purpose. (Windows shows its own error dialog;
    // FOF_NOERRORUI is unset.)
    if (result == 0 && !op.fAnyOperationsAborted) {
        ClearMarkedFiles();
    } else if (move) {
        // Keeping the set is right, but a cancelled or partly-failed move
        // leaves some of it naming files that are already gone. Retrying would
        // raise a shell error per missing source, and 'd' would open a delete
        // prompt listing them. Keep what is still there, drop what is not
        int kept = 0;
        for (int i = 0; i < marked_files_count; i++) {
            if (GetFileAttributes(marked_files[i].path) != INVALID_FILE_ATTRIBUTES) {
                marked_files[kept++] = marked_files[i];
            }
        }
        marked_files_count = kept;
        if (marked_files_count == 0) {
            ClearMarkedFiles(); // nothing left -- also drop mark_directory
        }
    }
}

void ToggleMark() {
    if (current_directory_file_count == 0) return;

    char full_path[MAX_PATH];
    if (!GetFilePath(current_directory, &current_directory_files[selected_row], full_path)) {
        return; // path too deep to operate on safely
    }

    // Match by path so marks stay consistent across reloads
    for (int i = 0; i < marked_files_count; i++) {
        if (strcmp(marked_files[i].path, full_path) == 0) {
            // Unmark
            for (int j = i; j < marked_files_count - 1; j++) {
                marked_files[j] = marked_files[j + 1];
            }
            marked_files_count--;

            if (marked_files_count == 0) {
                ClearMarkedFiles();
            }
            return;
        }
    }

    // Mark
    if (!MarkedFilesFull()) {
        strcpy(marked_files[marked_files_count].path, full_path);
        marked_files_count++;

        strcpy(mark_directory, current_directory);
    }
}

void ChangeCurrentDirectory(char* path) {
    SaveDirectoryState();

    strcpy(current_directory, path);
    LoadCurrentDirectory();
    if (!IsRootDirectory(current_directory)) {
        LoadParentDirectory();
    } else {
        parent_directory[0] = '\0';
        parent_directory_file_count = 0;
    }

    RestoreDirectoryState();
}

// DRIFT_HOME overrides USERPROFILE so wrappers can redirect the home jump
// (under Wine, USERPROFILE is the prefix's fake profile dir, not the real one)
void JumpToHome() {
    char home[MAX_PATH];
    DWORD len = GetEnvironmentVariable("DRIFT_HOME", home, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        len = GetEnvironmentVariable("USERPROFILE", home, MAX_PATH);
    }
    if (len > 0 && len < MAX_PATH) {
        ChangeCurrentDirectory(home);
    }
}

void GetParentDirectory(char* path, char* parent) {
    strcpy(parent, path);

    // Drop a trailing separator first, or strrchr finds that one and hands
    // back the directory itself as its own parent. Stops above "C:\", which
    // is a root and keeps its slash
    size_t len = strlen(parent);
    while (len > 3 && parent[len - 1] == '\\') {
        parent[--len] = '\0';
    }

    char* last_slash = strrchr(parent, '\\');
    if (last_slash == NULL) {
        // No slash found, treat as root
        parent[0] = '\0';
    } else if (last_slash == parent + 2 && parent[1] == ':') {
        // Handle root directory case (e.g., "C:\")
        *(last_slash + 1) = '\0';
    }
    else {
        *last_slash = '\0';
    }
}

int CompareFiles(const void* a, const void* b) {
    const WIN32_FIND_DATA* fa = (const WIN32_FIND_DATA*)a;
    const WIN32_FIND_DATA* fb = (const WIN32_FIND_DATA*)b;

    bool dir_a = (fa->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool dir_b = (fb->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    // Directories first, then case-insensitive by name
    if (dir_a != dir_b) {
        return dir_a ? -1 : 1;
    }
    return _stricmp(fa->cFileName, fb->cFileName);
}

int GetFilesInDirectory(char* path, WIN32_FIND_DATA files[]) {
    if (path[0] == '\0') {
        return 0; // "" would search "\*", i.e. the root of the current drive
    }

    char search_path[MAX_PATH];
    if (snprintf(search_path, MAX_PATH, "%s\\*", path) >= MAX_PATH) {
        return 0; // would truncate -- don't list a different directory
    }

    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search_path, &fd);

    int count = 0;

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0) continue;
            if (strcmp(fd.cFileName, "..") == 0) continue;
            if (!show_hidden && (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;

            files[count] = fd;
            count++;
        } while (FindNextFile(hFind, &fd) && count < MAX_FILES);

        FindClose(hFind);
    }

    qsort(files, count, sizeof(WIN32_FIND_DATA), CompareFiles);

    return count;
}

void ModifySelectedRow(int num) {
    if (current_directory_file_count == 0) {
        selected_row = 0;
        top_row = 0;
        return;
    }

    int target_row = selected_row + num;
    int visible_rows = GetVisibleRows();

    if (target_row >= current_directory_file_count) {
        if (num == 1) {
            selected_row = 0;
            top_row = 0;
        } else {
            selected_row = current_directory_file_count - 1;
            top_row = current_directory_file_count - visible_rows;
            if (top_row < 0) {
                top_row = 0;
            }
        }
        return;
    }

    if (target_row < 0) {
        if (num == -1) {
            selected_row = current_directory_file_count - 1;
            top_row = current_directory_file_count - visible_rows;
            if (top_row < 0) {
                top_row = 0;
            }
        } else {
            selected_row = 0;
            top_row = 0;
        }
        return;
    }

    if (target_row >= top_row + visible_rows) {
        top_row = target_row - visible_rows + 1;
        selected_row = target_row;
        return;
    }

    if (target_row < top_row) {
        top_row = target_row;
        selected_row = target_row;
        return;
    }

    selected_row = target_row;
}


void SaveDirectoryState() {
    // The startup call arrives before current_directory is ever set -- don't
    // record a permanent empty entry in history
    if (current_directory[0] == '\0') return;

    // check if the current directory is already in history
    for (int i = 0; i < history_count; i++) {
        if (strcmp(history[i].path, current_directory) == 0) {
            history[i].selected_row = selected_row;
            history[i].top_row = top_row;
            return;
        }
    }

    // Add new entry to history
    if (history_count < MAX_FILES) {
        strcpy(history[history_count].path, current_directory);
        history[history_count].selected_row = selected_row;
        history[history_count].top_row = top_row;
        history_count++;
    }
}

void RestoreDirectoryState() {
    for (int i = 0; i < history_count; i++) {
        if (strcmp(history[i].path, current_directory) == 0) {
            selected_row = history[i].selected_row;
            top_row = history[i].top_row;

            if (current_directory_file_count == 0 || selected_row >= current_directory_file_count) {
                selected_row = 0;
                top_row = 0;
            }

            return;
        }
    }

    // If not found, reset to defaults
    selected_row = 0;
    top_row = 0;
}

void ConfirmDelete() {
    if (marked_files_count == 0) return;

    // Painted from inside the loop so a resize can repaint it. The console
    // reflows on resize and wipes the popup, and this loop leaves 'Y' armed --
    // so a prompt that was still live used to be sitting there invisible,
    // over a screen that was not redrawn either
    bool repaint = true;
    while (1) {
        if (repaint) {
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
            int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
            int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

            // Shrunk too far to show the prompt: cancel rather than stay armed
            // behind nothing. Marks are kept, as with any other cancel
            if (screen_width < POPUP_WIDTH || screen_height < POPUP_HEIGHT) return;

            CHAR_INFO* popup_buffer = (CHAR_INFO*)malloc(POPUP_WIDTH * POPUP_HEIGHT * sizeof(CHAR_INFO));
            if (popup_buffer == NULL) return;
            DrawDeletePopup(POPUP_WIDTH, POPUP_HEIGHT, popup_buffer);

            // Center the popup on screen
            int start_col = info.srWindow.Left + (screen_width - POPUP_WIDTH) / 2;
            int start_row = info.srWindow.Top + (screen_height - POPUP_HEIGHT) / 2;

            COORD buffer_size = { POPUP_WIDTH, POPUP_HEIGHT };
            COORD origin = { 0, 0 };
            SMALL_RECT region = { (SHORT)start_col, (SHORT)start_row,
                                  (SHORT)(start_col + POPUP_WIDTH - 1), (SHORT)(start_row + POPUP_HEIGHT - 1) };
            WriteConsoleOutputW(hAlt, popup_buffer, buffer_size, origin, &region);

            free(popup_buffer);
            repaint = false;
        }

        INPUT_RECORD input;
        DWORD events;
        if (!ReadConsoleInput(hIn, &input, 1, &events)) {
            break; // Treat console failure as cancel
        }

        if (input.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            DrawScreen(); // restore what the popup sits on top of
            repaint = true;
            continue;
        }

        if (input.EventType == KEY_EVENT && input.Event.KeyEvent.bKeyDown) {
            if (input.Event.KeyEvent.wVirtualKeyCode == 'Y') {
                char* from = BuildFromList(NULL);
                if (from != NULL) {
                    char banner[64];
                    snprintf(banner, sizeof(banner), "Deleting %d item(s)...", marked_files_count);
                    ShowStatusBanner(banner);

                    SHFILEOPSTRUCT op = {0};
                    op.wFunc = FO_DELETE;
                    op.pFrom = from;
                    // FOF_WANTNUKEWARNING: on volumes with no recycle bin
                    // (network shares, some removable drives) still warn
                    // before permanently destroying files -- the popup
                    // promises "Move to Recycle Bin". FOF_SILENT unset: long
                    // recycles get the shell's progress dialog
                    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING;

                    int result = SHFileOperation(&op);
                    free(from);

                    // Discard keys mashed while the operation blocked input
                    FlushConsoleInputBuffer(hIn);

                    // Failure keeps the marks, same as cancelling
                    if (result == 0 && !op.fAnyOperationsAborted) {
                        ClearMarkedFiles();
                    }
                }

                ReloadCurrentDirectory();
                break;
            } else if (input.Event.KeyEvent.wVirtualKeyCode == 'N' ||
                       input.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) {
                // Cancelling keeps the marks -- the user built that set on purpose
                break;
            }
        }
    }
}

void DrawDeletePopup(int width, int height, CHAR_INFO* out_buffer) {
    // 1. Fill background with spaces
    for (int i = 0; i < width * height; i++) {
        out_buffer[i].Char.UnicodeChar = L' ';
        out_buffer[i].Attributes = white;
    }

    // 2. Draw border - corners
    out_buffer[0].Char.UnicodeChar = BOX_TOP_LEFT;
    out_buffer[width - 1].Char.UnicodeChar = BOX_TOP_RIGHT;
    int bottom = (height - 1) * width;
    out_buffer[bottom].Char.UnicodeChar = BOX_BOTTOM_LEFT;
    out_buffer[bottom + width - 1].Char.UnicodeChar = BOX_BOTTOM_RIGHT;

    // Top and bottom edges
    for (int col = 1; col < width - 1; col++) {
        out_buffer[col].Char.UnicodeChar = BOX_HORIZONTAL;
        out_buffer[bottom + col].Char.UnicodeChar = BOX_HORIZONTAL;
    }

    // Left and right edges
    for (int row = 1; row < height - 1; row++) {
        out_buffer[row * width].Char.UnicodeChar = BOX_VERTICAL;
        out_buffer[row * width + width - 1].Char.UnicodeChar = BOX_VERTICAL;
    }

    // 3. Title (row 1, centered)
    char* title = "Confirm Delete?";
    int title_col = (width - (int)strlen(title)) / 2;
    WriteToBuffer(out_buffer, width, 1, title_col, title, red);

    // 4. Item count (row 3)
    char count_msg[50];
    snprintf(count_msg, sizeof(count_msg), "%d item(s) will be deleted:", marked_files_count);
    WriteToBuffer(out_buffer, width, 3, 2, count_msg, white);

    // 5. File names (rows 5-9, max 5 items)
    int max_display = 5;
    for (int i = 0; i < marked_files_count && i < max_display; i++) {
        char* name = strrchr(marked_files[i].path, '\\');
        if (name) {
            name++;
        } else {
            name = marked_files[i].path;
        }
        WriteToBuffer(out_buffer, width, 5 + i, 4, name, white);
    }

    // 6. "...and N more" if needed
    if (marked_files_count > max_display) {
        char more_msg[30];
        snprintf(more_msg, sizeof(more_msg), "...and %d more", marked_files_count - max_display);
        WriteToBuffer(out_buffer, width, 5 + max_display, 4, more_msg, white);
    }

    // 7. Y/N options (bottom rows)
    WriteToBuffer(out_buffer, width, height - 4, 2, "[Y] Yes - Move to Recycle Bin", white);
    WriteToBuffer(out_buffer, width, height - 3, 2, "[N] No - Cancel", white);
}

// `cp` is the encoding of input_text only: the label and the placeholder are
// generated here and are always ASCII. Session titles come from a UTF-8
// transcript, while typed and filesystem-derived names are the ANSI codepage
void DrawCreatePopup(int width, char* input_text, const char* placeholder, CHAR_INFO* out_buffer, UINT cp) {
    int height = 3;

    // Fill background
    for (int i = 0; i < width * height; i++) {
        out_buffer[i].Char.UnicodeChar = L' ';
        out_buffer[i].Attributes = white;
    }

    // Top border
    out_buffer[0].Char.UnicodeChar = BOX_TOP_LEFT;
    for (int col = 1; col < width - 1; col++) {
        out_buffer[col].Char.UnicodeChar = BOX_HORIZONTAL;
    }
    out_buffer[width - 1].Char.UnicodeChar = BOX_TOP_RIGHT;

    // Middle row - edges
    out_buffer[width].Char.UnicodeChar = BOX_VERTICAL;
    out_buffer[width * 2 - 1].Char.UnicodeChar = BOX_VERTICAL;

    // Bottom border
    int bottom = 2 * width;
    out_buffer[bottom].Char.UnicodeChar = BOX_BOTTOM_LEFT;
    for (int col = 1; col < width - 1; col++) {
        out_buffer[bottom + col].Char.UnicodeChar = BOX_HORIZONTAL;
    }
    out_buffer[bottom + width - 1].Char.UnicodeChar = BOX_BOTTOM_RIGHT;

    // Label and input. When the field is empty and a placeholder was given,
    // show it greyed out -- pressing Enter accepts it as the default name
    WriteToBuffer(out_buffer, width, 1, 2, "Name: ", white);
    if (input_text[0] == '\0' && placeholder != NULL) {
        WriteToBuffer(out_buffer, width, 1, 8, placeholder, gray);
    } else {
        WriteToBufferCP(out_buffer, width, 1, 8, input_text, white, cp);
    }
}

// Paint a small centered banner right before a blocking shell operation so
// the console shows what is happening (the shell's own progress dialog only
// appears once the operation has run for a moment); the next DrawScreen
// erases it
void ShowStatusBanner(const char* text) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

    int height = 3;
    int width = (int)strlen(text) + 4;
    if (width > screen_width) width = screen_width;
    if (width < 6 || screen_height < height) return;

    CHAR_INFO* banner = (CHAR_INFO*)malloc(width * height * sizeof(CHAR_INFO));
    if (banner == NULL) return;

    for (int i = 0; i < width * height; i++) {
        banner[i].Char.UnicodeChar = L' ';
        banner[i].Attributes = white;
    }

    // Border (same 3-row frame as the create popup)
    banner[0].Char.UnicodeChar = BOX_TOP_LEFT;
    banner[width - 1].Char.UnicodeChar = BOX_TOP_RIGHT;
    int bottom = 2 * width;
    banner[bottom].Char.UnicodeChar = BOX_BOTTOM_LEFT;
    banner[bottom + width - 1].Char.UnicodeChar = BOX_BOTTOM_RIGHT;
    for (int col = 1; col < width - 1; col++) {
        banner[col].Char.UnicodeChar = BOX_HORIZONTAL;
        banner[bottom + col].Char.UnicodeChar = BOX_HORIZONTAL;
    }
    banner[width].Char.UnicodeChar = BOX_VERTICAL;
    banner[width * 2 - 1].Char.UnicodeChar = BOX_VERTICAL;

    WriteToBuffer(banner, width, 1, 2, text, white);

    int start_col = info.srWindow.Left + (screen_width - width) / 2;
    int start_row = info.srWindow.Top + (screen_height - height) / 2;
    COORD buffer_size = { (SHORT)width, (SHORT)height };
    COORD origin = { 0, 0 };
    SMALL_RECT region = { (SHORT)start_col, (SHORT)start_row,
                          (SHORT)(start_col + width - 1), (SHORT)(start_row + height - 1) };
    WriteConsoleOutputW(hAlt, banner, buffer_size, origin, &region);
    free(banner);
}

void HandleCreate() {
    char name[MAX_PATH];
    name[0] = '\0';
    int pos = 0;
    bool cancelled = false;

    // New workspaces get a greyed-out date-time default so Enter alone names
    // one without typing; colons are omitted since they're illegal in a path
    char placeholder[MAX_PATH];
    const char* ph = NULL;
    if (claude_mode == CM_WORKSPACES) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(placeholder, sizeof(placeholder), "%04d-%02d-%02d_%02d-%02d-%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        ph = placeholder;
    }

    int popup_h = 3;

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

    int popup_w = CREATE_POPUP_WIDTH;
    if (popup_w > screen_width - 2) popup_w = screen_width - 2;
    if (popup_w < 14 || screen_height < popup_h) return;

    // Allocated at the maximum width, so a resize only has to recompute the
    // geometry inside the loop and never reallocate: popup_w is
    // CREATE_POPUP_WIDTH clamped down to the screen, never above it
    CHAR_INFO* popup_buffer = (CHAR_INFO*)malloc(CREATE_POPUP_WIDTH * popup_h * sizeof(CHAR_INFO));
    if (popup_buffer == NULL) return;

    // Fixed for the life of the popup even if the window is resized, so text
    // already typed can never exceed what the field still accepts
    int max_len = popup_w - 10;

    // Show the hardware cursor while typing in the name field
    CONSOLE_CURSOR_INFO cursor_info = { 25, TRUE };
    SetConsoleCursorInfo(hAlt, &cursor_info);

    while (1) {
        // Re-measured every pass rather than once before the loop: the console
        // reflows on a resize, and drawing at the original geometry would put
        // the field and its caret somewhere other than where they appear
        CONSOLE_SCREEN_BUFFER_INFO cur;
        if (!GetConsoleScreenBufferInfo(hAlt, &cur)) {
            name[0] = '\0';
            cancelled = true;
            break;
        }
        int cur_w = cur.srWindow.Right - cur.srWindow.Left + 1;
        int cur_h = cur.srWindow.Bottom - cur.srWindow.Top + 1;
        popup_w = CREATE_POPUP_WIDTH;
        if (popup_w > cur_w - 2) popup_w = cur_w - 2;
        if (popup_w < 14 || cur_h < popup_h) {
            name[0] = '\0'; // shrunk too far to show the field -- cancel
            cancelled = true;
            break;
        }
        int start_col = cur.srWindow.Left + (cur_w - popup_w) / 2;
        int start_row = cur.srWindow.Top + (cur_h - popup_h) / 2;

        // Draw popup
        DrawCreatePopup(popup_w, name, ph, popup_buffer, CP_ACP);

        COORD buffer_size = { (SHORT)popup_w, (SHORT)popup_h };
        COORD origin = { 0, 0 };
        SMALL_RECT region = { (SHORT)start_col, (SHORT)start_row,
                              (SHORT)(start_col + popup_w - 1), (SHORT)(start_row + popup_h - 1) };
        WriteConsoleOutputW(hAlt, popup_buffer, buffer_size, origin, &region);

        // Position cursor after text
        COORD cursor_pos = { (SHORT)(start_col + 8 + pos), (SHORT)(start_row + 1) };
        SetConsoleCursorPosition(hAlt, cursor_pos);

        // Get input
        INPUT_RECORD input;
        DWORD events;
        if (!ReadConsoleInput(hIn, &input, 1, &events)) {
            name[0] = '\0'; // Treat console failure as cancel, not as
            cancelled = true;
            break;          // "create with the partial name"
        }

        if (input.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            DrawScreen(); // restore what the popup sits on top of
            continue;     // the loop re-measures and repaints above
        }

        if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) {
            continue;
        }

        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
        char c = input.Event.KeyEvent.uChar.AsciiChar;

        if (vk == VK_RETURN) {
            break;
        } else if (vk == VK_ESCAPE) {
            name[0] = '\0';
            cancelled = true;
            break;
        } else if (vk == VK_BACK && pos > 0) {
            pos--;
            name[pos] = '\0';
        } else if (c >= 32 && c < 127 && pos < max_len) {
            name[pos] = c;
            pos++;
            name[pos] = '\0';
        }
    }

    free(popup_buffer);

    cursor_info.bVisible = FALSE;
    SetConsoleCursorInfo(hAlt, &cursor_info);

    // Enter on an empty field accepts the greyed date-time default
    if (!cancelled && name[0] == '\0' && ph != NULL) {
        snprintf(name, sizeof(name), "%s", placeholder);
    }

    if (name[0] == '\0') return;

    if (claude_mode == CM_WORKSPACES) {
        // The typed text is the workspace's DISPLAY name. The folder itself is
        // an opaque, always-unique timestamp id, so naming never touches the
        // path Claude keys its sessions under, and the shown-name namespace is
        // the only one the user deals with. Accepting the greyed default leaves
        // the workspace unnamed -- it just shows its timestamp id.
        bool custom = strcmp(name, placeholder) != 0;
        if (custom && WorkspaceNameTaken(name, NULL)) {
            NotifyNameTaken(name);
            return;
        }
        // Mint a unique folder id from the timestamp, suffixing on the (rare)
        // same-second clash so an existing folder is never silently reused
        char id[MAX_PATH];
        snprintf(id, sizeof(id), "%s", placeholder);
        int n = 2;
        while (1) {
            char probe[MAX_PATH];
            if (snprintf(probe, MAX_PATH, "%s\\%s", workspaces_root, id) >= MAX_PATH) return;
            if (GetFileAttributes(probe) == INVALID_FILE_ATTRIBUTES) break; // free
            if (snprintf(id, sizeof(id), "%s-%d", placeholder, n++) >= MAX_PATH) return;
        }
        char anchor[MAX_PATH];
        if (snprintf(anchor, MAX_PATH, "%s\\%s", workspaces_root, id) >= MAX_PATH) return;
        if (!CreateDirectory(anchor, NULL)) return;
        EnsureWorkspaceNotes(anchor);
        bool display_name_saved = !custom || SetWorkspaceName(id, name);
        ReloadCurrentDirectory();
        for (int i = 0; i < current_directory_file_count; i++) {
            if (_stricmp(current_directory_files[i].cFileName, id) == 0) {
                selected_row = i;
                break;
            }
        }
        if (!display_name_saved) {
            char msg[MAX_PATH + 80];
            snprintf(msg, sizeof(msg),
                     "Workspace created as \"%s\"; display name was not saved -- press a key",
                     id);
            NotifyAndWait(msg);
        }
        return;
    }

    // Strip the trailing backslash (directory marker) before building the path
    // so the name is also usable for the cursor lookup below
    int len = (int)strlen(name);
    bool is_directory = (name[len - 1] == '\\');
    if (is_directory) {
        name[len - 1] = '\0';
        if (name[0] == '\0') return;
    }

    // The name is joined onto current_directory, so anything that could aim it
    // elsewhere has to be refused: ".." creates in the parent, and a separator
    // either fails or lands somewhere this pane is not showing -- both without
    // a trace, since the cursor lookup below then finds nothing. The trailing
    // '\' directory marker was already stripped above
    if (strpbrk(name, "\\/:*?\"<>|") != NULL ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        NotifyAndWait("That name cannot be used -- press a key");
        return;
    }

    char full_path[MAX_PATH];
    if (snprintf(full_path, MAX_PATH, "%s\\%s", current_directory, name) >= MAX_PATH) {
        return; // would truncate -- could create or collide at the wrong path
    }

    BOOL created;
    DWORD err = 0;
    if (is_directory) {
        created = CreateDirectory(full_path, NULL);
        if (!created) err = GetLastError();
    } else {
        HANDLE h = CreateFile(full_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        created = h != INVALID_HANDLE_VALUE;
        if (created) {
            CloseHandle(h);
        } else {
            err = GetLastError();
        }
    }

    // A reserved device name -- CON, NUL, COM1, and the same with any
    // extension -- opens successfully and leaves nothing on disk, so the call
    // reporting success is not proof that anything was created
    if (created && GetFileAttributes(full_path) == INVALID_FILE_ATTRIBUTES) {
        created = FALSE;
        err = 0;
    }

    if (!created) {
        // Silence here read as "the key did nothing": nothing appears, and the
        // cursor hunt below finds no match either
        NotifyAndWait(err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS
                          ? "That name already exists -- press a key"
                          : "Could not create that item -- press a key");
        return;
    }

    ReloadCurrentDirectory();

    // Move the cursor to the newly created item
    for (int i = 0; i < current_directory_file_count; i++) {
        if (_stricmp(current_directory_files[i].cFileName, name) == 0) {
            selected_row = i;
            break;
        }
    }
}

int CalculateDistance(char* current, char* target) {
    // Walk current upward until it is a prefix of target, counting steps.
    // The first iteration (steps_up == 0) covers the "target is below
    // current" case.
    char temp[MAX_PATH];
    strcpy(temp, current);
    int steps_up = 0;

    while (temp[0] != '\0') {
        // Compare "C:\" as "C:" so the drive root can prefix-match
        int temp_len = (int)strlen(temp);
        if (temp_len > 0 && temp[temp_len - 1] == '\\') {
            temp_len--;
        }

        if (strncmp(temp, target, temp_len) == 0 &&
            (target[temp_len] == '\\' || target[temp_len] == '\0')) {
            // Found common ancestor, count steps down. A lone trailing
            // backslash (root "C:\") is a separator, not a step.
            int steps_down = 0;
            for (int i = temp_len; target[i] != '\0'; i++) {
                if (target[i] == '\\' && target[i + 1] != '\0') steps_down++;
            }
            return steps_up + steps_down;
        }

        // Cut off last directory; the drive root itself is still a valid
        // ancestor, so reduce to "C:\" and try once more before giving up
        char* last_slash = strrchr(temp, '\\');
        if (last_slash == NULL) break;
        if (last_slash == temp + 2 && temp[1] == ':') {
            if (temp[3] == '\0') break; // already at the drive root
            temp[3] = '\0';
        } else {
            *last_slash = '\0';
        }
        steps_up++;
    }

    return 1000; // Different drives or no common ancestor - max distance
}

void HandleOldHistory() {
    if (history_count == 0) return;

    DistanceEntry distances[MAX_FILES];

    for (int i = 0; i < history_count; i++) {
        distances[i].index = i;
        distances[i].distance = CalculateDistance(current_directory, history[i].path);
    }

    // Sort by distance (descending)
    for (int i = 0; i < history_count - 1; i++) {
        for (int j = 0; j < history_count - i - 1; j++) {
            if (distances[j].distance < distances[j + 1].distance) {
                DistanceEntry temp = distances[j];
                distances[j] = distances[j + 1];
                distances[j + 1] = temp;
            }
        }
    }

    int display_count = history_count < 10 ? history_count : 10;

    // Painted from inside the loop so a resize can repaint it: the console
    // reflows and wipes the popup, and the number keys stay live behind it
    bool repaint = true;
    while (1) {
        if (repaint) {
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
            int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
            int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

            int popup_height = display_count + 5;
            int popup_width = 60;
            if (popup_width > screen_width) popup_width = screen_width;
            // Shrunk too far to show the list: cancel rather than leave the
            // number keys live behind nothing
            if (popup_width < 10 || screen_height < popup_height) return;

            CHAR_INFO* popup_buffer = (CHAR_INFO*)malloc(popup_width * popup_height * sizeof(CHAR_INFO));
            if (popup_buffer == NULL) return;
            DrawOldHistoryPopup(popup_width, popup_height, distances, display_count, popup_buffer);

            // Center the popup on screen
            int start_col = info.srWindow.Left + (screen_width - popup_width) / 2;
            int start_row = info.srWindow.Top + (screen_height - popup_height) / 2;

            COORD buffer_size = { (SHORT)popup_width, (SHORT)popup_height };
            COORD origin = { 0, 0 };
            SMALL_RECT region = { (SHORT)start_col, (SHORT)start_row,
                                  (SHORT)(start_col + popup_width - 1), (SHORT)(start_row + popup_height - 1) };
            WriteConsoleOutputW(hAlt, popup_buffer, buffer_size, origin, &region);

            free(popup_buffer);
            repaint = false;
        }

        INPUT_RECORD input;
        DWORD events;
        if (!ReadConsoleInput(hIn, &input, 1, &events)) {
            break; // Treat console failure as cancel
        }

        if (input.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            DrawScreen(); // restore what the popup sits on top of
            repaint = true;
            continue;
        }

        if (input.EventType == KEY_EVENT && input.Event.KeyEvent.bKeyDown) {
            char c = input.Event.KeyEvent.uChar.AsciiChar;

            if (c >= '1' && c <= '9') {
                int selected = c - '1';
                if (selected < display_count) {
                    ChangeCurrentDirectory(history[distances[selected].index].path);
                    break;
                }
            }
            else if (c == '0' && display_count == 10) {
                ChangeCurrentDirectory(history[distances[9].index].path);
                break;
            }
            else if (input.Event.KeyEvent.wVirtualKeyCode == 'Q') {
                break;
            }
            else if (input.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) {
                break;
            }
        }
    }
}

void DrawOldHistoryPopup(int width, int height, DistanceEntry* distances, int display_count, CHAR_INFO* out_buffer) {
    // Fill background
    for (int i = 0; i < width * height; i++) {
        out_buffer[i].Char.UnicodeChar = L' ';
        out_buffer[i].Attributes = white;
    }

    // Draw border - corners
    out_buffer[0].Char.UnicodeChar = BOX_TOP_LEFT;
    out_buffer[width - 1].Char.UnicodeChar = BOX_TOP_RIGHT;
    int bottom = (height - 1) * width;
    out_buffer[bottom].Char.UnicodeChar = BOX_BOTTOM_LEFT;
    out_buffer[bottom + width - 1].Char.UnicodeChar = BOX_BOTTOM_RIGHT;

    // Top and bottom edges
    for (int col = 1; col < width - 1; col++) {
        out_buffer[col].Char.UnicodeChar = BOX_HORIZONTAL;
        out_buffer[bottom + col].Char.UnicodeChar = BOX_HORIZONTAL;
    }

    // Left and right edges
    for (int row = 1; row < height - 1; row++) {
        out_buffer[row * width].Char.UnicodeChar = BOX_VERTICAL;
        out_buffer[row * width + width - 1].Char.UnicodeChar = BOX_VERTICAL;
    }

    // Title
    char* title = "Visited Directories";
    int title_col = (width - (int)strlen(title)) / 2;
    // HandleOldHistory clamps the popup to the screen and only refuses below
    // 10 columns, so a narrow window centres this title at a negative column.
    // The write stays inside the buffer but starts back on the previous row,
    // painting over the top border and the left edge
    if (title_col < 1) title_col = 1;
    WriteToBuffer(out_buffer, width, 1, title_col, title, blue);

    // List entries
    for (int i = 0; i < display_count; i++) {
        char line[MAX_PATH];
        int num = (i == 9) ? 0 : i + 1;
        snprintf(line, sizeof(line), "[%d] %s", num, history[distances[i].index].path);
        WriteToBuffer(out_buffer, width, 3 + i, 2, line, white);
    }

    // Instructions
    WriteToBuffer(out_buffer, width, height - 2, 2, "Press 1-9/0 to jump, ESC to cancel", white);
}

void Cleanup() {
    ClearMemberSource();
    const char* temp = getenv("TEMP");
    if (temp != NULL) {
        char temp_path[MAX_PATH];
        snprintf(temp_path, MAX_PATH, "%s\\browser_lastdir.txt", temp);

        // Claude mode builds and edits workspaces; it is not navigation, so
        // quitting out of it lands the shell where drift was browsing when
        // 'c' was pressed. The exits put current_directory back themselves
        // (ExitClaudeMode and friends, on h/c/Esc), but 'q' leaves from
        // wherever the mode had reached: the workspace and session views sit
        // in .drift\workspaces, and an edit browse roams anywhere at all --
        // it roams to pick member folders, though, not to pick a destination.
        //
        // The anchor browse is the deliberate exception. Its folder is a
        // minted timestamp that nothing else leads to and no one would type,
        // so quitting there is the only way a shell reaches it -- the same
        // reason 'f' exists at all (see EnterAnchorMode). anchor_armed is
        // therefore absent from the test on purpose, which leaves that browse
        // on current_directory: whichever directory under the anchor it had
        // reached. That makes the containment in HandleInput load-bearing --
        // '`', '~' and 'o' are inert while armed, and 'h' at the anchor root
        // exits the mode rather than stepping up into .drift\workspaces, so
        // an armed browse cannot be standing outside the anchor by this point
        const char* final_dir = (claude_mode != CM_OFF || edit_armed) ? claude_return_dir
                                                                     : current_directory;

        FILE* f = fopen(temp_path, "w");
        if (f) {
            fprintf(f, "%s", final_dir);
            fclose(f);
        }
    }

    SetConsoleMode(hIn, original_console_mode);
    SetConsoleActiveScreenBuffer(hOriginal);
    CloseHandle(hAlt);
}

// Deliberately lenient: no MB_ERR_INVALID_CHARS here, so a character clipped
// by a caller's line buffer decodes to one replacement mark. Failing strictly
// would return 0 and blank the whole row instead. Strictness belongs in the
// detection in LoadPreview, not in the render path
void BytesToWide(const char* src, wchar_t* dst, int dst_count, UINT cp) {
    int written = MultiByteToWideChar(cp, 0, src, -1, dst, dst_count);
    if (written == 0 && dst_count > 0) {
        dst[0] = L'\0';
    }
}

// Filenames from FindFirstFileA really are in the ANSI codepage
void AnsiToWide(const char* src, wchar_t* dst, int dst_count) {
    BytesToWide(src, dst, dst_count, CP_ACP);
}

void WriteToBufferCP(CHAR_INFO* buffer, int width, int row, int col, const char* text,
                     WORD text_color, UINT cp) {
    wchar_t wtext[MAX_PATH];
    BytesToWide(text, wtext, MAX_PATH, cp);
    int len = (int)wcslen(wtext);

    // wtext is UTF-16 and each cell holds one code unit, so the clip below is
    // in characters, not bytes -- an accent stops costing two columns
    for (int i = 0; i < len && col + i < width - 1; i++) {
        int index = row * width + col + i;
        buffer[index].Char.UnicodeChar = wtext[i];
        buffer[index].Attributes = text_color;
    }
}

void WriteToBuffer(CHAR_INFO* buffer, int width, int row, int col, const char* text, WORD text_color) {
    WriteToBufferCP(buffer, width, row, col, text, text_color, CP_ACP);
}

// How many bytes of a UTF-8 string fit in `cells` frame-buffer cells, cutting
// only on a character boundary. Slicing at a byte offset instead would split a
// character and make each row narrower than the space it was given. A
// character outside the BMP becomes a surrogate pair, and each cell holds one
// UTF-16 code unit, so those cost two cells
int Utf8Prefix(const char* s, int cells, int max_bytes) {
    int n = 0;
    int used = 0;
    while (s[n] != '\0' && used < cells) {
        unsigned char b = (unsigned char)s[n];
        int adv = b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : (b >= 0xC0 ? 2 : 1));
        // Truncated or malformed: take only the bytes actually present, so a
        // stray lead byte still advances rather than looping forever
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

void ClearMarkedFiles() {
    mark_directory[0] = '\0';
    marked_files_count = 0;
    implicit_mark = false;
    SetMarkStatus(MARKED);
}

// Listing rows only -- the top two window rows are the header and the rule
int GetVisibleRows() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) return 1;
    int rows = info.srWindow.Bottom - info.srWindow.Top - 1;
    return rows < 1 ? 1 : rows;
}

bool IsDirectory(WIN32_FIND_DATA* file_data) {
    return (file_data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Display color: hidden files are dimmed, directories blue, executables
// green, everything else white
WORD FileColor(WIN32_FIND_DATA* file) {
    if (file->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) return gray;
    if (file->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return blue;
    char* ext = strrchr(file->cFileName, '.');
    if (ext != NULL && (_stricmp(ext, ".exe") == 0 || _stricmp(ext, ".bat") == 0 ||
                        _stricmp(ext, ".cmd") == 0 || _stricmp(ext, ".com") == 0)) {
        return green;
    }
    return white;
}

// Same contract as GetFilePath -- false on a mangled or over-long name, so
// navigation and open never act on a truncated (wrong) path
bool GetSelectedRowPath(int selected_row, char* out_path) {
    return GetFilePath(current_directory, &current_directory_files[selected_row], out_path);
}

void LoadParentDirectory() {
    GetParentDirectory(current_directory, parent_directory);
    parent_directory_file_count = GetFilesInDirectory(parent_directory, parent_directory_files);
}

// Returns false when the combined path would truncate at MAX_PATH -- a
// truncated path could name a *different* existing file, so callers that
// feed SHFileOperation must skip such entries. Also refuses names containing
// '?' or '*': those are illegal in real Windows filenames, so their presence
// means FindFirstFileA mangled a name it could not represent in the ANSI
// codepage -- and SHFileOperation expands them as wildcards, which would
// operate on files that were never marked
bool GetFilePath(char* current_directory, WIN32_FIND_DATA* file, char* out_path) {
    if (strpbrk(file->cFileName, "?*") != NULL) {
        out_path[0] = '\0';
        return false;
    }
    // A root already ends in a separator, so adding one gives "C:\\Foo".
    // Win32 accepts that everywhere, but it is what the header renders
    size_t len = strlen(current_directory);
    const char* sep = (len > 0 && current_directory[len - 1] == '\\') ? "" : "\\";
    return snprintf(out_path, MAX_PATH, "%s%s%s", current_directory, sep,
                    file->cFileName) < MAX_PATH;
}

bool IsRootDirectory(char* path) {
    if (strlen(path) == 3 && path[1] == ':' && path[2] == '\\') {
        return true; // "C:\"
    }
    // A UNC share is a root too: there is no directory above \\server\share.
    // Treating it as an ordinary path let GetParentDirectory strip back to
    // "\\server", which lists nothing, and then to "\", whose search pattern
    // quietly resolves against the local drive
    if (path[0] == '\\' && path[1] == '\\') {
        const char* share = strchr(path + 2, '\\');
        if (share == NULL) {
            return true; // "\\server" -- nothing above it either
        }
        const char* next = strchr(share + 1, '\\');
        if (next == NULL || next[1] == '\0') {
            return true; // "\\server\share", with or without a trailing slash
        }
    }
    return false;
}

bool IsMarkDirectorySet() {
    return mark_directory[0] != '\0';
}

bool MarkDirEqualToCurrentDir() {
    return strcmp(mark_directory, current_directory) == 0;
}

// Full reset -- used when *changing* directories
void LoadCurrentDirectory() {
    current_directory_file_count = GetFilesInDirectory(current_directory, current_directory_files);
    selected_row = 0;
    top_row = 0;
    preview_path[0] = '\0'; // contents may have changed under the same path
}

// Refresh in place -- used after paste/delete/create/vim so the cursor
// doesn't jump back to the top. DrawScreen normalizes top_row.
void ReloadCurrentDirectory() {
    current_directory_file_count = GetFilesInDirectory(current_directory, current_directory_files);
    if (selected_row >= current_directory_file_count) {
        selected_row = current_directory_file_count - 1;
    }
    if (selected_row < 0) {
        selected_row = 0;
    }
    preview_path[0] = '\0'; // contents may have changed under the same path
}

void SetMarkStatus(enum MarkStatus status) {
    mark_status = status;
}

bool MarkedFilesFull() {
    return marked_files_count >= MAX_FILES;
}
