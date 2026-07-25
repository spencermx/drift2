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
//              root (or Esc/c) returns to the workspace list
// - R        : (in the workspace list) rename a workspace's drift display
//              name (stored in .drift\workspace-names); the folder itself
//              is never renamed, so its sessions stay associated
// - Shift+W  : (while browsing) add the directory under the cursor to a
//              workspace picked from a popup
// - Enter/L  : (session list) resume the session in claude, anchored in the
//              workspace; N starts a new session (works from the workspace
//              list too). claude is spawned via "cmd /c claude" so both a
//              native exe and the npm .cmd shim resolve.
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
void SetNameEntry(const char* leaf, const char* key, const char* name);
void LoadWorkspaceNames();
bool FindArraySpan(const char* buf, int* out_start, int* out_end);
void LoadMembersFrom(const char* anchor);
size_t AppendFmt(char* buf, size_t n, size_t cap, const char* fmt, ...);
void SaveMembersTo(const char* anchor);
int FindMember(const char* path);
void RemoveMemberAt(int index);
void ToggleMemberUnderCursor();
void EnterEditMode();
void ExitEditMode();
void EnsureWorkspaceNotes(const char* anchor);
void EnterAnchorMode();
void ExitAnchorMode();
void HandleQuickAdd();
int LaunchClaudeIn(const char* anchor, const char* session_id);
void ScrollOriginalScreen();
bool IsSafeSessionId(const char* id);
void ApplySessionNames(const char* anchor);
void SetSessionName(const char* anchor, const char* id, const char* name);
void WorkspaceDisplayName(const char* folder, char* out, size_t out_size);
void SetWorkspaceName(const char* folder, const char* display);
bool WorkspaceNameTaken(const char* name, const char* except_folder);
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
void AnsiToWide(const char* src, wchar_t* dst, int dst_count);
char* BuildFromList(size_t* out_size);
void OpenFileInEditor();

void LoadParentDirectory();
void LoadCurrentDirectory();
void ReloadCurrentDirectory();
void ClearMarkedFiles();
void DrawCreatePopup(int width, char* input_text, const char* placeholder, CHAR_INFO* out_buffer);
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
int member_count = 0;
bool manifest_focused = false;
int manifest_selected = 0;
int manifest_top = 0;
char json_buf[65536];
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
char workspace_names[32768];
bool workspace_names_loaded = false;

enum MarkStatus mark_status = MARKED;
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
    // Keep the process current directory out of the executable search path.
    // Without this, SearchPath prefers the directory drift was launched from,
    // so a vim.exe planted there would be picked over the real one
    SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);

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
    if (preview_len >= 3 && (unsigned char)preview_bytes[0] == 0xEF &&
        (unsigned char)preview_bytes[1] == 0xBB && (unsigned char)preview_bytes[2] == 0xBF) {
        memmove(preview_bytes, preview_bytes + 3, preview_len - 3);
        preview_len -= 3;
    }
}

void DrawContextPane(CHAR_INFO* buffer, int width, int height, int divider2) {
    char sel_path[MAX_PATH];
    if (!GetSelectedRowPath(selected_row, sel_path)) {
        sel_path[0] = '\0';
    }
    if (strcmp(sel_path, preview_path) != 0) {
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
            WriteToBuffer(buffer, width, row, col_start, line, white);
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
// is never bounded by a buffer and a crash can't leave it half written.
// `key` is one field for workspace names, "folder<TAB>session" for session
// names; a row matches when the line starts with the key and a tab.
void SetNameEntry(const char* leaf, const char* key, const char* name) {
    char file[MAX_PATH];
    if (!GetNameFile(file, leaf)) return;
    char tmp[MAX_PATH];
    if (snprintf(tmp, MAX_PATH, "%s.tmp", file) >= MAX_PATH) return;

    FILE* in = fopen(file, "rb");
    FILE* out = fopen(tmp, "wb");
    if (out == NULL) {
        if (in != NULL) fclose(in);
        return;
    }
    size_t klen = strlen(key);
    if (in != NULL) {
        char line[MAX_PATH + SESSION_NAME_LEN + 64];
        while (fgets(line, sizeof(line), in) != NULL) {
            if (_strnicmp(line, key, klen) == 0 && line[klen] == '\t') continue;
            fputs(line, out);
        }
        fclose(in);
    }
    if (name[0] != '\0') {
        fprintf(out, "%s\t%s\n", key, name);
    }
    // On a write error the rename simply doesn't happen and the original file
    // survives intact -- the rename is what makes an edit visible at all
    fclose(out);
    MoveFileEx(tmp, file, MOVEFILE_REPLACE_EXISTING);
    workspace_names_loaded = false;
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

// Suspend the TUI, run claude anchored in the workspace, resume when it
// exits. Spawned through cmd so PATH resolution finds a native claude.exe
// and the npm claude.cmd shim alike. session_id NULL starts a new session.
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

    // Quote the id: cmd only honors '&' and friends *outside* double quotes,
    // and '"' itself cannot occur in a filename, so the id cannot break back out
    char command[MAX_PATH + 64];
    int written = (session_id != NULL)
        ? snprintf(command, sizeof(command), "cmd /c claude --resume \"%s\"", session_id)
        : snprintf(command, sizeof(command), "cmd /c claude");
    if (written < 0 || written >= (int)sizeof(command)) {
        return 1;
    }

    SetConsoleActiveScreenBuffer(hOriginal);
    SetConsoleMode(hIn, original_console_mode);
    ScrollOriginalScreen();

    STARTUPINFO si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (CreateProcess(NULL, command, NULL, NULL, FALSE, 0, NULL, anchor, &si, &pi)) {
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
        snprintf(s->id, sizeof(s->id), "%s", fd.cFileName);
        char* dot = strrchr(s->id, '.');
        if (dot != NULL) *dot = '\0';
        // Not a uuid: either a truncated over-long name or a file someone
        // crafted to smuggle shell metacharacters into the launch. Either way
        // it cannot be resumed, so don't list it (delete it from the browser)
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
void SetSessionName(const char* anchor, const char* id, const char* name) {
    char key[MAX_PATH];
    if (snprintf(key, sizeof(key), "%s\t%s", AnchorFolder(anchor), id) >= (int)sizeof(key)) return;
    SetNameEntry(SESSION_NAMES_FILE, key, name);
}

void LoadWorkspaceNames() {
    workspace_names[0] = '\0';
    workspace_names_loaded = true;

    char file[MAX_PATH];
    if (!GetNameFile(file, WORKSPACE_NAMES_FILE)) return;
    FILE* f = fopen(file, "rb");
    if (f == NULL) return;
    size_t len = fread(workspace_names, 1, sizeof(workspace_names) - 1, f);
    fclose(f);
    workspace_names[len] = '\0';
}

// A workspace's shown name: its row in .drift\workspace-names if it has one,
// otherwise the folder name itself. The folder is never renamed -- that path
// is the key Claude files its sessions under, so renaming it would orphan
// them -- so this name is a pure drift-side overlay, exactly like the session
// names above.
void WorkspaceDisplayName(const char* folder, char* out, size_t out_size) {
    snprintf(out, out_size, "%s", folder); // default: the folder's own name
    if (!workspace_names_loaded) LoadWorkspaceNames();

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
void SetWorkspaceName(const char* folder, const char* display) {
    // An empty name drops the row, reverting to the folder name
    SetNameEntry(WORKSPACE_NAMES_FILE, folder, display);
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

// Report a rejected name and wait for a keypress -- ShowStatusBanner alone is
// wiped by the next redraw, so it needs an explicit acknowledgement to be seen
void NotifyNameTaken(const char* name) {
    char msg[160];
    snprintf(msg, sizeof(msg), "\"%s\" is already in use -- press a key", name);
    ShowStatusBanner(msg);
    INPUT_RECORD ir;
    DWORD ev;
    while (ReadConsoleInput(hIn, &ir, 1, &ev) &&
           (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown)) {
    }
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

    int start_col = info.srWindow.Left + (screen_width - popup_w) / 2;
    int start_row = info.srWindow.Top + (screen_height - popup_h) / 2;

    CHAR_INFO* popup_buffer = (CHAR_INFO*)malloc(popup_w * popup_h * sizeof(CHAR_INFO));
    if (popup_buffer == NULL) return;

    char name[MAX_PATH];
    int max_len = popup_w - 10;
    snprintf(name, sizeof(name), "%.*s", max_len, sel->name);
    int pos = (int)strlen(name);
    bool cancelled = false;

    CONSOLE_CURSOR_INFO cursor_info = { 25, TRUE };
    SetConsoleCursorInfo(hAlt, &cursor_info);

    while (1) {
        DrawCreatePopup(popup_w, name, NULL, popup_buffer);
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

    SetSessionName(claude_workspace, sel->id, name);
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

    int start_col = info.srWindow.Left + (screen_width - popup_w) / 2;
    int start_row = info.srWindow.Top + (screen_height - popup_h) / 2;

    CHAR_INFO* popup_buffer = (CHAR_INFO*)malloc(popup_w * popup_h * sizeof(CHAR_INFO));
    if (popup_buffer == NULL) return;

    char name[MAX_PATH];
    int max_len = popup_w - 10;
    char current[MAX_PATH];
    WorkspaceDisplayName(folder, current, sizeof(current));
    snprintf(name, sizeof(name), "%.*s", max_len, current);
    int pos = (int)strlen(name);
    bool cancelled = false;

    CONSOLE_CURSOR_INFO cursor_info = { 25, TRUE };
    SetConsoleCursorInfo(hAlt, &cursor_info);

    while (1) {
        DrawCreatePopup(popup_w, name, NULL, popup_buffer);
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
    SetWorkspaceName(folder, name);
}

// 'd' in the session list: delete the transcript (recycle bin) after a
// confirmation popup
void HandleDeleteSession() {
    if (session_count == 0) return;
    SessionEntry* sel = &sessions[session_selected];

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

    int popup_w = 44;
    int popup_h = 7;
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
    WriteToBuffer(popup, popup_w, 1, 2, "Delete session?", red);
    WriteToBuffer(popup, popup_w, 3, 2, sel->name, white);
    WriteToBuffer(popup, popup_w, 5, 2, "[Y] Yes - Recycle Bin  [N] No", white);

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

        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
        if (vk == 'Y') {
            char from[MAX_PATH + 2];
            int len = snprintf(from, MAX_PATH, "%s", sel->path);
            if (len > 0 && len < MAX_PATH) {
                from[len + 1] = '\0'; // double-null for SHFileOperation

                SHFILEOPSTRUCT op = {0};
                op.wFunc = FO_DELETE;
                op.pFrom = from;
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_WANTNUKEWARNING;

                if (SHFileOperation(&op) == 0 && !op.fAnyOperationsAborted) {
                    SetSessionName(claude_workspace, sel->id, ""); // drop the stale name
                }
                FlushConsoleInputBuffer(hIn);

                sessions_loaded_for[0] = '\0';
                LoadSessionsFor(claude_workspace);
                if (session_selected >= session_count) {
                    session_selected = session_count > 0 ? session_count - 1 : 0;
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
    for (int i = 0; i < list_height && i < current_directory_file_count; i++) {
        WIN32_FIND_DATA* w = &current_directory_files[i];
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
        AnsiToWide(row_text, wname, MAX_PATH);
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
    int total = (int)strlen(sel->name);
    int off = 0;
    while (off < total && row < height) {
        char chunk[200];
        int c = total - off;
        if (c > pane_w) c = pane_w;
        if (c > (int)sizeof(chunk) - 1) c = (int)sizeof(chunk) - 1;
        memcpy(chunk, sel->name + off, c);
        chunk[c] = '\0';
        WriteToBuffer(buffer, width, row, col_start, chunk, white);
        off += c;
        row++;
    }
}
// ============================= Workspace Designer ================================
// Membership lives in <anchor>\.claude\settings.json under
// permissions.additionalDirectories -- the file Claude Code natively reads.
// Editing splices only that array so any other settings in the file survive.

// Locates the additionalDirectories [...] span. start/end index '[' and ']'.
bool FindArraySpan(const char* buf, int* out_start, int* out_end) {
    const char* KEY = "\"additionalDirectories\"";
    const size_t key_len = strlen(KEY);

    // Accept the key only where a key can actually appear -- outside any
    // string, and followed by ':' then '['. A plain strstr also matches the
    // name used as a *value*, and a plain strchr for '[' walks past a non-array
    // value into whatever array comes next (typically permissions.allow), which
    // SaveMembersTo would then overwrite with folder paths
    const char* p = NULL;
    bool in_string = false;
    for (const char* c = buf; *c != '\0'; c++) {
        if (in_string) {
            if (*c == '\\' && c[1] != '\0') c++;
            else if (*c == '"') in_string = false;
            continue;
        }
        if (*c != '"') continue;
        if (strncmp(c, KEY, key_len) != 0) {
            in_string = true; // some other string; skip to its closing quote
            continue;
        }
        const char* v = c + key_len;
        while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
        if (*v != ':') {
            in_string = true; // the name appearing as a value, not as a key
            continue;
        }
        v++;
        while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r') v++;
        // A key whose value is not an array is left for the caller's insert
        // branch rather than spliced over something else
        if (*v != '[') return false;
        p = v;
        break;
    }
    if (p == NULL) return false;

    const char* q = p + 1;
    in_string = false;
    while (*q != '\0') {
        if (in_string) {
            if (*q == '\\' && q[1] != '\0') q++;
            else if (*q == '"') in_string = false;
        } else {
            if (*q == '"') in_string = true;
            else if (*q == ']') {
                *out_start = (int)(p - buf);
                *out_end = (int)(q - buf);
                return true;
            }
        }
        q++;
    }
    return false;
}

void LoadMembersFrom(const char* anchor) {
    member_count = 0;
    json_block_reason = NULL;

    // Under the Wine wrapper, entries are stored host-style ("/Users/...")
    // so the host claude can read them; internally we use the drive form
    char host_drive[8];
    DWORD hd = GetEnvironmentVariable("DRIFT_HOST_DRIVE", host_drive, sizeof(host_drive));
    bool host = hd > 0 && hd < sizeof(host_drive);

    char file[MAX_PATH];
    if (snprintf(file, MAX_PATH, "%s\\.claude\\settings.json", anchor) >= MAX_PATH) return;
    FILE* f = fopen(file, "rb");
    if (f == NULL) return;
    int len = (int)fread(json_buf, 1, sizeof(json_buf) - 1, f);
    int extra = fgetc(f); // anything left means the file exceeds our buffer
    fclose(f);
    json_buf[len] = '\0';
    if (extra != EOF) {
        // A partial parse could corrupt the file on save
        json_block_reason = "(settings.json too large to edit)";
        return;
    }

    int s, e;
    if (!FindArraySpan(json_buf, &s, &e)) return;
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
        if (n > 0) {
            if (host && out[0] == '/') {
                char tmp[MAX_PATH];
                if (snprintf(tmp, sizeof(tmp), "%s%s", host_drive, out) < (int)sizeof(tmp)) {
                    for (char* q = tmp; *q != '\0'; q++) {
                        if (*q == '/') *q = '\\';
                    }
                    strcpy(out, tmp);
                }
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

void SaveMembersTo(const char* anchor) {
    if (json_block_reason != NULL) return;

    char dir[MAX_PATH];
    if (snprintf(dir, MAX_PATH, "%s\\.claude", anchor) >= MAX_PATH) return;
    CreateDirectory(dir, NULL);
    char file[MAX_PATH];
    if (snprintf(file, MAX_PATH, "%s\\.claude\\settings.json", anchor) >= MAX_PATH) return;

    // Build the replacement array text, JSON-escaping backslashes
    size_t cap = (size_t)MAX_MEMBERS * (MAX_PATH * 2 + 16) + 64;
    char* arr = (char*)malloc(cap);
    if (arr == NULL) return;
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
    int len = 0;
    if (f != NULL) {
        len = (int)fread(json_buf, 1, sizeof(json_buf) - 1, f);
        bool read_failed = ferror(f) != 0;
        fclose(f);
        if (read_failed) {
            free(arr);
            return;
        }
    } else {
        DWORD attr = GetFileAttributes(file);
        DWORD err = GetLastError();
        bool absent = attr == INVALID_FILE_ATTRIBUTES &&
                      (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
        if (!absent) {
            free(arr);
            return; // leave whatever is on disk alone
        }
    }
    json_buf[len] = '\0';

    char* out = (char*)malloc((size_t)len + cap + 256);
    if (out == NULL) {
        free(arr);
        return;
    }

    int s, e;
    if (len > 0 && FindArraySpan(json_buf, &s, &e)) {
        // Replace just the array; everything else in the file survives
        memcpy(out, json_buf, s);
        out[s] = '\0';
        strcat(out, arr);
        strcat(out, json_buf + e + 1);
    } else if (len > 0) {
        // No additionalDirectories yet: insert into the permissions object
        // if there is one, otherwise into the root object
        char* perm = strstr(json_buf, "\"permissions\"");
        char* brace = perm != NULL ? strchr(perm, '{') : strchr(json_buf, '{');
        if (brace == NULL) { // not JSON we understand -- leave it alone
            free(arr);
            free(out);
            return;
        }
        int at = (int)(brace - json_buf) + 1;
        const char* look = json_buf + at;
        while (*look == ' ' || *look == '\n' || *look == '\r' || *look == '\t') look++;
        bool needs_comma = *look != '}';
        memcpy(out, json_buf, at);
        out[at] = '\0';
        char insert[512];
        if (perm != NULL) {
            snprintf(insert, sizeof(insert), "\n    \"additionalDirectories\": ");
        } else {
            snprintf(insert, sizeof(insert), "\n  \"permissions\": {\n    \"additionalDirectories\": ");
        }
        strcat(out, insert);
        strcat(out, arr);
        if (perm == NULL) strcat(out, "\n  }");
        if (needs_comma) strcat(out, ",");
        strcat(out, json_buf + at);
    } else {
        snprintf(out, cap + 256, "{\n  \"permissions\": {\n    \"additionalDirectories\": ");
        strcat(out, arr);
        strcat(out, "\n  }\n}\n");
    }

    // Write via temp + rename so a crash can't leave a half-written file
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
            }
        }
    }
    free(arr);
    free(out);
}

int FindMember(const char* path) {
    for (int i = 0; i < member_count; i++) {
        if (_stricmp(members[i], path) == 0) return i;
    }
    return -1;
}

void RemoveMemberAt(int index) {
    for (int j = index; j < member_count - 1; j++) {
        strcpy(members[j], members[j + 1]);
    }
    member_count--;
    if (manifest_selected >= member_count && manifest_selected > 0) {
        manifest_selected = member_count - 1;
    }
    SaveMembersTo(edit_workspace);
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
    if (member_count >= MAX_MEMBERS) return;
    strcpy(members[member_count], path);
    member_count++;
    SaveMembersTo(edit_workspace);
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
                LoadMembersFrom(anchor);
                if (json_block_reason == NULL && FindMember(target) < 0 && member_count < MAX_MEMBERS) {
                    strcpy(members[member_count], target);
                    member_count++;
                    SaveMembersTo(anchor);
                }
                char msg[96];
                snprintf(msg, sizeof(msg), "Added to %s", labels[ch - '1']);
                ShowStatusBanner(msg);
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

    if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) {
        return 1; // Ignore non-key events (window resize lands here and triggers a redraw)
    }

    BOOL ctrl = input.Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
    BOOL shift= input.Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED;

    // Session list has its own small keymap; everything else is inert there
    if (claude_mode == CM_SESSIONS) {
        pending_g = false;
        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
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
            pending_g = false;
            if (vk == 'Q') return 0;
            if (vk == 'J' && manifest_selected < member_count - 1) manifest_selected++;
            else if (vk == 'K' && manifest_selected > 0) manifest_selected--;
            else if ((vk == 'X' || vk == VK_SPACE) && member_count > 0) RemoveMemberAt(manifest_selected);
            else if (vk == VK_RETURN && member_count > 0) {
                // Jump the browser to this member for inspection
                char jump[MAX_PATH];
                strcpy(jump, members[manifest_selected]);
                ChangeCurrentDirectory(jump);
                manifest_focused = false;
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
        pending_g = false;
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
        pending_g = false;

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
        bool was_g = pending_g;
        pending_g = false;

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

                if (!IsMarkDirectorySet() || !MarkDirEqualToCurrentDir()) {
                    ClearMarkedFiles();
                    strcpy(mark_directory, current_directory);
                    ToggleMark();
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
                ModifySelectedRow(1);
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

    // Resolve vim to an absolute path first. CreateProcess with a NULL
    // lpApplicationName takes the program from the command line and searches
    // drift's own directory and the process current directory *before* PATH,
    // so a planted vim.exe would win; naming the executable outright makes it
    // search nothing at all. (Initialize's SetSearchPathMode is what stops
    // SearchPath from preferring the current directory in turn.)
    char vim_exe[MAX_PATH];
    DWORD vim_len = SearchPath(NULL, "vim.exe", NULL, MAX_PATH, vim_exe, NULL);
    bool have_vim = vim_len > 0 && vim_len < MAX_PATH;

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

    if (have_vim && CreateProcess(vim_exe, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
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
}

void HandleMarkOperation(enum MarkStatus new_status) {
    if (current_directory_file_count == 0) return;

    // If marks already exist in this directory, operate on them as-is.
    // Otherwise mark the cursor line. (No special case for a single mark --
    // that previously cleared the marked file and yanked the cursor line
    // instead.)
    if (!IsMarkDirectorySet() || !MarkDirEqualToCurrentDir() || marked_files_count == 0) {
        ClearMarkedFiles();
        strcpy(mark_directory, current_directory);
        ToggleMark();
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

    // Failure keeps the marks, same as cancelling the delete popup -- the
    // user built that set on purpose. (Windows shows its own error dialog;
    // FOF_NOERRORUI is unset.)
    if (result == 0 && !op.fAnyOperationsAborted) {
        ClearMarkedFiles();
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

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

    if (screen_width < POPUP_WIDTH || screen_height < POPUP_HEIGHT) return;

    // Create popup buffer
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

    // Wait for input
    while (1) {
        INPUT_RECORD input;
        DWORD events;
        if (!ReadConsoleInput(hIn, &input, 1, &events)) {
            break; // Treat console failure as cancel
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

void DrawCreatePopup(int width, char* input_text, const char* placeholder, CHAR_INFO* out_buffer) {
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
        WriteToBuffer(out_buffer, width, 1, 8, input_text, white);
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

    int start_col = info.srWindow.Left + (screen_width - popup_w) / 2;
    int start_row = info.srWindow.Top + (screen_height - popup_h) / 2;

    CHAR_INFO* popup_buffer = (CHAR_INFO*)malloc(popup_w * popup_h * sizeof(CHAR_INFO));
    if (popup_buffer == NULL) return;

    // Show the hardware cursor while typing in the name field
    CONSOLE_CURSOR_INFO cursor_info = { 25, TRUE };
    SetConsoleCursorInfo(hAlt, &cursor_info);

    while (1) {
        // Draw popup
        DrawCreatePopup(popup_w, name, ph, popup_buffer);

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
        } else if (c >= 32 && c < 127 && pos < popup_w - 10) {
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
        if (custom) SetWorkspaceName(id, name); // else it shows the id itself
        ReloadCurrentDirectory();
        for (int i = 0; i < current_directory_file_count; i++) {
            if (_stricmp(current_directory_files[i].cFileName, id) == 0) {
                selected_row = i;
                break;
            }
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

    char full_path[MAX_PATH];
    if (snprintf(full_path, MAX_PATH, "%s\\%s", current_directory, name) >= MAX_PATH) {
        return; // would truncate -- could create or collide at the wrong path
    }

    if (is_directory) {
        CreateDirectory(full_path, NULL);
    } else {
        HANDLE h = CreateFile(full_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
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

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

    int popup_height = display_count + 5;
    int popup_width = 60;
    if (popup_width > screen_width) popup_width = screen_width;
    if (popup_width < 10 || screen_height < popup_height) return;

    // Create popup buffer
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

    // Wait for input
    while (1) {
        INPUT_RECORD input;
        DWORD events;
        if (!ReadConsoleInput(hIn, &input, 1, &events)) {
            break; // Treat console failure as cancel
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
    const char* temp = getenv("TEMP");
    if (temp != NULL) {
        char temp_path[MAX_PATH];
        snprintf(temp_path, MAX_PATH, "%s\\browser_lastdir.txt", temp);

        // In a claude mode the process cwd is the workspaces root, not where
        // the user was browsing. Quitting straight from the workspace/session
        // view never unwinds it (ExitClaudeMode does, on H/C/Esc), so persist
        // the saved return directory instead -- otherwise the cd-on-quit
        // wrapper strands the shell in .drift\workspaces. Same for an anchor
        // browse, which is deeper still and nowhere the user wants to land
        const char* final_dir = (claude_mode != CM_OFF || anchor_armed) ? claude_return_dir
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

void AnsiToWide(const char* src, wchar_t* dst, int dst_count) {
    int written = MultiByteToWideChar(CP_ACP, 0, src, -1, dst, dst_count);
    if (written == 0 && dst_count > 0) {
        dst[0] = L'\0';
    }
}

void WriteToBuffer(CHAR_INFO* buffer, int width, int row, int col, const char* text, WORD text_color) {
    wchar_t wtext[MAX_PATH];
    AnsiToWide(text, wtext, MAX_PATH);
    int len = (int)wcslen(wtext);

    for (int i = 0; i < len && col + i < width - 1; i++) {
        int index = row * width + col + i;
        buffer[index].Char.UnicodeChar = wtext[i];
        buffer[index].Attributes = text_color;
    }
}

void ClearMarkedFiles() {
    mark_directory[0] = '\0';
    marked_files_count = 0;
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
    return snprintf(out_path, MAX_PATH, "%s\\%s", current_directory, file->cFileName) < MAX_PATH;
}

bool IsRootDirectory(char* path) {
    return strlen(path) == 3 && path[1] == ':' && path[2] == '\\';
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
