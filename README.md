# drift

A vim-style terminal file browser for Windows, in a single 200 KB binary with no
runtime dependencies — plus a workspace manager for Claude Code built on top of
it.

```
C:\Users\spencer\source\repos\drift\src                                   1/2
─────────────────────────┬────────────────────────┬───────────────────────────
  .git                   │ ███drift.c█████████████│ // A simple terminal file
  build                  │    settings_json.h     │ // browser for Windows
  dist                   │                        │ // Controls:
  quality                │                        │ // - Q        : Quit
  scripts                │                        │ // - H        : Parent dir
  src                    │                        │ // - L        : Enter dir
  tests                  │                        │ // - J        : Down
                         │                        │ // - K        : Up
```

Parent directory on the left, current in the middle, and a third pane that
previews whatever the cursor is on — the head of a text file, the contents of a
directory, or a size note for binaries. Windows narrower than 80 columns drop to
two panes.

---

## Install

1. Grab `drift.exe` from [Releases](../../releases), or build it (below — it
   lands in `build\`)
2. Put `drift.exe` and `dist\drift.bat` somewhere on your `PATH`
3. Run `drift`

Run it via the `dist\drift.bat` wrapper rather than the exe directly and your
shell follows drift to wherever you quit:

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
| `v` | open in VS Code, or a solution in Visual Studio |
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

### Opening in an editor — `v`

`Enter` opens one file. `v` opens the whole project:

```
┌──────────────────────────────────────┐
│ Open ConsoleApp1 in:                 │
│                                      │
│  ███VS Code██████████folder██████████│
│    Visual Studio     ConsoleApp1.sln │
│                                      │
│  j/k move   Enter open   Esc cancel  │
└──────────────────────────────────────┘
```

`j`/`k` or the arrow keys move, `Enter` opens, `Esc` backs out — the same as
everywhere else in drift. The number keys still work as accelerators.

The subject is the directory under the cursor, or the one you're browsing if the
cursor is on a file. The menu adapts to what's actually there: with a single
solution it's named outright, with several the second row opens a picker, and
with none Visual Studio isn't offered at all. A cursor resting on a `.sln` names
that one directly. Solutions are looked for in that one directory — if it lives a
level down, step in and press `v` again.

Both editors are GUI programs, so drift spawns them and keeps running rather than
suspending itself the way `Enter` and the Claude session verbs do.

**Visual Studio** is reached by whichever of these answers first:

1. `VSLauncher.exe`, the Visual Studio Version Selector, from
   `%CommonProgramFiles(x86)%\Microsoft Shared\MSEnv\`. It reads the solution's
   own version header, so on a machine with several Visual Studios each solution
   opens in the one it was written for.
2. `devenv.exe`, if `...\Common7\IDE` is on your `PATH`. One fixed install for
   every solution, which is why it's the fallback.
3. The `.sln` file association — whatever a double-click would do.

**VS Code** is resolved as `code.cmd` or `code.exe` from `PATH`; its installer
puts `bin\` there. Neither editor is required — if one is missing, `v` says so
instead of failing silently.

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
v    open in VS Code     │                        │
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

`h` at the anchor root — or `Esc`/`c` — returns to the workspace list. `q` quits
drift outright and leaves your shell *in* the anchor — or in whichever directory
under it you'd browsed to — which is the one place claude mode changes where you
land. Everywhere else in claude mode, quitting returns you to whatever directory
you were browsing when you pressed `c`: workspaces are for building and editing,
navigation is drift's job. The anchor
is the exception because its directory is named for a timestamp — nothing else
leads there, and typing the path is not realistic.

### Opening the whole workspace — `v`

The folder set is already curated; `v` hands it to VS Code as one multi-root
window. drift regenerates a `<name>.code-workspace` in the anchor and opens
that:

```jsonc
{
  // Generated by drift from .claude\settings.json.
  // Edits here are overwritten. Put your own settings in a
  // member folder's .vscode\settings.json instead.
  "folders": [
    { "path": ".", "name": "rusty" },
    { "path": "C:\\Users\\spencer\\.config" },
    { "path": "C:\\Users\\spencer\\.dotnet" }
  ]
}
```

The anchor leads the list, so the workspace's own `CLAUDE.md` is in the tree
alongside the code. Because `"."` resolves against the workspace file's own
directory, that file has to live at the anchor root — which is also why it shows
up beside `CLAUDE.md` rather than tucked inside `.claude\`.

It's a real file rather than a one-off invocation because VS Code has no flag
that opens N folders as a single window: `--add` targets "the last active
window", so a fileless route would mean `code -n <first>` followed by
`code -a <rest>`, racing whichever window happened to be focused in between. The
file is one invocation with no race — and it lands in VS Code's Recent list, so
you can reopen the workspace without going through drift at all.

drift owns that file and rewrites it on every `v`, which is the opposite of how
it treats `settings.json` — that one is Claude's, so drift splices it and
refuses rather than risk being lossy. This one is drift's own output, and says so
in its header.

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
        │   ├── .drift-members.lock  drift's cross-instance membership lock
        │   └── settings.json        its folders, in permissions.additionalDirectories
        ├── rusty.code-workspace     generated by `v`; drift rewrites it
        └── CLAUDE.md                yours
```

`settings.json` is Claude Code's own file, read natively — drift splices only
`permissions.additionalDirectories`, so anything else you put in it survives.
If the JSON or that exact path is malformed, wrongly typed, or ambiguous, drift
refuses the membership edit and leaves the file alone. Session transcripts stay
where Claude puts them, in `%USERPROFILE%\.claude\projects`.

Concurrent drift processes serialize folder changes through
`.drift-members.lock`, then re-read and apply the requested add or removal to
the latest list. The file remains in place normally; lock ownership is the live
Windows file handle, which is released when the operation or process ends.

### Environment variables

| | |
|-|-|
| `DRIFT_HOME` | overrides `%USERPROFILE%` for `~` and for `.drift\` |
| `DRIFT_CLAUDE_DIR` | where to find `.claude\projects` |
| `DRIFT_HOST_DRIVE` | write workspace folders host-style — see `scripts/run.sh` |
| `DRIFT_LAUNCH_FILE` | hand Claude launches to a wrapper instead of spawning them |

The last two exist for `scripts/run.sh`, which builds and runs drift under Wine
on macOS so it can be developed off-Windows.

---

## Build

**Layout.**

```
src/       drift.c and settings_json.h — one translation unit
tests/     regression tests; each includes ../src/drift.c whole
scripts/   build.bat, run_tests.bat, run_tests.sh, run.sh
dist/      drift.bat and refresh_prompt.bat — the shipped shell wrappers
quality/   issue tracker, audits, and validate.ps1
build/     compiler output (gitignored)
```

Every script in `scripts/` switches to the repository root first, so it can be
invoked from anywhere.

**Windows.** Run `scripts\build.bat` — it finds Visual Studio itself and tells
you what to install if the C++ tools are missing. It writes `build\drift.exe`.
From a Developer Command Prompt:

```batch
cl /W3 /O2 src\drift.c /Fe:build\drift.exe shell32.lib
```

**Cross-compile.**

```bash
x86_64-w64-mingw32-gcc src/drift.c -o build/drift.exe -lshell32
```

**Tests.** `scripts/run_tests.sh` runs a static lint for the frame buffer's
row-guard invariant, a regression suite under AddressSanitizer, and a
full-warning cross-compile. It's host-native — no Wine needed. On Windows,
`scripts\run_tests.bat` runs the full ten-stage MSVC suite.

---

## Known limitations

- Filenames outside the system ANSI codepage list as `?` placeholders, and file
  operations on them are refused rather than risking a mangled name acting as a
  wildcard. Fixing this properly means moving to `FindFirstFileW`.
- UNC paths aren't supported.
- Directories are listed up to 4096 entries.

## License

MIT
