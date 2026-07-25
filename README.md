# drift

A fast, vim-style terminal file browser for Windows.

## Features

- **Miller columns** - Parent directory on the left, current directory in the middle
- **Preview pane** - Right column previews text files, directory contents, and binary sizes (windows 80+ columns wide)
- **Vim-style navigation** - hjkl movement, gg/G for top/bottom
- **File operations** - Copy, move, delete with recycle bin support
- **Batch marking** - Mark multiple files for bulk operations
- **Directory history** - Jump to recently visited directories
- **Create files/directories** - Quick creation with `a`
- **Single binary** - No dependencies, just `drift.exe`

## Controls

### Navigation
| Key | Action |
|-----|--------|
| `h` | Go to parent directory |
| `l` | Enter selected directory |
| `j` | Move down |
| `k` | Move up |
| `gg` | Jump to top |
| `G` | Jump to bottom |
| `Ctrl+d` | Page down |
| `Ctrl+u` | Page up |
| `` ` `` or `~` | Jump to home directory |
| `o` | Show directory history |

### File Operations
| Key | Action |
|-----|--------|
| `Space` | Toggle mark on file |
| `Ctrl+a` | Mark all files |
| `Ctrl+[` | Clear all marks |
| `y` | Yank (copy) marked files |
| `x` | Cut marked files |
| `p` | Paste files |
| `d` | Delete marked files (to recycle bin) |

### Other
| Key | Action |
|-----|--------|
| `a` | Create new file (or directory if name ends with `\`) |
| `.` | Toggle hidden files (hidden by default) |
| `c` | Claude workspace browser (`l` opens a workspace's sessions, `h` backs out) |
| `e` | (workspace list) edit the workspace's folders: `Space` adds/removes, `Tab`/`l` focus the list, `Esc`/`c` done |
| `f` | (workspace list) browse the workspace's own files — its `CLAUDE.md` and notes, which apply across every folder in the workspace |
| `W` | (browsing) add directory under cursor to a workspace |
| `Enter` | (session list) resume the session in claude |
| `n` | new claude session in the workspace |
| `r` | (session list) rename session (drift-side title) |
| `d` | (session list) delete session (recycle bin) |

Run `drift -c` to open straight into the claude workspace browser.
| `Enter` | Open file in vim |
| `q` | Quit |

## Build

### Windows (MSVC)
Run `build.bat` — it locates Visual Studio automatically (and tells you what
to install if the C++ tools are missing). Or, from a Developer Command Prompt:
```batch
cl drift.c /Fe:drift.exe shell32.lib
```

### Linux (cross-compile)
```bash
x86_64-w64-mingw32-gcc drift.c -o drift.exe -lshell32
```

## Installation

1. Download `drift.exe` from [Releases](../../releases) or build from source
2. Place `drift.exe` and `drift.bat` somewhere in your PATH
3. Run `drift` from cmd

### Stay in directory on exit

Use the included `drift.bat` wrapper to change your shell's directory when you quit:
```batch
@echo off
drift.exe
if exist "%TEMP%\browser_lastdir.txt" (
    for /f "usebackq delims=" %%i in ("%TEMP%\browser_lastdir.txt") do cd /d "%%i"
)
```

## Requirements

- Windows 10 or later
- vim in PATH (optional, for opening files with Enter)

## License

MIT
