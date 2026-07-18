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
// - Enter    : Open file with vim (or default application if vim is not in PATH)
// Operations on marked files:
// - Space    : Toggle mark on selected file/directory
// - Y        : Mark selected files for copy (yank)
// - X        : Mark selected files for move (cut)
// - P        : Paste (copy/move) marked files to current directory
// - D        : Delete marked files (with confirmation)
// - Ctrl + A : Mark all files in the current directory
// - Ctrl + [ : Clear all marks
// Misc:
// - O        : Jump list of visited directories (farthest first) - press number to jump
// - A        : Create new file/directory (append '\' to name for directory)
//
// Known limitations: no UNC path support (drive-letter paths assumed),
// directories capped at MAX_FILES entries, paths capped at MAX_PATH.
//
// Compilation - x86_64-w64-mingw32-gcc drift.c -o drift.exe
//             - cl drift.c

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#pragma comment(lib, "shell32.lib")

// =========================== Types and Constants ===========================
#define MAX_FILES 4096
#define BOX_TOP_LEFT     L'\x250C'
#define BOX_TOP_RIGHT    L'\x2510'
#define BOX_BOTTOM_LEFT  L'\x2514'
#define BOX_BOTTOM_RIGHT L'\x2518'
#define BOX_HORIZONTAL   L'\x2500'
#define BOX_VERTICAL     L'\x2502'

#define DELETE_POPUP_WIDTH 35
#define DELETE_POPUP_HEIGHT 15
#define COLUMN_DIVIDER_POSITION 28
#define MIN_SCREEN_WIDTH (COLUMN_DIVIDER_POSITION + 8)

enum MarkStatus {
    MARKED,
    YANKED,
    CUT
};
typedef struct {
    WCHAR path[MAX_PATH];
    int selected_row;
    int top_row;
} DirectoryState;
typedef struct {
    WCHAR path[MAX_PATH];
    int row;
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
void ChangeCurrentDirectory(WCHAR* path);
int GetFilesInDirectory(WCHAR* path, WIN32_FIND_DATAW files[]);
void GetParentDirectory(WCHAR* path, WCHAR* parent);
bool IsDirectory(WIN32_FIND_DATAW* file_data);
void GetSelectedRowPath(int row, WCHAR* out_path);
void GetFilePath(WCHAR* directory, WIN32_FIND_DATAW* file_data, WCHAR* out_path);
void JoinPath(WCHAR* out_path, const WCHAR* directory, const WCHAR* name);
int GetVisibleRows();
bool IsRootDirectory(WCHAR* path);
void SaveDirectoryState();
void RestoreDirectoryState();
void Cleanup();
void Initialize();
void ApplyInputMode();
void ToggleMark();
bool MarkedFilesFull();
void SetMarkStatus(enum MarkStatus status);
void HandlePaste(int move);
bool IsMarkDirectorySet();
bool MarkDirEqualToCurrentDir();
bool ConfirmDelete();
void DrawDeletePopup(int width, int height, CHAR_INFO* out_buffer);
void DrawBox(CHAR_INFO* buffer, int width, int height);
void WriteToBuffer(CHAR_INFO* buffer, int width, int row, int col, const WCHAR* text, WORD color);
void ShowMessage(const WCHAR* msg);
WCHAR* BuildMarkedFileList();

void LoadParentDirectory();
void LoadCurrentDirectory();
void SyncMarkedRows();
void ClearMarkedFiles();
void DrawCreatePopup(int width, WCHAR* input_text, CHAR_INFO* out_buffer);
void HandleCreate();
void HandleMarkOperation(enum MarkStatus new_status);
void HandleOldHistory();
void DrawOldHistoryPopup(int width, int height, DistanceEntry* distances, int display_count, CHAR_INFO* out_buffer);
int CompareFiles(const void* a, const void* b);
// =========================== Function Declarations =========================

// =========================== Global Variables ==============================
static const WORD COLOR_WHITE = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
static const WORD COLOR_BLUE  = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static const WORD COLOR_RED   = FOREGROUND_RED | FOREGROUND_INTENSITY;
static const WORD COLOR_GRAY  = FOREGROUND_INTENSITY;

HANDLE hIn;
HANDLE hOriginal;
HANDLE hAlt;
DWORD original_input_mode;

DirectoryState history[MAX_FILES];
MarkedFile marked_files[MAX_FILES];
WIN32_FIND_DATAW current_directory_files[MAX_FILES];
WIN32_FIND_DATAW parent_directory_files[MAX_FILES];

WCHAR mark_directory[MAX_PATH];
WCHAR parent_directory[MAX_PATH];
WCHAR current_directory[MAX_PATH];

int history_count = 0;
int marked_files_count = 0;
int current_directory_file_count;
int parent_directory_file_count;

int selected_row = 0;
int top_row = 0;

bool pending_g = false;

enum MarkStatus mark_status = MARKED;
// =========================== Global Variables ==============================
int main(void) {
    Initialize();

    do {
        DrawScreen();
    } while (HandleInput());

    Cleanup();

    return 0;
}

void Initialize() {
    hIn = GetStdHandle(STD_INPUT_HANDLE);
    hOriginal = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleMode(hIn, &original_input_mode);

    // Create an alternate screen buffer sized to the visible window so that
    // buffer coordinates and window coordinates always agree
    hAlt = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, 0, NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
    if (hAlt == INVALID_HANDLE_VALUE) {
        hAlt = hOriginal; // Degrade gracefully: draw into the main buffer
    }

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(hOriginal, &info)) {
        COORD size = {
            (SHORT)(info.srWindow.Right - info.srWindow.Left + 1),
            (SHORT)(info.srWindow.Bottom - info.srWindow.Top + 1)
        };
        SetConsoleScreenBufferSize(hAlt, size);
    }
    SetConsoleActiveScreenBuffer(hAlt);

    ApplyInputMode();

    // Set initial directory (use a temp buffer: passing current_directory into
    // ChangeCurrentDirectory would make it strcpy onto itself)
    WCHAR initial_directory[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, initial_directory);
    ChangeCurrentDirectory(initial_directory);
}

void ApplyInputMode() {
    // ENABLE_WINDOW_INPUT delivers resize events so the screen redraws immediately.
    // Re-applied after running vim, which may change console modes.
    SetConsoleMode(hIn, (original_input_mode & ~ENABLE_PROCESSED_INPUT) | ENABLE_WINDOW_INPUT);
}

void DrawScreen() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
    int width = info.srWindow.Right - info.srWindow.Left + 1;
    int height = info.srWindow.Bottom - info.srWindow.Top + 1;

    // Too small to draw the two-pane layout safely
    if (width < MIN_SCREEN_WIDTH || height < 1) return;

    // Re-clamp scroll state against the *current* window height. The window may
    // have shrunk since the last input, which previously caused the cursor write
    // below to index past the end of the frame buffer.
    if (current_directory_file_count == 0) {
        selected_row = 0;
        top_row = 0;
    }
    if (selected_row < top_row) top_row = selected_row;
    if (selected_row - top_row >= height) top_row = selected_row - height + 1;
    if (top_row < 0) top_row = 0;

    // Persistent frame buffer, grown on demand instead of malloc/free per frame
    static CHAR_INFO* buffer = NULL;
    static int buffer_cells = 0;
    if (width * height > buffer_cells) {
        free(buffer);
        buffer = (CHAR_INFO*)malloc((size_t)width * height * sizeof(CHAR_INFO));
        buffer_cells = buffer ? width * height : 0;
    }
    if (!buffer) return;

    // Fill with spaces and default color
    for (int i = 0; i < width * height; i++) {
        buffer[i].Char.UnicodeChar = L' ';
        buffer[i].Attributes = COLOR_WHITE;
    }

    // ============================= Draw Parent Directory =============================
    for (int i = 0; i < height && i < parent_directory_file_count; i++) {
        WORD color = IsDirectory(&parent_directory_files[i]) ? COLOR_BLUE : COLOR_WHITE;
        WCHAR* file_name = parent_directory_files[i].cFileName;
        int len = (int)wcslen(file_name);

        for (int col = 0; col < len && col < COLUMN_DIVIDER_POSITION - 2; col++) {
            int index = i * width + col;
            buffer[index].Char.UnicodeChar = file_name[col];
            buffer[index].Attributes = color;
        }
    }
    // ============================= Draw Parent Directory =============================

    // ============================= Draw Column Divider ===============================
    for (int i = 0; i < height; i++) {
        int index = i * width + COLUMN_DIVIDER_POSITION;
        buffer[index].Char.UnicodeChar = BOX_VERTICAL;
        buffer[index].Attributes = COLOR_BLUE;
    }
    // ============================= Draw Column Divider ===============================

    // ============================= Draw Current Directory ============================
    if (current_directory_file_count == 0) {
        WriteToBuffer(buffer, width, 0, COLUMN_DIVIDER_POSITION + 4, L"(empty)", COLOR_GRAY);
    }

    for (int i = 0; i < height && top_row + i < current_directory_file_count; i++) {
        int file_index = top_row + i;
        WORD color = IsDirectory(&current_directory_files[file_index]) ? COLOR_BLUE : COLOR_WHITE;
        WCHAR* file_name = current_directory_files[file_index].cFileName;
        int len = (int)wcslen(file_name);

        for (int col = 0; col < len && col + COLUMN_DIVIDER_POSITION + 4 < width; col++) {
            int index = i * width + COLUMN_DIVIDER_POSITION + 4 + col;
            buffer[index].Char.UnicodeChar = file_name[col];
            buffer[index].Attributes = color;
        }
    }

    // ================== Highlight marked files in the current directory ==================
    if (MarkDirEqualToCurrentDir()) {
        WCHAR glyph = (mark_status == MARKED) ? L'*' : (mark_status == YANKED) ? L'Y' : L'X';
        for (int j = 0; j < marked_files_count; j++) {
            int marked_row = marked_files[j].row;
            if (marked_row >= top_row && marked_row < top_row + height) {
                int index = (marked_row - top_row) * width + COLUMN_DIVIDER_POSITION + 1;
                buffer[index].Char.UnicodeChar = glyph;
                buffer[index].Attributes = COLOR_WHITE;
            }
        }
    }
    // ================== Highlight marked files in the current directory ==================
    // ============================= Draw Current Directory ============================

    // Draw the cursor
    if (current_directory_file_count > 0) {
        int index = (selected_row - top_row) * width + COLUMN_DIVIDER_POSITION + 2;
        buffer[index].Char.UnicodeChar = L'>';
        buffer[index].Attributes = COLOR_WHITE;
    }

    COORD buffer_size = { (SHORT)width, (SHORT)height };
    COORD origin = { 0, 0 };
    SMALL_RECT region = { 0, 0, (SHORT)(width - 1), (SHORT)(height - 1) };
    WriteConsoleOutputW(hAlt, buffer, buffer_size, origin, &region);

    COORD cursor_position = { (SHORT)(COLUMN_DIVIDER_POSITION + 4), (SHORT)(selected_row - top_row) };
    SetConsoleCursorPosition(hAlt, cursor_position);
}

int HandleInput() {
    INPUT_RECORD input;
    DWORD events;
    ReadConsoleInputW(hIn, &input, 1, &events);

    if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) {
        return 1; // Ignore non-key events (resize events land here and trigger a redraw)
    }

    BOOL ctrl = input.Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
    BOOL shift = input.Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED;
    WORD vk = input.Event.KeyEvent.wVirtualKeyCode;

    if (shift) {
        pending_g = false;
        if (vk == 'G') {
            // Jump by the full list length; ModifySelectedRow clamps to the bottom
            ModifySelectedRow(current_directory_file_count);
        }
    }
    else if (ctrl) {
        pending_g = false;
        switch (vk) {
            case 'D': {
                int half = GetVisibleRows() / 2;
                ModifySelectedRow(half > 0 ? half : 1); // Half page down
                break;
            }
            case 'U': {
                int half = GetVisibleRows() / 2;
                ModifySelectedRow(half > 0 ? -half : -1); // Half page up
                break;
            }
            case 'A': {
                if (current_directory_file_count == 0) break;

                ClearMarkedFiles();
                wcscpy(mark_directory, current_directory);

                for (int i = 0; i < current_directory_file_count; i++) {
                    GetFilePath(current_directory, &current_directory_files[i], marked_files[i].path);
                    marked_files[i].row = i;
                }
                marked_files_count = current_directory_file_count;
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

        switch (vk) {
            case 'A': {
                HandleCreate();
                break;
            }
            case 'H': {
                if (parent_directory[0] != L'\0') {
                    ChangeCurrentDirectory(parent_directory);
                }
                break;
            }
            case 'L': {
                if (current_directory_file_count == 0) break;
                if (IsDirectory(&current_directory_files[selected_row])) {
                    WCHAR selected_directory_path[MAX_PATH];
                    GetSelectedRowPath(selected_row, selected_directory_path);
                    ChangeCurrentDirectory(selected_directory_path);
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
                bool auto_marked = false;

                if (!(IsMarkDirectorySet() && MarkDirEqualToCurrentDir() && marked_files_count > 0)) {
                    if (current_directory_file_count == 0) break;
                    ClearMarkedFiles();
                    wcscpy(mark_directory, current_directory);
                    ToggleMark();
                    auto_marked = true;
                }

                // Cancelling keeps a pre-existing mark set; only the auto-mark is reverted
                if (!ConfirmDelete() && auto_marked) {
                    ClearMarkedFiles();
                }
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
                    wcscpy(mark_directory, current_directory);
                }

                ToggleMark();
                ModifySelectedRow(1);
                break;
            }
            case VK_RETURN: {
                if (current_directory_file_count == 0) break;
                if (IsDirectory(&current_directory_files[selected_row])) break;

                WCHAR file_path[MAX_PATH];
                GetSelectedRowPath(selected_row, file_path);

                WCHAR command[MAX_PATH + 16];
                StringCchPrintfW(command, ARRAYSIZE(command), L"vim \"%ls\"", file_path);

                // Temporarily switch to the original buffer to run the editor
                SetConsoleActiveScreenBuffer(hOriginal);

                STARTUPINFOW si = {0};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi = {0};
                bool opened = false;

                // CreateProcess searches PATH and does no cmd-style %VAR% expansion
                if (CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                    WaitForSingleObject(pi.hProcess, INFINITE);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    opened = true;
                } else {
                    // vim not found - fall back to the default application
                    opened = (INT_PTR)ShellExecuteW(NULL, L"open", file_path, NULL, NULL, SW_SHOWNORMAL) > 32;
                }

                SetConsoleActiveScreenBuffer(hAlt);
                ApplyInputMode();       // vim may have altered console modes
                LoadCurrentDirectory(); // pick up files created/changed by the editor

                if (!opened) {
                    ShowMessage(L"Could not open file.");
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

void HandleMarkOperation(enum MarkStatus new_status) {
    // If there is a valid mark set in this directory, Y/X apply to it.
    // Otherwise mark the cursor line and apply to that.
    if (marked_files_count == 0 || !MarkDirEqualToCurrentDir()) {
        if (current_directory_file_count == 0) return;
        ClearMarkedFiles();
        wcscpy(mark_directory, current_directory);
        ToggleMark();
        if (marked_files_count == 0) return;
    }
    SetMarkStatus(new_status);
}

WCHAR* BuildMarkedFileList() {
    // Builds the double-null-terminated list SHFileOperation expects,
    // so the whole batch is one operation (and one undo unit)
    size_t total = 1;
    for (int i = 0; i < marked_files_count; i++) {
        total += wcslen(marked_files[i].path) + 1;
    }

    WCHAR* list = (WCHAR*)malloc(total * sizeof(WCHAR));
    if (!list) return NULL;

    size_t pos = 0;
    for (int i = 0; i < marked_files_count; i++) {
        size_t len = wcslen(marked_files[i].path);
        wcscpy(list + pos, marked_files[i].path);
        pos += len + 1;
    }
    list[pos] = L'\0';

    return list;
}

void HandlePaste(int move) {
    if (marked_files_count == 0 || !IsMarkDirectorySet()) return;

    // Cutting and pasting into the same directory is a no-op
    if (move && MarkDirEqualToCurrentDir()) {
        ClearMarkedFiles();
        return;
    }

    WCHAR* source_list = BuildMarkedFileList();
    if (!source_list) return;

    WCHAR destination[MAX_PATH + 2];
    wcscpy(destination, current_directory);
    destination[wcslen(destination) + 1] = L'\0'; // double-null terminate

    SHFILEOPSTRUCTW op = {0};
    op.wFunc = move ? FO_MOVE : FO_COPY;
    op.pFrom = source_list;
    op.pTo = destination;
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_RENAMEONCOLLISION;

    int result = SHFileOperationW(&op);
    bool aborted = op.fAnyOperationsAborted;

    free(source_list);

    LoadCurrentDirectory();
    ClearMarkedFiles();

    if (result != 0 || aborted) {
        ShowMessage(L"Some files could not be pasted.");
    }
}

void ToggleMark() {
    for (int i = 0; i < marked_files_count; i++) {
        if (marked_files[i].row == selected_row) {
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
        GetFilePath(current_directory, &current_directory_files[selected_row], marked_files[marked_files_count].path);
        marked_files[marked_files_count].row = selected_row;
        marked_files_count++;

        wcscpy(mark_directory, current_directory);
    }
}

void ChangeCurrentDirectory(WCHAR* path) {
    SaveDirectoryState();

    wcscpy(current_directory, path);
    LoadCurrentDirectory();
    if (!IsRootDirectory(current_directory)) {
        LoadParentDirectory();
    } else {
        parent_directory[0] = L'\0';
        parent_directory_file_count = 0;
    }

    RestoreDirectoryState();
}

void GetParentDirectory(WCHAR* path, WCHAR* parent) {
    wcscpy(parent, path);

    WCHAR* last_slash = wcsrchr(parent, L'\\');
    if (last_slash == NULL) {
        // No slash found, treat as root
        parent[0] = L'\0';
    } else if (last_slash == parent + 2 && parent[1] == L':') {
        // Handle root directory case (e.g., "C:\")
        *(last_slash + 1) = L'\0';
    }
    else {
        *last_slash = L'\0';
    }
}

int CompareFiles(const void* a, const void* b) {
    const WIN32_FIND_DATAW* fa = (const WIN32_FIND_DATAW*)a;
    const WIN32_FIND_DATAW* fb = (const WIN32_FIND_DATAW*)b;

    bool dir_a = (fa->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool dir_b = (fb->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (dir_a != dir_b) {
        return dir_b - dir_a; // directories first
    }
    return _wcsicmp(fa->cFileName, fb->cFileName);
}

int GetFilesInDirectory(WCHAR* path, WIN32_FIND_DATAW files[]) {
    WCHAR search_path[MAX_PATH];
    JoinPath(search_path, path, L"*");

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search_path, &fd);

    int count = 0;

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0) continue;
            if (wcscmp(fd.cFileName, L"..") == 0) continue;

            files[count] = fd;
            count++;
        } while (FindNextFileW(hFind, &fd) && count < MAX_FILES);

        FindClose(hFind);
    }

    qsort(files, count, sizeof(WIN32_FIND_DATAW), CompareFiles);

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
    // Check if the current directory is already in history
    for (int i = 0; i < history_count; i++) {
        if (wcscmp(history[i].path, current_directory) == 0) {
            history[i].selected_row = selected_row;
            history[i].top_row = top_row;
            return;
        }
    }

    // Add new entry to history
    if (history_count < MAX_FILES) {
        wcscpy(history[history_count].path, current_directory);
        history[history_count].selected_row = selected_row;
        history[history_count].top_row = top_row;
        history_count++;
    }
}

void RestoreDirectoryState() {
    for (int i = 0; i < history_count; i++) {
        if (wcscmp(history[i].path, current_directory) == 0) {
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

bool ConfirmDelete() {
    if (marked_files_count == 0) return false;

    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(hAlt, &info);
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

    CHAR_INFO* popup_buffer = (CHAR_INFO*)malloc(DELETE_POPUP_WIDTH * DELETE_POPUP_HEIGHT * sizeof(CHAR_INFO));
    if (!popup_buffer) return false;
    DrawDeletePopup(DELETE_POPUP_WIDTH, DELETE_POPUP_HEIGHT, popup_buffer);

    // Center the popup on screen
    int start_col = (screen_width - DELETE_POPUP_WIDTH) / 2;
    int start_row = (screen_height - DELETE_POPUP_HEIGHT) / 2;
    if (start_col < 0) start_col = 0;
    if (start_row < 0) start_row = 0;

    COORD buffer_size = { DELETE_POPUP_WIDTH, DELETE_POPUP_HEIGHT };
    COORD origin = { 0, 0 };
    SMALL_RECT region = { (SHORT)start_col, (SHORT)start_row,
                          (SHORT)(start_col + DELETE_POPUP_WIDTH - 1), (SHORT)(start_row + DELETE_POPUP_HEIGHT - 1) };
    WriteConsoleOutputW(hAlt, popup_buffer, buffer_size, origin, &region);

    free(popup_buffer);

    // Wait for input
    while (1) {
        INPUT_RECORD input;
        DWORD events;
        ReadConsoleInputW(hIn, &input, 1, &events);

        if (input.EventType == KEY_EVENT && input.Event.KeyEvent.bKeyDown) {
            if (input.Event.KeyEvent.wVirtualKeyCode == 'Y') {
                WCHAR* source_list = BuildMarkedFileList();
                if (!source_list) return false;

                SHFILEOPSTRUCTW op = {0};
                op.wFunc = FO_DELETE;
                op.pFrom = source_list;
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;

                int result = SHFileOperationW(&op);
                bool aborted = op.fAnyOperationsAborted;

                free(source_list);

                LoadCurrentDirectory();
                ClearMarkedFiles();

                if (result != 0 || aborted) {
                    ShowMessage(L"Some files could not be deleted.");
                }
                return true;
            } else if (input.Event.KeyEvent.wVirtualKeyCode == 'N' ||
                       input.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) {
                return false; // Marks are kept on cancel
            }
        }
    }
}

void DrawBox(CHAR_INFO* buffer, int width, int height) {
    // Fill background with spaces
    for (int i = 0; i < width * height; i++) {
        buffer[i].Char.UnicodeChar = L' ';
        buffer[i].Attributes = COLOR_WHITE;
    }

    // Corners
    int bottom = (height - 1) * width;
    buffer[0].Char.UnicodeChar = BOX_TOP_LEFT;
    buffer[width - 1].Char.UnicodeChar = BOX_TOP_RIGHT;
    buffer[bottom].Char.UnicodeChar = BOX_BOTTOM_LEFT;
    buffer[bottom + width - 1].Char.UnicodeChar = BOX_BOTTOM_RIGHT;

    // Top and bottom edges
    for (int col = 1; col < width - 1; col++) {
        buffer[col].Char.UnicodeChar = BOX_HORIZONTAL;
        buffer[bottom + col].Char.UnicodeChar = BOX_HORIZONTAL;
    }

    // Left and right edges
    for (int row = 1; row < height - 1; row++) {
        buffer[row * width].Char.UnicodeChar = BOX_VERTICAL;
        buffer[row * width + width - 1].Char.UnicodeChar = BOX_VERTICAL;
    }
}

void DrawDeletePopup(int width, int height, CHAR_INFO* out_buffer) {
    DrawBox(out_buffer, width, height);

    // Title (row 1, centered)
    const WCHAR* title = L"Confirm Delete?";
    int title_col = (width - (int)wcslen(title)) / 2;
    WriteToBuffer(out_buffer, width, 1, title_col, title, COLOR_RED);

    // Item count (row 3)
    WCHAR count_msg[50];
    StringCchPrintfW(count_msg, ARRAYSIZE(count_msg), L"%d item(s) will be deleted:", marked_files_count);
    WriteToBuffer(out_buffer, width, 3, 2, count_msg, COLOR_WHITE);

    // File names (rows 5-9, max 5 items)
    int max_display = 5;
    for (int i = 0; i < marked_files_count && i < max_display; i++) {
        WCHAR* name = wcsrchr(marked_files[i].path, L'\\');
        name = name ? name + 1 : marked_files[i].path;
        WriteToBuffer(out_buffer, width, 5 + i, 4, name, COLOR_WHITE);
    }

    // "...and N more" if needed
    if (marked_files_count > max_display) {
        WCHAR more_msg[30];
        StringCchPrintfW(more_msg, ARRAYSIZE(more_msg), L"...and %d more", marked_files_count - max_display);
        WriteToBuffer(out_buffer, width, 5 + max_display, 4, more_msg, COLOR_WHITE);
    }

    // Y/N options (bottom rows)
    WriteToBuffer(out_buffer, width, height - 4, 2, L"[Y] Yes - Move to Recycle Bin", COLOR_WHITE);
    WriteToBuffer(out_buffer, width, height - 3, 2, L"[N] No - Cancel", COLOR_WHITE);
}

void ShowMessage(const WCHAR* msg) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hAlt, &info)) return;
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

    int width = (int)wcslen(msg) + 6;
    if (width > screen_width - 2) width = screen_width - 2;
    if (width < 10) return;
    int height = 3;

    CHAR_INFO* buffer = (CHAR_INFO*)malloc(width * height * sizeof(CHAR_INFO));
    if (!buffer) return;

    DrawBox(buffer, width, height);
    WriteToBuffer(buffer, width, 1, 2, msg, COLOR_WHITE);

    int start_col = (screen_width - width) / 2;
    int start_row = (screen_height - height) / 2;

    COORD buffer_size = { (SHORT)width, (SHORT)height };
    COORD origin = { 0, 0 };
    SMALL_RECT region = { (SHORT)start_col, (SHORT)start_row,
                          (SHORT)(start_col + width - 1), (SHORT)(start_row + height - 1) };
    WriteConsoleOutputW(hAlt, buffer, buffer_size, origin, &region);

    free(buffer);

    // Wait for any key
    while (1) {
        INPUT_RECORD input;
        DWORD events;
        ReadConsoleInputW(hIn, &input, 1, &events);
        if (input.EventType == KEY_EVENT && input.Event.KeyEvent.bKeyDown) break;
    }
}

void DrawCreatePopup(int width, WCHAR* input_text, CHAR_INFO* out_buffer) {
    DrawBox(out_buffer, width, 3);
    WriteToBuffer(out_buffer, width, 1, 2, L"Name: ", COLOR_WHITE);
    WriteToBuffer(out_buffer, width, 1, 8, input_text, COLOR_WHITE);
}

void HandleCreate() {
    WCHAR name[MAX_PATH];
    name[0] = L'\0';
    int pos = 0;

    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(hAlt, &info);
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;

    // Size the popup to the window instead of a fixed 35 columns
    int popup_w = screen_width - 10;
    if (popup_w > 70) popup_w = 70;
    if (popup_w < 30) popup_w = 30;
    if (popup_w > screen_width - 2) return;
    int popup_h = 3;

    int max_name_len = popup_w - 10;
    if (max_name_len > MAX_PATH - 1) max_name_len = MAX_PATH - 1;

    int start_col = (screen_width - popup_w) / 2;
    int start_row = (screen_height - popup_h) / 2;

    CHAR_INFO* popup_buffer = (CHAR_INFO*)malloc(popup_w * popup_h * sizeof(CHAR_INFO));
    if (!popup_buffer) return;

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
        ReadConsoleInputW(hIn, &input, 1, &events);

        if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) {
            continue;
        }

        WORD vk = input.Event.KeyEvent.wVirtualKeyCode;
        WCHAR c = input.Event.KeyEvent.uChar.UnicodeChar;

        if (vk == VK_RETURN) {
            break;
        } else if (vk == VK_ESCAPE) {
            name[0] = L'\0';
            break;
        } else if (vk == VK_BACK && pos > 0) {
            pos--;
            name[pos] = L'\0';
        } else if (c >= 32 && c != 127 && pos < max_name_len) {
            name[pos] = c;
            pos++;
            name[pos] = L'\0';
        }
    }

    free(popup_buffer);

    if (name[0] == L'\0') return;

    size_t len = wcslen(name);
    bool is_directory = name[len - 1] == L'\\';
    if (is_directory) {
        name[len - 1] = L'\0';
        if (name[0] == L'\0') return; // name was just "\"
    }

    WCHAR full_path[MAX_PATH];
    JoinPath(full_path, current_directory, name);

    if (is_directory) {
        if (!CreateDirectoryW(full_path, NULL)) {
            ShowMessage(L"Could not create directory.");
        }
    } else {
        HANDLE h = CreateFileW(full_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        } else {
            ShowMessage(L"Could not create file.");
        }
    }

    LoadCurrentDirectory();
}

int CalculateDistance(WCHAR* current, WCHAR* target) {
    // Check if target is below current (current is prefix of target)
    size_t current_len = wcslen(current);
    if (wcsncmp(current, target, current_len) == 0 &&
        (target[current_len] == L'\\' || target[current_len] == L'\0')) {
        // Count slashes after current path
        int distance = 0;
        for (size_t i = current_len; target[i] != L'\0'; i++) {
            if (target[i] == L'\\') distance++;
        }
        return distance;
    }

    // Otherwise, find common ancestor
    WCHAR temp[MAX_PATH];
    wcscpy(temp, current);
    int steps_up = 0;

    while (temp[0] != L'\0') {
        size_t temp_len = wcslen(temp);
        if (wcsncmp(temp, target, temp_len) == 0 &&
            (target[temp_len] == L'\\' || target[temp_len] == L'\0')) {
            // Found common ancestor, count steps down
            int steps_down = 0;
            for (size_t i = temp_len; target[i] != L'\0'; i++) {
                if (target[i] == L'\\') steps_down++;
            }
            return steps_up + steps_down;
        }

        // Cut off last directory
        WCHAR* last_slash = wcsrchr(temp, L'\\');
        if (last_slash == NULL || last_slash == temp + 2) {
            break; // Hit root or different drive
        }
        *last_slash = L'\0';
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

    // Sort by distance, farthest first: nearby directories are cheap to reach
    // with H/L, so the jump list prioritizes the expensive ones.
    // (Flip the comparison to '>' for nearest-first.)
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
    if (popup_width > screen_width - 2) popup_width = screen_width - 2;
    if (popup_width < 20 || popup_height > screen_height) return;

    CHAR_INFO* popup_buffer = (CHAR_INFO*)malloc(popup_width * popup_height * sizeof(CHAR_INFO));
    if (!popup_buffer) return;
    DrawOldHistoryPopup(popup_width, popup_height, distances, display_count, popup_buffer);

    // Center the popup on screen
    int start_col = (screen_width - popup_width) / 2;
    int start_row = (screen_height - popup_height) / 2;

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
        ReadConsoleInputW(hIn, &input, 1, &events);

        if (input.EventType == KEY_EVENT && input.Event.KeyEvent.bKeyDown) {
            WCHAR c = input.Event.KeyEvent.uChar.UnicodeChar;

            if (c >= L'1' && c <= L'9') {
                int selected = c - L'1';
                if (selected < display_count) {
                    ChangeCurrentDirectory(history[distances[selected].index].path);
                    break;
                }
            }
            else if (c == L'0' && display_count == 10) {
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
    DrawBox(out_buffer, width, height);

    // Title
    const WCHAR* title = L"Jump to Directory";
    int title_col = (width - (int)wcslen(title)) / 2;
    WriteToBuffer(out_buffer, width, 1, title_col, title, COLOR_BLUE);

    // List entries
    for (int i = 0; i < display_count; i++) {
        WCHAR line[MAX_PATH + 8];
        int num = (i == 9) ? 0 : i + 1;
        StringCchPrintfW(line, ARRAYSIZE(line), L"[%d] %ls", num, history[distances[i].index].path);
        WriteToBuffer(out_buffer, width, 3 + i, 2, line, COLOR_WHITE);
    }

    // Instructions
    WriteToBuffer(out_buffer, width, height - 2, 2, L"Press 1-9/0 to jump, ESC to cancel", COLOR_WHITE);
}

void Cleanup() {
    SetConsoleActiveScreenBuffer(hOriginal);
    SetConsoleMode(hIn, original_input_mode);
    if (hAlt != hOriginal) {
        CloseHandle(hAlt);
    }

    // Write the last directory for shell integration (same file name as before;
    // content is now UTF-8, which is byte-identical to ANSI for ASCII paths)
    WCHAR temp_dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TEMP", temp_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    WCHAR file_path[MAX_PATH];
    JoinPath(file_path, temp_dir, L"browser_lastdir.txt");

    HANDLE h = CreateFileW(file_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    char utf8[MAX_PATH * 4];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, current_directory, -1, utf8, sizeof(utf8), NULL, NULL);
    if (bytes > 1) {
        DWORD written;
        WriteFile(h, utf8, bytes - 1, &written, NULL); // bytes includes the null terminator
    }
    CloseHandle(h);
}

void WriteToBuffer(CHAR_INFO* buffer, int width, int row, int col, const WCHAR* text, WORD color) {
    int len = (int)wcslen(text);
    for (int i = 0; i < len && col + i < width - 1; i++) {
        int index = row * width + col + i;
        buffer[index].Char.UnicodeChar = text[i];
        buffer[index].Attributes = color;
    }
}

void ClearMarkedFiles() {
    mark_directory[0] = L'\0';
    marked_files_count = 0;
    SetMarkStatus(MARKED);
}

// After the current directory listing changes (create, delete, paste, edits
// made in vim), re-find each mark's row by file name and drop marks whose
// files no longer exist. Keeps highlights and D/P targets correct.
void SyncMarkedRows() {
    if (!IsMarkDirectorySet() || !MarkDirEqualToCurrentDir()) return;

    int write = 0;
    for (int i = 0; i < marked_files_count; i++) {
        WCHAR* base = wcsrchr(marked_files[i].path, L'\\');
        base = base ? base + 1 : marked_files[i].path;

        int found = -1;
        for (int j = 0; j < current_directory_file_count; j++) {
            if (wcscmp(current_directory_files[j].cFileName, base) == 0) {
                found = j;
                break;
            }
        }

        if (found >= 0) {
            marked_files[write] = marked_files[i];
            marked_files[write].row = found;
            write++;
        }
    }
    marked_files_count = write;

    if (marked_files_count == 0) {
        ClearMarkedFiles();
    }
}

int GetVisibleRows() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(hAlt, &info);
    return info.srWindow.Bottom - info.srWindow.Top + 1;
}

bool IsDirectory(WIN32_FIND_DATAW* file_data) {
    return (file_data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void GetSelectedRowPath(int row, WCHAR* out_path) {
    JoinPath(out_path, current_directory, current_directory_files[row].cFileName);
}

void LoadParentDirectory() {
    GetParentDirectory(current_directory, parent_directory);
    parent_directory_file_count = GetFilesInDirectory(parent_directory, parent_directory_files);
}

void GetFilePath(WCHAR* directory, WIN32_FIND_DATAW* file, WCHAR* out_path) {
    JoinPath(out_path, directory, file->cFileName);
}

void JoinPath(WCHAR* out_path, const WCHAR* directory, const WCHAR* name) {
    size_t len = 0;
    while (len < MAX_PATH - 1 && directory[len] != L'\0') {
        out_path[len] = directory[len];
        len++;
    }
    // Avoid a double backslash when the directory is a root like "C:\"
    if (len > 0 && len < MAX_PATH - 1 && out_path[len - 1] != L'\\') {
        out_path[len++] = L'\\';
    }
    for (size_t i = 0; len < MAX_PATH - 1 && name[i] != L'\0'; i++) {
        out_path[len++] = name[i];
    }
    out_path[len] = L'\0';
}

bool IsRootDirectory(WCHAR* path) {
    return wcslen(path) == 3 && path[1] == L':' && path[2] == L'\\';
}

bool IsMarkDirectorySet() {
    return mark_directory[0] != L'\0';
}

bool MarkDirEqualToCurrentDir() {
    return wcscmp(mark_directory, current_directory) == 0;
}

void LoadCurrentDirectory() {
    current_directory_file_count = GetFilesInDirectory(current_directory, current_directory_files);

    // Clamp the cursor instead of resetting to the top, so reloads after
    // delete/paste/create/vim keep you roughly where you were
    if (selected_row >= current_directory_file_count) {
        selected_row = current_directory_file_count > 0 ? current_directory_file_count - 1 : 0;
    }
    int visible = GetVisibleRows();
    if (top_row > selected_row) top_row = selected_row;
    if (selected_row - top_row >= visible) top_row = selected_row - visible + 1;
    if (top_row < 0) top_row = 0;

    SyncMarkedRows();
}

void SetMarkStatus(enum MarkStatus status) {
    mark_status = status;
}

bool MarkedFilesFull() {
    return marked_files_count >= MAX_FILES;
}
