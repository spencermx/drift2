# DRIFT-002 — Vim lookup can execute a planted `vim.exe`

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Verified`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** High
**Final severity:** High
**Primary locations:** `drift.c:ResolveAllowedFileFromPath`,
`drift.c:ResolveVim`, `drift.c:OpenFileInEditor`,
`tests/vim_resolver_test.c`
**Implemented by:** Codex
**Reviewed by:** Claude: approved
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

## Agreed design

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

The user approved this design before production or test edits began.

## Implementation

- `FindAllowedFileInPathEntry`, `ResolveAllowedFileFromPath`, and
  `ResolveAllowedFile` now hold the shared absolute-entry validation, quoted
  semicolon-aware tokenization, ordered filename probing, file-type rejection,
  and dynamically sized environment read.
- The Claude wrappers still request `claude.exe` followed by `claude.cmd` in
  each directory and map that ordered result back to the existing launcher
  kind. No Claude process-spec or process-launch behavior changed.
- `ResolveVimFromPath` and `ResolveVim` allow only `vim.exe`. `OpenFileInEditor`
  uses the returned absolute path as `CreateProcess`'s non-null
  `lpApplicationName`.
- The Vim command line still contains the quoted selected file, the displayed
  directory is still the child working directory, and resolution or launch
  failure still uses the existing `ShellExecute` fallback.
- The obsolete process-wide `SetSearchPathMode` call was removed. No production
  `SearchPath` call remains.
- `tests/vim_resolver_test.c` exercises the production translation unit and
  adds source-wiring guards so restoring the unsafe lookup or disconnecting the
  safe resolver from `CreateProcess` fails the Windows suite.

## Security, compatibility, and error paths

The security boundary is now explicit: only an existing non-directory
`vim.exe` under a fully qualified `PATH` entry can be selected. Empty, relative,
drive-relative, and root-relative entries cannot be expanded through Drift's
process current directory. `CreateProcess` receives the resolved absolute path,
so it performs no second executable search.

Claude compatibility is material because the implementation shares its
resolver internals with DRIFT-001. The unchanged 17-case Claude suite verifies
directory order, `.exe`/`.cmd` precedence, quoted and metacharacter-bearing
paths, session validation, command construction, and real child processes.

If safe resolution finds nothing, or if the selected Vim cannot be launched,
Drift follows the pre-existing default-application fallback. This fix does not
alter the selected file argument, console-buffer transitions, wait behavior,
child working directory, or post-editor directory reload.

## Acceptance criteria and implementer evidence

| Criterion | Result | Evidence |
|---|---|---|
| A planted current-directory `vim.exe` is not selected when omitted from the absolute search path. | Pass | Dedicated missing, empty, and invalid-`PATH` fixture cases in `tests/vim_resolver_test.c:118-126`. |
| Empty, `.`, named relative, drive-relative, and root-relative entries are ignored. | Pass | Combined invalid-entry test plants reachable files under the test cwd and resolves none. |
| Fully qualified directory order is preserved. | Pass | Both orderings are exercised at `tests/vim_resolver_test.c:128-141`. |
| Quoted absolute directories containing spaces and semicolons resolve. | Pass | Quoted fixture at `tests/vim_resolver_test.c:143-147`. |
| A directory named `vim.exe` is rejected. | Pass | Directory-only fixture at `tests/vim_resolver_test.c:149-150`. |
| The safe absolute path is passed directly to `CreateProcess`; file quoting and displayed-directory cwd remain. | Pass | Production wiring at `drift.c:3203-3225`; source regression guard at `tests/vim_resolver_test.c:171-173`. |
| No safe Vim match retains the default-application fallback. | Pass | Production branch at `drift.c:3224-3231`; source regression guard at `tests/vim_resolver_test.c:174-176`. |
| Obsolete implicit-search state is absent. | Pass | No production `SearchPath` or `SetSearchPathMode` call; asserted at `tests/vim_resolver_test.c:168-170`. |
| DRIFT-001 behavior remains compatible. | Pass | All 17 unchanged Claude launcher tests pass under AddressSanitizer. |
| Full regression, optimized build, and warning checks pass. | Pass | Commands and results below. |

The new suite would detect the pre-fix implementation: the production Vim
wrapper did not exist, and its source guard rejects both `SearchPath` and
`SetSearchPathMode` while requiring the safe resolver-to-`CreateProcess`
wiring.

## Validation performed

- `cmd.exe /d /c tests\run_tests.bat` — `ALL CHECKS PASSED`, all five stages:
  source lint, general AddressSanitizer regressions, 17 Claude launcher cases,
  13 Vim resolver/wiring cases, and `/W4 /WX` production compilation.
- `cmd.exe /d /c build.bat` — optimized `drift.exe` build succeeded.
- MSVC `cl /nologo /W4 /wd4459 /analyze /c drift.c` — completed successfully;
  only the established 32,900-byte stack-frame warning and two established
  shadowing warnings were reported.
- `git diff --check` — passed.
- Production-source search — no `SearchPath(` or `SetSearchPathMode(` remains.

Safe optional manual validation for the independent reviewer:

1. Put a harmless sentinel executable named `vim.exe` in Drift's process
   current directory, omit that directory and every real Vim from `PATH`, and
   open a file. The sentinel must not run; Drift should use the default app.
2. Put a real or harmless sentinel `vim.exe` in an explicit absolute `PATH`
   directory and open a file. Confirm that executable receives the selected
   file and the directory shown by Drift as its working directory.
3. Re-run the Windows test suite and inspect the unchanged Claude cases because
   their resolver internals are shared.

## Non-goals and residual risk

- Editor configurability is outside this item; Drift continues preferring an
  executable specifically named `vim.exe`.
- Absolute directories explicitly configured on `PATH` remain trusted. A
  malicious or writable trusted directory is an installation/environment
  problem outside this current-directory vulnerability.
- Existing `MAX_PATH` and ANSI-path limitations remain.
- The fallback `ShellExecute` working-directory issue is already tracked
  separately as DRIFT-012.

## User disposition

The user approved the recommended shared-resolver design. Codex implemented and
validated it as an isolated, attributed fix. Claude then independently approved
the complete commit set with no requested changes, so this item is `Verified`.
The implementer did not approve its own work.

## Independent review handoff

Locate the complete commit set by audit trailer rather than relying on a hash
copied from conversation history:

```text
git log --all --reverse --format="%H %s" --grep="Audit-ID: DRIFT-002"
git show --stat --patch <commit>
```

Read `quality/README.md`, inspect every returned commit and the surrounding
resolver/editor code, run `cmd /c tests\run_tests.bat`, and evaluate each
acceptance criterion above. Pay particular attention to the shared-resolver
refactor's Claude compatibility, absolute-path validation, quote/semicolon
tokenization, direct non-null `lpApplicationName`, fallback preservation, and
the scope of the isolated commit. Report `Approved`, `Approved with residual
risk`, or `Changes requested` using the workflow's required review format.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible executable-search hijack. |
| 2026-07-25 | Codex | `Untriaged` | `Investigating` | Confirmed safe mode still selects a planted current-directory `vim.exe` when no earlier match exists; documented options and recommended shared absolute-path resolution. |
| 2026-07-25 | User | `Investigating` | `Fix planned` | Approved the recommended shared absolute-`PATH` resolver design. |
| 2026-07-25 | Codex | `Fix planned` | `Fixing` | Began the isolated implementation and regression coverage. |
| 2026-07-25 | Codex | `Fixing` | `Awaiting review` | Implemented absolute Vim resolution, preserved Claude and fallback behavior, passed focused and full validation, and prepared the independent-review handoff. |
| 2026-07-25 | Claude | `Awaiting review` | `Verified` | Independent review approved the complete commit set with no code findings; recorded one records-level follow-up against DRIFT-001's audit trail. |

## Review history

### Round 1 — Claude, 2026-07-25

- **Reviewer:** `Claude` (Opus 5). Absent from `Implemented by`, so eligible.
- **Commit set:** `8501bfb` "Investigate DRIFT-002: confirm Vim search hijack"
  (documentation only) and `e1070d2` "Fix DRIFT-002: resolve Vim only from
  absolute PATH entries". Both carry the `Audit-ID: DRIFT-002` trailer; no
  others exist.
- **Verdict:** `Approved`.

**Acceptance criteria:** all ten pass, each confirmed independently rather than
accepted from the implementer evidence column. The tenth was re-run directly.

**Tests run by the reviewer:**

- `cmd /c tests\run_tests.bat` — ALL CHECKS PASSED, five stages: source lint,
  general AddressSanitizer regressions, 17/17 Claude launcher cases, 13/13 Vim
  resolver and wiring cases, and the `/W4 /WX` production compile.
- `.\build.bat` — optimized `drift.exe` built successfully. The working tree
  remained clean afterward; `drift.exe` is untracked.
- Negative control on the new source guards: applied all five to
  `git show a9e75bd:drift.c`. Three fail against the pre-fix source —
  `SearchPath(` present, `SetSearchPathMode(` present, `ResolveVim(vim_exe)`
  absent — so the suite does detect a reversion, as the implementer claimed.
  `CreateProcess(vim_exe, command` and the `ShellExecute` fallback were already
  present before the fix, so those two guards do not discriminate on their own.

**Claude compatibility**, which the agreed design required the reviewer to
inspect explicitly, was verified three ways: `tests/claude_launcher_test.c` is
untouched by `e1070d2`; all 17 cases pass unchanged; and tracing the refactor
shows the tokenizer body is identical, `.exe`-before-`.cmd` ordering is preserved
through the `names[]` index-0/1 to `EXE`/`CMD` mapping, and the wrappers restore
the exact pre-refactor initialization of `out->path` and `out->kind`. DRIFT-001
therefore remains correctly `Verified`.

**Findings — no code defects. Three records-level items:**

1. *DRIFT-001's audit trail no longer reaches the commit that rewrote its code.*
   `e1070d2` restructured all three DRIFT-001 resolver functions but carries only
   `Audit-ID: DRIFT-002`. `git log --grep="Audit-ID: DRIFT-001"` returns only
   `a9e75bd` and `2be62cf`, while `git log -L 1163,1200:drift.c` shows `e1070d2`
   as the last commit to touch those lines, so a reviewer following section 5.4
   would miss the refactor. This file discloses the coupling; DRIFT-001.md did
   not. Rule 9 forbids amending `e1070d2`, so the remedy is a note in DRIFT-001's
   record, added to that file's Round 1 by this reviewer.
2. *The source-text guards are effective but brittle.* They match text, not
   semantics: a harmless rename of `vim_exe` would fail them, and they do not
   verify the guarded call is reachable or in the correct branch. The criterion
   that the displayed directory remains the child working directory is verified
   by inspection of `drift.c:3225` only — the guard stops at
   `CreateProcess(vim_exe, command`.
3. *Trailer vocabulary is drifting.* `8501bfb` uses `Investigated-by:`, which
   `README.md` does not define. It is sensible and correctly avoids claiming
   `Implemented-by` for a documentation-only commit, but the protocol defines
   only `Audit-ID`, `Implemented-by`, and `Reviewed-by`; the set should be pinned
   before it fragments.

**Checked and dismissed:** removing `SetSearchPathMode` is inert, because that
flag governs `SearchPath` and no `SearchPath` call remains; both surviving
`CreateProcess` calls pass non-null absolute application names; Vim's own
bare-command lookups from the displayed directory are the DRIFT-031 class but are
Vim's behavior under an explicitly set working directory, identical to running it
there from a shell; and the Vim path has no DRIFT-032-style silent failure
because the `ShellExecute` fallback is visible behavior.

**Scope check:** clean. `e1070d2` touches `drift.c` in five related areas —
declarations, the `SetSearchPathMode` removal, resolver generalization, the new
wrappers, and `OpenFileInEditor` — plus its own tracker row, issue file, test
runner, and new test file. `8501bfb` is documentation only. No unrelated
cleanup, formatting, or refactoring.

**Resolution:** approved with no requested changes. No finding requires a change
to this commit set, so the item closes as `Verified`. Finding 1 is actioned as a
note in DRIFT-001's review round; findings 2 and 3 are recorded observations for
the maintainer. Recorded by the reviewer under section 6 because no implementer
was present in the session; no implementer-authored section of this file was
modified.

## Record maintenance

**2026-07-25 — Codex; documentation only.** In response to Round 1's first and
third records-level observations, the commit adding this section carries both
`Audit-ID: DRIFT-001` and `Audit-ID: DRIFT-002`, explicitly bridges the shared
refactor into DRIFT-001's discoverable history, and updates `quality/README.md`
to define repeatable audit IDs plus the optional `Reported-by:` and
`Investigated-by:` trailers. It does not amend reviewed commits or change the
reviewer's text, verdict, or evidence.
