# drift

A vim-style terminal file browser for Windows, in a single 200 KB binary with no
runtime dependencies — plus a workspace manager for Claude Code built on top of
it.

```
C:\Users\spencer\source\repos\drift                                      5/8
─────────────────────────┬────────────────────────┬───────────────────────────
  .claude                │    .git                │ // A simple terminal file
  .config                │    tests               │ // browser for Windows
  claude1                │    .gitignore          │ // Controls:
  drift                  │    README.md           │ // - Q        : Quit
  notes                  │ ███drift.c█████████████│ // - H        : Parent dir
                         │    drift.exe           │ // - L        : Enter dir
                         │    build.bat           │ // - J        : Down
                         │    run.sh              │ // - K        : Up
```

Parent directory on the left, current in the middle, and a third pane that
previews whatever the cursor is on — the head of a text file, the contents of a
directory, or a size note for binaries. Windows narrower than 80 columns drop to
two panes.

---

## Install

1. Grab `drift.exe` from [Releases](../../releases), or build it (below)
2. Put `drift.exe` and `drift.bat` somewhere on your `PATH`
3. Run `drift`

Run it via the `drift.bat` wrapper rather than the exe directly and your shell
follows drift to wherever you quit:

```batch
@echo off
drift.exe
if exist "%TEMP%\browser_lastdir.txt" (
    for /f "usebackq delims=" %%i in ("%TEMP%\browser_lastdir.txt") do cd /d "%%i"
)
```

**Requirements:** Windows 10 or later. `vim` on `PATH` is optional — `Enter`
falls back to the file's default application. `claude` on `PATH` is optional too,
and only needed for the workspace features.

---

## Browsing

| Key | |
|-----|--|
| `h` `j` `k` `l` | parent / down / up / enter |
| `gg` `G` | top / bottom |
| `Ctrl+d` `Ctrl+u` | half page down / up |
| `` ` `` or `~` | jump to home |
| `o` | recently visited directories — pick one with `1`-`9`/`0` |
| `Enter` | open in vim, or the default application |
| `a` | create a file — or a directory, if the name ends with `\` |
| `.` | show hidden files (off by default) |
| `q` | quit |

### Marking and file operations

Marks are the unit of work: mark a set, then act on it. They're tracked by path,
so they survive re-sorting and directory reloads, and the header shows the count.

| Key | |
|-----|--|
| `Space` | mark / unmark, and step down |
| `Ctrl+a` | mark everything here |
| `Ctrl+[` or `Esc` | clear marks |
| `y` | yank (copy) |
| `x` | cut |
| `p` | paste into the current directory |
| `d` | delete, with a confirmation popup |

Copies, moves and deletes go through the Windows shell as a single batched
operation — so deletes land in the Recycle Bin as one undo unit, collisions
become "Copy of…" instead of silent overwrites, and long operations get the
shell's own progress dialog with a cancel button.

---

## Claude workspaces

Claude Code anchors a session to one working directory. That's a poor fit when
the thing you're actually working on is a repo *plus* a scratch folder *plus*
some config directory three levels into your home dir.

A **workspace** is a named set of folders scattered anywhere on the machine.
Sessions belong to the workspace, not to a repo. Press `c`:

```
Claude Mode                Workspaces              Sessions           2/3
─────────────────────────┬────────────────────────┬────────────────────────
A workspace is a set     │    drift               │ 3 sessions
of folders Claude        │ ███rusty███████████████│
opens together.          │    scratch             │ 2h   fix the parser
                         │                        │ 1d   add tests
l    open sessions       │                        │ 3d   main
n    new session         │                        │
e    edit workspace      │                        │
f    workspace files     │                        │
a    new workspace       │                        │
r    rename workspace    │                        │
y/p  duplicate           │                        │
d    delete workspace    │                        │
c    back to files       │                        │
```

`l` opens a workspace's sessions; `h` or `c` backs out. `drift -c` boots
straight into this view.

### Choosing the folders — `e`

Editing a workspace drops you back into the ordinary file browser, wherever you
were before you pressed `c`, with the workspace's folder list pinned in the third
pane. Browse anywhere and press `Space` to add or remove the directory under the
cursor; members are marked with a `+` as you pass them.

| Key | |
|-----|--|
| `Space` | add / remove the directory under the cursor |
| `Tab` or `l` | focus the folder list |
| `j` `k` | move within it |
| `Space` or `x` | remove the selected folder |
| `Enter` | jump the browser to that folder |
| `Esc` or `c` | done |

`Shift+W` does the same thing in one keystroke from anywhere in the browser: add
the directory under the cursor to a workspace picked from a popup.

### Workspace notes — `f`

Every workspace owns a directory of its own, and `f` opens it as a normal browse.
Claude runs with that directory as its working directory, so a `CLAUDE.md` there
is the workspace's project memory — it reaches every folder in the workspace at
once, while each member folder can still carry its own.

New workspaces get an empty `CLAUDE.md` to fill in. drift creates it once and
never writes to it again. Anything else you drop in there is yours.

### Sessions

```
Claude Mode                Sessions                                 1/3
─────────────────────────┬────────────────────────┬────────────────────────
  drift                  │ ██2h   fix the parser██│ Enter  resume session
  rusty                  │   1d   add tests       │ n      new session
  scratch                │   3d   main            │ r      rename session
                         │                        │ d      delete session
                         │                        │
                         │                        │ rusty
                         │                        │ 3 sessions
                         │                        │
                         │                        │ last active: 2h
                         │                        │ id: 6c12b33d-be75-426c
                         │                        │
                         │                        │ fix the parser
```

Newest first, each labelled with its age and its first prompt. `Enter` resumes
one in place — drift suspends itself, hands the terminal over, and comes back
when Claude exits. `n` starts a fresh one. `r` renames a session and `d` deletes
its transcript to the Recycle Bin.

Renaming is drift-side only: it never modifies Claude's own transcripts, and
never renames a workspace's directory either. That directory's path is the key
Claude files sessions under, so renaming it would orphan every session in the
workspace. Names live beside it instead.

---

## Where things live

```
%USERPROFILE%\.drift\
├── workspace-names                  folder id <TAB> name
├── session-names                    folder id <TAB> session id <TAB> name
└── workspaces\
    └── 2026-07-25_07-02-00\         a workspace — the folder is an opaque id
        ├── .claude\
        │   └── settings.json        its folders, in permissions.additionalDirectories
        └── CLAUDE.md                yours
```

`settings.json` is Claude Code's own file, read natively — drift splices only
`permissions.additionalDirectories`, so anything else you put in it survives.
If the JSON or that exact path is malformed, wrongly typed, or ambiguous, drift
refuses the membership edit and leaves the file alone. Session transcripts stay
where Claude puts them, in `%USERPROFILE%\.claude\projects`.

### Environment variables

| | |
|-|-|
| `DRIFT_HOME` | overrides `%USERPROFILE%` for `~` and for `.drift\` |
| `DRIFT_CLAUDE_DIR` | where to find `.claude\projects` |
| `DRIFT_HOST_DRIVE` | write workspace folders host-style — see `run.sh` |
| `DRIFT_LAUNCH_FILE` | hand Claude launches to a wrapper instead of spawning them |

The last two exist for `run.sh`, which builds and runs drift under Wine on macOS
so it can be developed off-Windows.

---

## Build

**Windows.** Run `build.bat` — it finds Visual Studio itself and tells you what
to install if the C++ tools are missing. From a Developer Command Prompt:

```batch
cl /W3 /O2 drift.c /Fe:drift.exe shell32.lib
```

**Cross-compile.**

```bash
x86_64-w64-mingw32-gcc drift.c -o drift.exe -lshell32
```

**Tests.** `tests/run_tests.sh` runs a static lint for the frame buffer's
row-guard invariant, a regression suite under AddressSanitizer, and a
full-warning cross-compile. It's host-native — no Wine needed.

---

## Known limitations

- Filenames outside the system ANSI codepage list as `?` placeholders, and file
  operations on them are refused rather than risking a mangled name acting as a
  wildcard. Fixing this properly means moving to `FindFirstFileW`.
- UNC paths aren't supported.
- Directories are listed up to 4096 entries.

## License

MIT
