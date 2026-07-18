# drift

A fast, vim-style terminal file browser for Windows.

## Features

- **Miller columns** - Parent directory on the left, current directory on the right
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
