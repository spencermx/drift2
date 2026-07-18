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

#define POPUP_WIDTH 35
#define POPUP_HEIGHT 15
#define CREATE_POPUP_WIDTH 50
#define COLUMN_DIVIDER_POSITION 28
#define MIN_WINDOW_WIDTH (COLUMN_DIVIDER_POSITION + 8)

enum MarkStatus {
    MARKED,
    YANKED,
    CUT
};
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
int GetFilesInDirectory(char* path, WIN32_FIND_DATA files[]);
int CompareFiles(const void* a, const void* b);
void GetParentDirectory(char* path, char* parent);
bool IsDirectory(WIN32_FIND_DATA* file_data);
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
WORD color;

HANDLE hIn;
HANDLE hOriginal;
HANDLE hAlt;
DWORD original_console_mode;

DirectoryState history[MAX_FILES];
MarkedFile marked_files[MAX_FILES];
WIN32_FIND_DATA current_directory_files[MAX_FILES];
WIN32_FIND_DATA parent_directory_files[MAX_FILES];

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

    // Window too small to draw the layout safely
    if (width < MIN_WINDOW_WIDTH || height < 1) {
        return;
    }

    // Re-clamp scroll state against the *current* window size. The window may
    // have been resized since the last keypress, which would otherwise let the
    // cursor draw below write past the end of the buffer.
    if (current_directory_file_count == 0) {
        selected_row = 0;
        top_row = 0;
    } else {
        if (selected_row >= current_directory_file_count) {
            selected_row = current_directory_file_count - 1;
        }
        if (top_row > current_directory_file_count - height) {
            top_row = current_directory_file_count - height;
        }
        if (top_row < 0) {
            top_row = 0;
        }
        if (top_row > selected_row) {
            top_row = selected_row;
        }
        if (selected_row - top_row >= height) {
            top_row = selected_row - height + 1;
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

    // ============================= Draw Parent Directory =============================
    for (int i = 0; i < height && i < parent_directory_file_count; i++) {
        int file_index = i;

        if (IsDirectory(&parent_directory_files[file_index])) {
            color = blue;
        } else {
            color = white;
        }

        AnsiToWide(parent_directory_files[file_index].cFileName, wname, MAX_PATH);
        int len = (int)wcslen(wname);

        for (int col = 0; col < len && col < COLUMN_DIVIDER_POSITION - 2; col++) {
            int index = i * width + col;
            buffer[index].Char.UnicodeChar = wname[col];
            buffer[index].Attributes = color;
        }
    }
    // ============================= Draw Parent Directory =============================

    // ============================= Draw Column Divider ===============================
    color = blue;
    for (int i = 0; i < height; i++) {
        int index = i * width + COLUMN_DIVIDER_POSITION;
        buffer[index].Char.UnicodeChar = BOX_VERTICAL;
        buffer[index].Attributes = color;
    }
    // ============================= Draw Column Divider ===============================

    // ============================= Draw Current Directory ============================
    for (int i = 0; i < height && top_row + i < current_directory_file_count; i++) {
        int file_index = top_row + i;

        if (IsDirectory(&current_directory_files[file_index])) {
            color = blue;
        } else {
            color = white;
        }

        AnsiToWide(current_directory_files[file_index].cFileName, wname, MAX_PATH);
        int len = (int)wcslen(wname);

        for (int col = 0; col < len && col + COLUMN_DIVIDER_POSITION + 4 < width; col++) {
            int index = i * width + COLUMN_DIVIDER_POSITION + 4 + col;
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
                        int mark_index = i * width + COLUMN_DIVIDER_POSITION + 1;

                        if (mark_status == MARKED) {
                            buffer[mark_index].Char.UnicodeChar = L'*';
                        } else if (mark_status == YANKED) {
                            buffer[mark_index].Char.UnicodeChar = L'Y';
                        } else if (mark_status == CUT) {
                            buffer[mark_index].Char.UnicodeChar = L'X';
                        }

                        buffer[mark_index].Attributes = white;
                        break;
                    }
                }
            }
        }
        // ================== Highlight marked files in the current directory ==================
    }
    // ============================= Draw Current Directory ============================

    // Draw the cursor (skip in empty directories -- there is nothing to point at)
    if (current_directory_file_count > 0) {
        int index = (selected_row - top_row) * width + COLUMN_DIVIDER_POSITION + 2;
        buffer[index].Char.UnicodeChar = L'>';
        buffer[index].Attributes = white;
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

    COORD cursor_position = {
        (SHORT)(info.srWindow.Left + COLUMN_DIVIDER_POSITION + 4),
        (SHORT)(info.srWindow.Top + selected_row - top_row)
    };
    SetConsoleCursorPosition(hAlt, cursor_position);
}

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

    if (shift) {
        pending_g = false;
        if (input.Event.KeyEvent.wVirtualKeyCode == 'G') {
            // Jump to bottom: any magnitude >= file count clamps to the last row
            ModifySelectedRow(current_directory_file_count);
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

    if (name[0] == '\0') return;

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

int GetVisibleRows() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(hAlt, &info);
    return info.srWindow.Bottom - info.srWindow.Top + 1;
}

bool IsDirectory(WIN32_FIND_DATA* file_data) {
    return (file_data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
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
}

void SetMarkStatus(enum MarkStatus status) {
    mark_status = status;
}

bool MarkedFilesFull() {
    return marked_files_count >= MAX_FILES;
}
