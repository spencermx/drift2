# DRIFT-002 — Vim lookup can execute a planted `vim.exe`

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Investigating`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** High
**Final severity:** High
**Primary locations:** `drift.c:393` (`Initialize`), `drift.c:3157`
(`OpenFileInEditor`)
**Implemented by:** —
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

## Trigger and impact

Drift is launched with its process current directory set to a directory that
contains an attacker-controlled `vim.exe`. The normal configured search path
does not contain an earlier `vim.exe`, which is common when Vim is not installed
and Drift should fall back to the file's default application. When the user
opens a file, `SearchPath(NULL, "vim.exe", ...)` eventually selects the planted
file and `CreateProcess` executes its returned absolute path.

This is arbitrary native-code execution triggered by an ordinary file-open
action. A legitimate Vim earlier in the safe search order masks the planted
file, but its presence is not a security boundary and can change between
machines or environments.

## Confirmed control flow

1. `Initialize` calls `SetSearchPathMode` with
   `BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT`.
2. Drift does not change the Win32 process current directory while browsing;
   its `ChangeCurrentDirectory` function updates application state only.
3. `OpenFileInEditor` calls `SearchPath` with a null `lpPath`, so Windows uses
   the process search mode.
4. Safe mode searches configured/system path locations before the current
   working directory, but still searches the current directory if no earlier
   match exists.
5. Drift passes the returned absolute path as `CreateProcess`'s non-null
   application name, so that planted path is executed without another search.

The comment at `drift.c:394-396` incorrectly says safe mode keeps the current
directory out of the executable search path. It only changes its position.

## Reproduction

A temporary native Win32 probe used the same calls and flags as Drift:

1. Create an otherwise empty anchor containing a sentinel `vim.exe`.
2. Set the process current directory to that anchor.
3. Set `PATH` to a separate empty absolute directory.
4. Remove `NoDefaultCurrentDirectoryInExePath`, because agent shells can inherit
   it and otherwise produce a false negative.
5. Permanently enable safe process search mode.
6. Call `SearchPath(NULL, "vim.exe", NULL, ...)`.

Observed result:

```text
need_cwd=1
safe_mode=1
length=100
resolved=<temporary-anchor>\vim.exe
```

The temporary probe and fixture were removed after the run. The development
machine normally finds `C:\Program Files\Git\usr\bin\vim.exe`; that legitimate
earlier match explains why the planted-file condition is environment-dependent
but does not remove it.

## Authoritative platform behavior

- Microsoft documents that when safe process search mode is enabled,
  `SearchPath` searches the system/configured path before the current working
  folder—not that it excludes the current folder:
  <https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-searchpatha>
- `SetSearchPathMode` only selects that per-process ordering, and the permanent
  flag prevents later changes:
  <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setsearchpathmode>
- `CreateProcess` accepts a full executable path through `lpApplicationName`,
  which avoids executable search at process-creation time:
  <https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessa>

## Severity assessment

High remains appropriate under the workflow rubric because the result is
credible arbitrary code execution from an attacker-controlled launch directory.
The requirement that no legitimate Vim be found earlier reduces exploitability
but not impact. Falling back to the default application is already Drift's
intended behavior when Vim is unavailable, so “Vim is not installed” must not
turn an untrusted current directory into an executable source.

## Options considered

1. **Keep safe search mode.** Rejected: confirmed vulnerable by documentation
   and native reproduction.
2. **Set `NoDefaultCurrentDirectoryInExePath` globally.** It can suppress the
   current directory, but it would be inherited by Vim, Claude, and their child
   processes, changing legitimate project-local command behavior. It also
   relies on mutable process environment state rather than making this lookup
   explicit.
3. **Build a sanitized absolute `PATH` string and pass it as `SearchPath`'s
   non-null `lpPath`.** Potentially safe, but retains an unnecessary second
   search API and duplicates parsing/filtering decisions already introduced for
   DRIFT-001.
4. **Generalize DRIFT-001's explicit absolute-`PATH` resolver and use it for
   Vim.** Recommended. A shared resolver can accept an ordered set of allowed
   filenames, walk only fully qualified entries, preserve directory order, and
   return a typed absolute match. Claude continues checking `claude.exe` then
   `claude.cmd` within each directory; Vim checks only `vim.exe`.

## Recommended implementation

Refactor the already-tested DRIFT-001 path tokenizer and directory probe into a
small generic internal resolver, preserving Claude's behavior byte-for-byte at
its public wrapper. Add a Vim wrapper that resolves only `vim.exe` from
explicit absolute `PATH` entries. Replace the null-path `SearchPath` call in
`OpenFileInEditor` with that wrapper and remove the now-unused process-wide
`SetSearchPathMode` call from `Initialize`.

Continue passing the resolved absolute Vim path as `lpApplicationName`; keep the
currently displayed directory as Vim's child working directory. If no safe Vim
is found, preserve the existing default-application fallback rather than trying
any relative or current-directory lookup.

This changes shared resolver internals used by the already verified DRIFT-001,
so its complete launcher test suite must remain green and the DRIFT-002 reviewer
must inspect that compatibility explicitly. It does not reopen DRIFT-001 unless
Claude launch behavior actually changes.

## Proposed acceptance criteria and regression coverage

- A planted current-directory `vim.exe` is never selected when no legitimate
  Vim exists on the absolute search path.
- Empty, `.`, drive-relative, root-relative, and named relative `PATH` entries
  are ignored.
- Fully qualified `PATH` directory order is preserved.
- Quoted absolute directories containing spaces and semicolons resolve.
- A directory named `vim.exe` is not accepted as an executable file.
- A safe absolute Vim path is passed as non-null `lpApplicationName`; the file
  path remains a quoted argument and the displayed directory remains the child
  working directory.
- No safe Vim match takes the existing default-application fallback.
- The process-wide `SetSearchPathMode` call and its inaccurate comment are
  removed once no production code uses `SearchPath` for executable discovery.
- All DRIFT-001 `.exe` and `.cmd` ordering, quoting, metacharacter, session, and
  real-child-process tests continue to pass unchanged.
- The full regression suite, optimized build, and warning-clean compilation
  pass.

## Non-goals and residual risk

- Editor configurability is outside this item; Drift continues preferring an
  executable specifically named `vim.exe`.
- Absolute directories explicitly configured on `PATH` remain trusted. A
  malicious or writable trusted directory is an installation/environment
  problem outside this current-directory vulnerability.
- Existing `MAX_PATH` and ANSI-path limitations remain.
- The fallback `ShellExecute` working-directory issue is already tracked
  separately as DRIFT-012.

## User disposition needed

The diagnosis and recommended design are ready for user disposition. No
production or test code has been changed for DRIFT-002.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible executable-search hijack. |
| 2026-07-25 | Codex | `Untriaged` | `Investigating` | Confirmed safe mode still selects a planted current-directory `vim.exe` when no earlier match exists; documented options and recommended shared absolute-path resolution. |
