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
#define SESSION_TITLE_LEN 96
typedef struct {
    char path[MAX_PATH];          // full path to the .jsonl transcript
    char id[48];                  // session uuid (filename without .jsonl)
    char title[SESSION_TITLE_LEN]; // first user message excerpt
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
void LoadSessions();
void ParseSessionTitle(const char* path, char* out, int out_size);
int CompareSessions(const void* a, const void* b);
void FormatAge(FILETIME ft, char* out, int out_size);
void EncodeProjectPath(const char* path, char* out, bool keep_dots);
void DrawSessionsPanes(CHAR_INFO* buffer, int width, int height, int divider2, int list_height);
void DrawClaudeInfoPane(CHAR_INFO* buffer, int width, int height, int divider2);
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
void DrawCreatePopup(int width, char* input_text, CHAR_INFO* out_buffer);
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

enum MarkStatus mark_status = MARKED;
// =========================== Global Variables ==============================
int main() {
    Initialize();

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
    GetConsoleScreenBufferInfo(hAlt, &info);
    int width = info.srWindow.Right - info.srWindow.Left + 1;
    int height = info.srWindow.Bottom - info.srWindow.Top + 1;

    // Window too small to draw the layout safely (header line, rule line,
    // and at least one listing row)
    if (width < MIN_WINDOW_WIDTH || height < 3) {
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

        // Path on the left, truncated from the front so the deepest (most
        // useful) part stays visible. The claude browser announces itself
        // with a title instead of a filesystem path, in yellow.
        char claude_title[MAX_PATH + 32];
        char* header_path;
        if (claude_mode == CM_WORKSPACES) {
            snprintf(claude_title, sizeof(claude_title), "claude workspaces");
            header_path = claude_title;
        } else if (claude_mode == CM_SESSIONS) {
            snprintf(claude_title, sizeof(claude_title), "claude workspaces > %s", claude_workspace_name);
            header_path = claude_title;
        } else {
            header_path = current_directory;
        }
        WORD header_color = claude_mode == CM_OFF ? blue : yellow;
        int path_avail = right_col - 2;
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
        WriteToBuffer(buffer, width, 0, 0, path_display, header_color);
    }

    // Horizontal rule under the header, with a junction where it crosses
    // the column divider
    // Frame follows the mode: blue for files, claude-orange in claude mode
    WORD frame_color = claude_mode == CM_OFF ? blue : orange;
    for (int col = 0; col < width; col++) {
        int index = width + col;
        bool junction = col == COLUMN_DIVIDER_POSITION || (three_pane && col == divider2);
        buffer[index].Char.UnicodeChar = junction ? BOX_T_DOWN : BOX_HORIZONTAL;
        buffer[index].Attributes = frame_color;
    }
    // ============================= Draw Header Line ==================================

    // ============================= Draw Parent Directory =============================
    for (int i = 0; claude_mode != CM_SESSIONS && i < list_height && i < parent_directory_file_count; i++) {
        int file_index = i;

        color = FileColor(&parent_directory_files[file_index]);
        if (claude_mode != CM_OFF && color == blue) {
            color = yellow; // claude mode: directories join the claude palette
        }

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

        color = FileColor(&current_directory_files[file_index]);
        if (claude_mode != CM_OFF && color == blue) {
            color = yellow; // claude mode: directories join the claude palette
        }

        if (is_selected) {
            // Remap the foreground to its dark variant (white becomes black)
            // and paint the whole pane-width segment as the selection bar
            WORD fg = color & (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            if (fg == (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)) {
                fg = 0;
            }
            color = fg | bar_background;
            for (int col = COLUMN_DIVIDER_POSITION + 1; col < divider2; col++) {
                buffer[(i + 2) * width + col].Attributes = color;
            }
        }

        AnsiToWide(current_directory_files[file_index].cFileName, wname, MAX_PATH);
        int len = (int)wcslen(wname);

        for (int col = 0; col < len && col + COLUMN_DIVIDER_POSITION + 4 < divider2; col++) {
            int index = (i + 2) * width + COLUMN_DIVIDER_POSITION + 4 + col;
            buffer[index].Char.UnicodeChar = wname[col];
            buffer[index].Attributes = color;
        }

        // ================== Highlight marked files in the current directory ==================
        // Matched by path, not row index, so highlights survive directory
        // reloads and re-sorting
        if (marked_files_count > 0 && MarkDirEqualToCurrentDir()) {
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
    }
    // ============================= Draw Current Directory ============================

    // Empty directories get an explicit placeholder instead of a blank pane
    if (claude_mode != CM_SESSIONS && current_directory_file_count == 0) {
        WriteToBuffer(buffer, width, 2, COLUMN_DIVIDER_POSITION + 4, "(empty)", gray);
    }

    if (three_pane && claude_mode == CM_OFF && current_directory_file_count > 0) {
        DrawContextPane(buffer, width, height, divider2);
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
void EnterClaudeMode() {
    char home[MAX_PATH];
    DWORD len = GetEnvironmentVariable("DRIFT_HOME", home, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        len = GetEnvironmentVariable("USERPROFILE", home, MAX_PATH);
    }
    if (len == 0 || len >= MAX_PATH) return;

    char path[MAX_PATH];
    if (snprintf(path, MAX_PATH, "%s\\.drift", home) >= MAX_PATH) return;
    CreateDirectory(path, NULL); // ok if it already exists
    if (snprintf(path, MAX_PATH, "%s\\.drift\\workspaces", home) >= MAX_PATH) return;
    CreateDirectory(path, NULL);

    strcpy(claude_return_dir, current_directory);
    strcpy(workspaces_root, path);
    ChangeCurrentDirectory(workspaces_root);
    claude_mode = CM_WORKSPACES;
}

void ExitClaudeMode() {
    claude_mode = CM_OFF;
    ChangeCurrentDirectory(claude_return_dir);
}

void EnterSessionsView() {
    char* name = current_directory_files[selected_row].cFileName;
    if (snprintf(claude_workspace, MAX_PATH, "%s\\%s", workspaces_root, name) >= MAX_PATH) {
        return;
    }
    snprintf(claude_workspace_name, MAX_PATH, "%s", name);
    LoadSessions();
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

void LoadSessions() {
    session_count = 0;

    char claude_root[MAX_PATH];
    DWORD len = GetEnvironmentVariable("DRIFT_CLAUDE_DIR", claude_root, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        char home[MAX_PATH];
        len = GetEnvironmentVariable("USERPROFILE", home, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return;
        if (snprintf(claude_root, MAX_PATH, "%s\\.claude", home) >= MAX_PATH) return;
    }

    char enc[MAX_PATH];
    char dir[MAX_PATH];
    EncodeProjectPath(claude_workspace, enc, false);
    if (snprintf(dir, MAX_PATH, "%s\\projects\\%s", claude_root, enc) >= MAX_PATH) return;
    if (GetFileAttributes(dir) == INVALID_FILE_ATTRIBUTES) {
        EncodeProjectPath(claude_workspace, enc, true);
        if (snprintf(dir, MAX_PATH, "%s\\projects\\%s", claude_root, enc) >= MAX_PATH) return;
        if (GetFileAttributes(dir) == INVALID_FILE_ATTRIBUTES) return; // no sessions yet
    }

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
        s->mtime = fd.ftLastWriteTime;
        ParseSessionTitle(s->path, s->title, sizeof(s->title));
        session_count++;
    } while (FindNextFile(hFind, &fd) && session_count < MAX_SESSIONS);
    FindClose(hFind);

    qsort(sessions, session_count, sizeof(SessionEntry), CompareSessions);
}

int CompareSessions(const void* a, const void* b) {
    const SessionEntry* sa = (const SessionEntry*)a;
    const SessionEntry* sb = (const SessionEntry*)b;
    return CompareFileTime(&sb->mtime, &sa->mtime); // newest first
}

// Shallow, read-only scan of a transcript for the first real user message.
// The format is undocumented; on any surprise the title just stays generic.
void ParseSessionTitle(const char* path, char* out, int out_size) {
    snprintf(out, out_size, "(untitled)");

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
                snprintf(out, out_size, "(untitled)");
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

    // Left pane: workspace list, the open one in white
    for (int i = 0; i < list_height && i < current_directory_file_count; i++) {
        WIN32_FIND_DATA* w = &current_directory_files[i];
        WORD c = strcmp(w->cFileName, claude_workspace_name) == 0 ? white : yellow;
        if (!IsDirectory(w)) c = gray;
        AnsiToWide(w->cFileName, wname, MAX_PATH);
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
        snprintf(row_text, sizeof(row_text), "%-4s %s", age, sessions[idx].title);
        AnsiToWide(row_text, wname, MAX_PATH);
        int len = (int)wcslen(wname);
        for (int col = 0; col < len && COLUMN_DIVIDER_POSITION + 2 + col < divider2; col++) {
            int index = (i + 2) * width + COLUMN_DIVIDER_POSITION + 2 + col;
            buffer[index].Char.UnicodeChar = wname[col];
            buffer[index].Attributes = attr;
        }
    }

    // Third pane: selected session details
    if (divider2 >= width || session_count == 0) return;
    int col_start = divider2 + 2;
    SessionEntry* sel = &sessions[session_selected];

    WriteToBuffer(buffer, width, 2, col_start, claude_workspace_name, white);
    char meta[64];
    snprintf(meta, sizeof(meta), "%d session(s)", session_count);
    WriteToBuffer(buffer, width, 3, col_start, meta, gray);

    char age[16];
    FormatAge(sel->mtime, age, sizeof(age));
    snprintf(meta, sizeof(meta), "last active: %s", age);
    WriteToBuffer(buffer, width, 5, col_start, meta, white);
    snprintf(meta, sizeof(meta), "id: %s", sel->id);
    WriteToBuffer(buffer, width, 6, col_start, meta, gray);

    // First prompt, wrapped to the pane
    int pane_w = width - 1 - col_start;
    if (pane_w < 8) return;
    int total = (int)strlen(sel->title);
    int off = 0;
    int row = 8;
    while (off < total && row < height) {
        char chunk[200];
        int c = total - off;
        if (c > pane_w) c = pane_w;
        if (c > (int)sizeof(chunk) - 1) c = (int)sizeof(chunk) - 1;
        memcpy(chunk, sel->title + off, c);
        chunk[c] = '\0';
        WriteToBuffer(buffer, width, row, col_start, chunk, white);
        off += c;
        row++;
    }
}
// In the workspace list, the third pane explains what this mode is and how
// to drive it instead of uselessly previewing the anchor directory
void DrawClaudeInfoPane(CHAR_INFO* buffer, int width, int height, int divider2) {
    int col = divider2 + 2;
    if (col >= width - 8) return;

    WriteToBuffer(buffer, width, 2, col, "Claude Workspaces", yellow);
    WriteToBuffer(buffer, width, 4, col, "A workspace is a set of folders", gray);
    WriteToBuffer(buffer, width, 5, col, "Claude Code opens together.", gray);

    char count_msg[48];
    snprintf(count_msg, sizeof(count_msg), "%d workspace(s)", current_directory_file_count);
    WriteToBuffer(buffer, width, 7, col, count_msg, white);

    int row = 9;
    if (row < height) WriteToBuffer(buffer, width, row++, col, "l      open sessions", white);
    if (row < height) WriteToBuffer(buffer, width, row++, col, "a      new workspace", white);
    if (row < height) WriteToBuffer(buffer, width, row++, col, "y/p    duplicate", white);
    if (row < height) WriteToBuffer(buffer, width, row++, col, "d      delete", white);
    if (row < height) WriteToBuffer(buffer, width, row++, col, "h/c    back to files", white);
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
        }
        return 1;
    }

    // Workspace list is a real directory browsed normally, with a few verbs
    // rerouted: l/Enter opens the session view instead of entering the dir,
    // h/c/Esc leave the mode, and jumps that would teleport away are inert
    if (claude_mode == CM_WORKSPACES && !ctrl && !shift) {
        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
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
        if (vk == 'O' || vk == VK_OEM_3) {
            return 1;
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

    char command[MAX_PATH + 16];
    snprintf(command, sizeof(command), "vim \"%s\"", file_path);

    // Temporarily switch to the original buffer and restore the console mode
    // for the child process
    SetConsoleActiveScreenBuffer(hOriginal);
    SetConsoleMode(hIn, original_console_mode);

    // CreateProcess instead of system(): avoids cmd.exe %VAR% expansion
    // inside quoted paths, and still searches PATH for vim
    STARTUPINFO si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;

    if (CreateProcess(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
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
    GetConsoleScreenBufferInfo(hAlt, &info);
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

void DrawCreatePopup(int width, char* input_text, CHAR_INFO* out_buffer) {
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

    // Label and input
    WriteToBuffer(out_buffer, width, 1, 2, "Name: ", white);
    WriteToBuffer(out_buffer, width, 1, 8, input_text, white);
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

    int popup_h = 3;

    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(hAlt, &info);
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
        DrawCreatePopup(popup_w, name, popup_buffer);

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

    if (name[0] == '\0') return;

    // Strip the trailing backslash (directory marker) before building the path
    // so the name is also usable for the cursor lookup below
    int len = (int)strlen(name);
    bool is_directory = (name[len - 1] == '\\');
    if (is_directory) {
        name[len - 1] = '\0';
        if (name[0] == '\0') return;
    }
    if (claude_mode == CM_WORKSPACES) {
        is_directory = true; // workspaces are always directories
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
    GetConsoleScreenBufferInfo(hAlt, &info);
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

        FILE* f = fopen(temp_path, "w");
        if (f) {
            fprintf(f, "%s", current_directory);
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
