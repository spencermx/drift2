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
// - Ctrl + D : Page Down
// - Ctrl + U : Page Up
// - Enter    : Open file with default application (or vim if in PATH)
// Operations on marked files:
// - Space    : Toggle mark on selected file/directory
// - Y        : Mark selected files for copy (yank)
// - X        : Mark selected files for move (cut)
// - P        : Paste (copy/move) marked files to current directory 
// - D        : Delete marked files (with confirmation)
// - Ctrl + A : Mark all files in the current Directory
// - Ctrl + [ : Clear all mark_status
// Misc:
// - O        : Show recently visited directories (history) and jump to selected one
// - A        : Create new file/directory (append '\' to name for directory)
//
// Compilation - x86_64-w64-mingw32-gcc drift.c -o drift.exe 
//             - cl drift.c

#define _CRT_SECURE_NO_WARNINGS
#undef UNICODE
#undef _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdbool.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

// =========================== Types and Constants =========================== 
#define MAX_FILES 4096
#define BOX_TOP_LEFT     218
#define BOX_TOP_RIGHT    191
#define BOX_BOTTOM_LEFT  192
#define BOX_BOTTOM_RIGHT 217
#define BOX_HORIZONTAL   196
#define BOX_VERTICAL     179

#define PAGE_SIZE 35
#define POPUP_WIDTH 35
#define POPUP_HEIGHT 15
#define COLUMN_DIVIDER_POSITION 28
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
    int index;
    int distance;
} DistanceEntry;
// =========================== Types and Constants =========================== 

// =========================== Function Declarations =========================
void DrawScreen();
int HandleInput();
void ModifySelectedRow(int num);
void ChangeCurrentDirectory(char* path);
int GetFilesInDirectory(char* path, WIN32_FIND_DATA current_directory_files[]);
void GetParentDirectory(char* path, char* parent);
bool IsDirectory(WIN32_FIND_DATA* file_data);
void GetSelectedRowPath(int selected_row, char* out_path);
void GetFilePath(char* current_directory, WIN32_FIND_DATA* file_data, char* out_path);
int GetVisibleRows();
void DrawParent();
void DrawCurrent();
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

void LoadParentDirectory();
void LoadCurrentDirectory();
void ClearMarkedFiles();
void DrawCreatePopup(int width, char* input_text, CHAR_INFO* out_buffer);
void HandleCreate();
void HandleMarkOperation(enum MarkStatus new_status);
void HandleOldHistory();
void DrawOldHistoryPopup(int width, int height, DistanceEntry* distances, int display_count, CHAR_INFO* out_buffer);
// =========================== Function Declarations =========================

// =========================== Global Variables ==============================
WORD white = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
WORD blue = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
WORD color;

HANDLE hIn;
HANDLE hOriginal;
HANDLE hAlt;

DirectoryState history[MAX_FILES];
DirectoryState marked_files[MAX_FILES];
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

    // Set initial directory
    GetCurrentDirectory(MAX_PATH, current_directory);
    ChangeCurrentDirectory(current_directory);

    DWORD mode;
    GetConsoleMode(hIn, &mode);
    mode &= ~ENABLE_PROCESSED_INPUT;
    SetConsoleMode(hIn, mode);
}

void DrawScreen() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(hAlt, &info);
    int width = info.srWindow.Right - info.srWindow.Left + 1;
    int height = info.srWindow.Bottom - info.srWindow.Top + 1;

    color = white;

    // Allocate a buffer for the entire screen
    CHAR_INFO* buffer = (CHAR_INFO*)malloc(width * height * sizeof(CHAR_INFO));

    // Fill with spaces and default color  
    for (int i = 0; i < width * height; i++) {
        buffer[i].Char.AsciiChar = ' ';
        buffer[i].Attributes = color;
    }

    // ============================= Draw Parent Directory =============================
    for (int i = 0; i < height && i < parent_directory_file_count; i++) {
        int file_index = i;

        if (IsDirectory(&parent_directory_files[file_index])) {
            color = blue;
        } else {
            color = white;
        }

        char* file_name = parent_directory_files[file_index].cFileName;
        int len = strlen(file_name);

        for (int col = 0; col < len && col < COLUMN_DIVIDER_POSITION - 2; col++) {
            int index = i * width + col;
            buffer[index].Char.AsciiChar = file_name[col];
            buffer[index].Attributes = color;
        }
    }
    // ============================= Draw Parent Directory =============================

    // ============================= Draw Column Divider ===============================
    color = blue;
    for (int i = 0; i < height; i++) {
        int index = i * width + COLUMN_DIVIDER_POSITION;
        buffer[index].Char.AsciiChar = '|';
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

        char* file_name = current_directory_files[file_index].cFileName;
        int len = strlen(file_name);

        for (int col = 0; col < len && col + COLUMN_DIVIDER_POSITION + 4 < width; col++) {
            int index = i * width + COLUMN_DIVIDER_POSITION + 4 + col;
            buffer[index].Char.AsciiChar = file_name[col];
            buffer[index].Attributes = color;
        }

        // Skip marked files highlighting if the mark directory is different from the current directory
        if (!MarkDirEqualToCurrentDir()) {
            continue;
        }

        // ================== Highlight marked files in the current directory ================== 
        color = white;
        for (int j = 0; j < marked_files_count; j++) {
            int marked_row = marked_files[j].selected_row;
            if (marked_row == file_index &&
                marked_row >= top_row &&
                marked_row < top_row + height) {
                int index = (marked_row - top_row) * width + COLUMN_DIVIDER_POSITION + 1;        

                if (mark_status == MARKED) {
                    buffer[index].Char.AsciiChar = '*';
                } else if (mark_status == YANKED) {
                    buffer[index].Char.AsciiChar = 'Y';
                } else if (mark_status == CUT) {
                    buffer[index].Char.AsciiChar = 'X';
                }

                buffer[index].Attributes = color;
            }
        }
        // ================== Highlight marked files in the current directory ================== 
    }
    // ============================= Draw Current Directory ============================
    
    int index = (selected_row - top_row) * width + COLUMN_DIVIDER_POSITION + 2;

    // Draw the cursor
    color = white;
    buffer[index].Char.AsciiChar = '>';
    buffer[index].Attributes = color;

    COORD buffer_size = { width, height };
    COORD origin = { 0, 0 };
    SMALL_RECT region = { 0, 0, width - 1, height - 1 };
    WriteConsoleOutput(hAlt, buffer, buffer_size, origin, &region);

    COORD cursor_position = { COLUMN_DIVIDER_POSITION + 4, selected_row - top_row };
    SetConsoleCursorPosition(hAlt, cursor_position);

    free(buffer);
}

int HandleInput() {
    INPUT_RECORD input;
    DWORD events;
    ReadConsoleInput(hIn, &input, 1, &events);

    if (input.EventType != KEY_EVENT || !input.Event.KeyEvent.bKeyDown) {
        return 1; // Ignore non-key events
    }

    BOOL ctrl = input.Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED); 
    BOOL shift= input.Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED;

    if (shift) {
        if (input.Event.KeyEvent.wVirtualKeyCode == 'G') {
            ModifySelectedRow(1000);
        }
    }
    else if (ctrl) {
        switch (input.Event.KeyEvent.wVirtualKeyCode) {
            case 'D': {
                ModifySelectedRow(35); // Page Down
                break;
            }
            case 'U': {
                ModifySelectedRow(-35); // Page Up
                break;
            }
            case 'A': {
                SetMarkStatus(MARKED);
                
                for (int i = 0; i < MAX_FILES && i < current_directory_file_count; i++) {
                    if (i == 0) {
                        ClearMarkedFiles();
                        strcpy(mark_directory, current_directory);
                    }
                    char full_path[MAX_PATH];
                    GetFilePath(current_directory, &current_directory_files[i], full_path);

                    strcpy(marked_files[marked_files_count].path, full_path);
                    marked_files[i].selected_row = i;
                    marked_files_count++;
                }
                break;
            }
            case VK_OEM_4: {
                ClearMarkedFiles();
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
                if (IsDirectory(&current_directory_files[selected_row])) {
                    char selected_directory_path[MAX_PATH];
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
                    ModifySelectedRow(-1000); 
                    pending_g = false;
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
                if (IsMarkDirectorySet() && MarkDirEqualToCurrentDir()) {
                    ConfirmDelete();
                }
                else {
                    ClearMarkedFiles();
                    strcpy(mark_directory, current_directory);
                    ToggleMark();
                    ConfirmDelete();
                }
                break;
            }
            case 'O': {
                HandleOldHistory();
                break;
            }
            case VK_SPACE: {
                if (!IsMarkDirectorySet() || !MarkDirEqualToCurrentDir()) {
                    ClearMarkedFiles();
                    strcpy(mark_directory, current_directory);
                }

                ToggleMark();
                ModifySelectedRow(1);
                break;
            }
            case VK_RETURN: {
                if (!IsDirectory(&current_directory_files[selected_row])) {
                    char file_path[MAX_PATH];
                    GetSelectedRowPath(selected_row, file_path);

                    char command[MAX_PATH + 10];
                    snprintf(command, sizeof(command), "vim \"%s\"", file_path);

                    // Temporarily switch to the original buffer to run the command, then switch back
                    SetConsoleActiveScreenBuffer(hOriginal);

                    system(command);

                    // Switch back to the alternate buffer after the command is executed
                    SetConsoleActiveScreenBuffer(hAlt);
                }
                break;
            }
            case 'Q': {
                pending_g = false;
                return 0; // Exit
            }
        }
    }
    return 1;
}

void HandleMarkOperation(enum MarkStatus new_status) {
    if (IsMarkDirectorySet()) {
        if (MarkDirEqualToCurrentDir()) {
            if (marked_files_count == 1) {
                ClearMarkedFiles();
                ToggleMark();
            }
            SetMarkStatus(new_status);
        }
        else {
            ClearMarkedFiles();
            strcpy(mark_directory, current_directory);
            ToggleMark();
            SetMarkStatus(new_status);
        }
    }
    else {
        ClearMarkedFiles();
        strcpy(mark_directory, current_directory);
        ToggleMark();
        SetMarkStatus(new_status);
    }
}

void HandlePaste(int move) {
    for (int i = 0; i < marked_files_count; i++) {
        char source[MAX_PATH + 1];
        char destination[MAX_PATH + 1];

        strcpy(source, marked_files[i].path);
        source[strlen(source) + 1] = '\0';

        snprintf(destination, MAX_PATH, "%s\\%s", current_directory, strrchr(marked_files[i].path, '\\') + 1);

        destination[strlen(destination) + 1] = '\0';
        
        SHFILEOPSTRUCT op = {0};
        op.wFunc = move ? FO_MOVE : FO_COPY;
        op.pFrom = source;
        op.pTo = destination;
        op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT;

        SHFileOperation(&op);
    }

    LoadCurrentDirectory();
    SetMarkStatus(MARKED);
    marked_files_count = 0;
}

void ToggleMark() {
    for (int i = 0; i < marked_files_count; i++) {
        if (marked_files[i].selected_row == selected_row) {
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
        char full_path[MAX_PATH];
        GetFilePath(current_directory, &current_directory_files[selected_row], full_path);
        strcpy(marked_files[marked_files_count].path, full_path);
        marked_files[marked_files_count].selected_row = selected_row;
        marked_files_count++;
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

int GetFilesInDirectory(char* path, WIN32_FIND_DATA current_directory_files[]) {
    char search_path[MAX_PATH];
    snprintf(search_path, MAX_PATH, "%s\\*", path);

    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search_path, &fd);

    int count = 0;

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0) continue; 
            if (strcmp(fd.cFileName, "..") == 0) continue; 
            
            current_directory_files[count] = fd;
            count++;
        } while (FindNextFile(hFind, &fd) && count < MAX_FILES);

        FindClose(hFind);
    }

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
    // check if the current directory is already in history_count
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
    
    // Create popup buffer
    CHAR_INFO* popup_buffer = malloc(POPUP_WIDTH * POPUP_HEIGHT * sizeof(CHAR_INFO));
    DrawDeletePopup(POPUP_WIDTH, POPUP_HEIGHT, popup_buffer);
    
    // Center the popup on screen
    int start_col = (screen_width - POPUP_WIDTH) / 2;
    int start_row = (screen_height - POPUP_HEIGHT) / 2;
    
    COORD buffer_size = { POPUP_WIDTH, POPUP_HEIGHT };
    COORD origin = { 0, 0 };
    SMALL_RECT region = { start_col, start_row, start_col + POPUP_WIDTH - 1, start_row + POPUP_HEIGHT - 1 };
    WriteConsoleOutput(hAlt, popup_buffer, buffer_size, origin, &region);
    
    free(popup_buffer);
    
    // Wait for input
    while (1) {
        INPUT_RECORD input;
        DWORD events;
        ReadConsoleInput(hIn, &input, 1, &events);
        
        if (input.EventType == KEY_EVENT && input.Event.KeyEvent.bKeyDown) {
            if (input.Event.KeyEvent.wVirtualKeyCode == 'Y') {
                for (int i = 0; i < marked_files_count; i++) {
                    char source[MAX_PATH + 1];
                    strcpy(source, marked_files[i].path);
                    source[strlen(source) + 1] = '\0';
                    
                    SHFILEOPSTRUCT op = {0};
                    op.wFunc = FO_DELETE;
                    op.pFrom = source;
                    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
                    
                    SHFileOperation(&op);
                }
                marked_files_count = 0;
                LoadCurrentDirectory();
                break;
            } else if (input.Event.KeyEvent.wVirtualKeyCode == 'N' || 
                       input.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) {
                break;
            }
        }
    }

    ClearMarkedFiles();
}

void DrawDeletePopup(int width, int height, CHAR_INFO* out_buffer) {
    WORD white = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    WORD red = FOREGROUND_RED | FOREGROUND_INTENSITY;
    
    // 1. Fill background with spaces
    for (int i = 0; i < width * height; i++) {
        out_buffer[i].Char.AsciiChar = ' ';
        out_buffer[i].Attributes = white;
    }
    
    // 2. Draw border - corners
    out_buffer[0].Char.AsciiChar = BOX_TOP_LEFT;
    out_buffer[width - 1].Char.AsciiChar = BOX_TOP_RIGHT;
    int bottom = (height - 1) * width;
    out_buffer[bottom].Char.AsciiChar = BOX_BOTTOM_LEFT;
    out_buffer[bottom + width - 1].Char.AsciiChar = BOX_BOTTOM_RIGHT;
    
    // Top and bottom edges
    for (int col = 1; col < width - 1; col++) {
        out_buffer[col].Char.AsciiChar = BOX_HORIZONTAL;
        out_buffer[bottom + col].Char.AsciiChar = BOX_HORIZONTAL;
    }
    
    // Left and right edges
    for (int row = 1; row < height - 1; row++) {
        out_buffer[row * width].Char.AsciiChar = BOX_VERTICAL;
        out_buffer[row * width + width - 1].Char.AsciiChar = BOX_VERTICAL;
    }
    
    // 3. Title (row 1, centered)
    char* title = "Confirm Delete?";
    int title_col = (width - strlen(title)) / 2;
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
    WORD white = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    int height = 3;
    
    // Fill background
    for (int i = 0; i < width * height; i++) {
        out_buffer[i].Char.AsciiChar = ' ';
        out_buffer[i].Attributes = white;
    }
    
    // Top border
    out_buffer[0].Char.AsciiChar = BOX_TOP_LEFT;
    for (int col = 1; col < width - 1; col++) {
        out_buffer[col].Char.AsciiChar = BOX_HORIZONTAL;
    }
    out_buffer[width - 1].Char.AsciiChar = BOX_TOP_RIGHT;
    
    // Middle row - left edge
    out_buffer[width].Char.AsciiChar = BOX_VERTICAL;
    out_buffer[width * 2 - 1].Char.AsciiChar = BOX_VERTICAL;
    
    // Bottom border
    int bottom = 2 * width;
    out_buffer[bottom].Char.AsciiChar = BOX_BOTTOM_LEFT;
    for (int col = 1; col < width - 1; col++) {
        out_buffer[bottom + col].Char.AsciiChar = BOX_HORIZONTAL;
    }
    out_buffer[bottom + width - 1].Char.AsciiChar = BOX_BOTTOM_RIGHT;
    
    // Label and input
    WriteToBuffer(out_buffer, width, 1, 2, "Name: ", white);
    WriteToBuffer(out_buffer, width, 1, 8, input_text, white);
}

void HandleCreate() {
    char name[MAX_PATH];
    name[0] = '\0';
    int pos = 0;
    
    int popup_w = PAGE_SIZE;
    int popup_h = 3;
    
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(hAlt, &info);
    int screen_width = info.srWindow.Right - info.srWindow.Left + 1;
    int screen_height = info.srWindow.Bottom - info.srWindow.Top + 1;
    
    int start_col = (screen_width - popup_w) / 2;
    int start_row = (screen_height - popup_h) / 2;
    
    CHAR_INFO* popup_buffer = malloc(popup_w * popup_h * sizeof(CHAR_INFO));
    
    while (1) {
        // Draw popup
        DrawCreatePopup(popup_w, name, popup_buffer);
        
        COORD buffer_size = { popup_w, popup_h };
        COORD origin = { 0, 0 };
        SMALL_RECT region = { start_col, start_row, start_col + popup_w - 1, start_row + popup_h - 1 };
        WriteConsoleOutput(hAlt, popup_buffer, buffer_size, origin, &region);
        
        // Position cursor after text
        COORD cursor_pos = { start_col + 8 + pos, start_row + 1 };
        SetConsoleCursorPosition(hAlt, cursor_pos);
        
        // Get input
        INPUT_RECORD input;
        DWORD events;
        ReadConsoleInput(hIn, &input, 1, &events);
        
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
        } else if (c >= 32 && c < 127 && pos < popup_w - 12) {
            name[pos] = c;
            pos++;
            name[pos] = '\0';
        }
    }
    
    free(popup_buffer);
    
    if (name[0] == '\0') return;
    
    char full_path[MAX_PATH];
    snprintf(full_path, MAX_PATH, "%s\\%s", current_directory, name);
    
    int len = strlen(name);
    if (name[len - 1] == '\\') {
        full_path[strlen(full_path) - 1] = '\0';
        CreateDirectory(full_path, NULL);
    } else {
        HANDLE h = CreateFile(full_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
    
    LoadCurrentDirectory();
}

int CalculateDistance(char* current, char* target) {
    // Check if target is below current (current is prefix of target)
    int current_len = strlen(current);
    if (strncmp(current, target, current_len) == 0 && 
        (target[current_len] == '\\' || target[current_len] == '\0')) {
        // Count slashes after current path
        int distance = 0;
        for (int i = current_len; target[i] != '\0'; i++) {
            if (target[i] == '\\') distance++;
        }
        return distance;
    }
    
    // Otherwise, find common ancestor
    char temp[MAX_PATH];
    strcpy(temp, current);
    int steps_up = 0;
    
    while (temp[0] != '\0') {
        int temp_len = strlen(temp);
        if (strncmp(temp, target, temp_len) == 0 && 
            (target[temp_len] == '\\' || target[temp_len] == '\0')) {
            // Found common ancestor, count steps down
            int steps_down = 0;
            for (int i = temp_len; target[i] != '\0'; i++) {
                if (target[i] == '\\') steps_down++;
            }
            return steps_up + steps_down;
        }
        
        // Cut off last directory
        char* last_slash = strrchr(temp, '\\');
        if (last_slash == NULL || last_slash == temp + 2) {
            break; // Hit root or different drive
        }
        *last_slash = '\0';
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
    
    // Create popup buffer
    CHAR_INFO* popup_buffer = malloc(popup_width * popup_height * sizeof(CHAR_INFO));
    DrawOldHistoryPopup(popup_width, popup_height, distances, display_count, popup_buffer);
    
    // Center the popup on screen
    int start_col = (screen_width - popup_width) / 2;
    int start_row = (screen_height - popup_height) / 2;
    
    COORD buffer_size = { popup_width, popup_height };
    COORD origin = { 0, 0 };
    SMALL_RECT region = { start_col, start_row, start_col + popup_width - 1, start_row + popup_height - 1 };
    WriteConsoleOutput(hAlt, popup_buffer, buffer_size, origin, &region);
    
    free(popup_buffer);
    
    // Wait for input
    while (1) {
        INPUT_RECORD input;
        DWORD events;
        ReadConsoleInput(hIn, &input, 1, &events);
        
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
    WORD white = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    WORD blue = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    
    // Fill background
    for (int i = 0; i < width * height; i++) {
        out_buffer[i].Char.AsciiChar = ' ';
        out_buffer[i].Attributes = white;
    }
    
    // Draw border - corners
    out_buffer[0].Char.AsciiChar = BOX_TOP_LEFT;
    out_buffer[width - 1].Char.AsciiChar = BOX_TOP_RIGHT;
    int bottom = (height - 1) * width;
    out_buffer[bottom].Char.AsciiChar = BOX_BOTTOM_LEFT;
    out_buffer[bottom + width - 1].Char.AsciiChar = BOX_BOTTOM_RIGHT;
    
    // Top and bottom edges
    for (int col = 1; col < width - 1; col++) {
        out_buffer[col].Char.AsciiChar = BOX_HORIZONTAL;
        out_buffer[bottom + col].Char.AsciiChar = BOX_HORIZONTAL;
    }
    
    // Left and right edges
    for (int row = 1; row < height - 1; row++) {
        out_buffer[row * width].Char.AsciiChar = BOX_VERTICAL;
        out_buffer[row * width + width - 1].Char.AsciiChar = BOX_VERTICAL;
    }
    
    // Title
    char* title = "Recent Directories";
    int title_col = (width - strlen(title)) / 2;
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
    char temp_path[MAX_PATH];
    snprintf(temp_path, MAX_PATH, "%s\\browser_lastdir.txt", getenv("TEMP"));
    
    FILE* f = fopen(temp_path, "w");
    if (f) {
        fprintf(f, "%s", current_directory);
        fclose(f);
    }

    SetConsoleActiveScreenBuffer(hOriginal);
    CloseHandle(hAlt);
}

void WriteToBuffer(CHAR_INFO* buffer, int width, int row, int col, const char* text, WORD color) {
    int len = strlen(text);
    for (int i = 0; i < len && col + i < width - 1; i++) {
        int index = row * width + col + i;
        buffer[index].Char.AsciiChar = text[i];
        buffer[index].Attributes = color;
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

void GetSelectedRowPath(int selected_row, char* out_path) {
    snprintf(out_path, MAX_PATH, "%s\\%s", current_directory, current_directory_files[selected_row].cFileName);
}

void LoadParentDirectory() {
    GetParentDirectory(current_directory, parent_directory);
    parent_directory_file_count = GetFilesInDirectory(parent_directory, parent_directory_files);
}

void GetFilePath(char* current_directory, WIN32_FIND_DATA* file, char* out_path) {
    snprintf(out_path, MAX_PATH, "%s\\%s", current_directory, file->cFileName);
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

void LoadCurrentDirectory() { 
    current_directory_file_count = GetFilesInDirectory(current_directory, current_directory_files);
    selected_row = 0;
    top_row = 0;
}

void SetMarkStatus(enum MarkStatus status) {
    mark_status = status;
}

bool MarkedFilesFull() {
    return marked_files_count >= MAX_FILES;
}


