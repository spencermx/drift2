# DRIFT-003 — Name metadata can be replaced by a truncated file

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Awaiting review`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Final severity:** Medium
**Primary locations:** `drift.c:SetNameEntry`, `SetSessionName`,
`SetWorkspaceName`, rename/create/delete callers; `tests/name_entry_test.c`
**Implemented by:** Codex
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

## Trigger and impact

Before the fix, `SetNameEntry` rewrote `.drift\workspace-names` or
`.drift\session-names` through a sibling `.tmp` file. It treated every failure
to open the existing file as if the file did not exist, and it did not check
whether streaming the old contents, appending the new row, closing the streams,
or publishing the temp file succeeded. If a read or write failed and the
destination became replaceable before `MoveFileEx`, Drift could publish
incomplete metadata over the original.

Workspace display names or session display names after the failure point could
be silently lost. The underlying workspace directories and Claude transcripts
were not modified, but the loss persisted and could affect name uniqueness
checks and what the user saw in Drift.

## Original confirmed control flow

1. `SetNameEntry` opens the current metadata file with `fopen(file, "rb")`.
2. A null input stream is accepted without checking whether the file is absent
   or merely unreadable.
3. A fixed-size `fgets`/`fputs` loop copies whatever prefix can be read. It
   checks neither `ferror(in)` nor `fputs`.
4. `fprintf` optionally appends the replacement row. Its negative error result
   is ignored.
5. Both `fclose` results are ignored, including the output close that flushes
   buffered data.
6. `MoveFileEx(tmp, file, MOVEFILE_REPLACE_EXISTING)` runs unconditionally and
   its result is ignored.
7. `workspace_names_loaded` is invalidated even if no edit was published, and
   the function returns `void`, so callers update in-memory names or continue
   without knowing whether persistence succeeded.

The nearby pre-fix comment said a write error prevented the rename and preserved
the original. No branch enforced that claim.

The fixed input buffer was not itself a whole-file limit: nonmatching long lines
were copied in chunks. It created a second correctness edge, however. If a
matching row exceeded the buffer, only its first chunk was dropped; its suffix
was copied as a malformed orphan row. The implementation preserves streaming
while tracking whether the current logical line is being removed.

## Reproduction

A temporary Windows test included the production `drift.c` translation unit and
used a four-row `workspace-names` fixture:

```text
keep-a<TAB>Alpha
keep-b<TAB>Bravo
target<TAB>Old
keep-c<TAB>Charlie
```

The probe held the original with sharing that denied a new reader but permitted
deletion. A test hook released that handle immediately before production
`MoveFileEx`, deterministically modeling a transient process that blocks the
initial read and exits before publication. Production `SetNameEntry` treated the
failed input open as an absent file, wrote one new row, and replaced the
original. Observed result:

```text
target<TAB>New
```

The three unrelated rows were lost. A control that retained the sharing lock
through `MoveFileEx` left the original intact because replacement failed. That
control narrows the bug: persistent contention happens to fail safely, while a
transient failure between the unchecked open and unconditional publication is
destructive. The temporary source, executable fixture, and data directory were
removed after the run.

## Authoritative I/O behavior

- Microsoft documents `ferror` as the required stream-error indicator; a read
  loop ending does not by itself establish clean EOF:
  <https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/ferror?view=msvc-170>
- `fprintf` returns a negative value on output failure, and `fclose` returns
  `EOF` on failure. The latter matters because buffered output can fail during
  close:
  <https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fprintf-fprintf-l-fwprintf-fwprintf-l?view=msvc-170>
  and
  <https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fclose-fcloseall?view=msvc-170>
- `MoveFileEx` reports publication success or failure through its Boolean
  return value; `MOVEFILE_REPLACE_EXISTING` permits replacement when access and
  sharing requirements allow it:
  <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw>

## Severity assessment

Medium remains appropriate. The failure silently destroys persistent,
user-authored display-name metadata and can affect every row in either name
file, meeting the workflow's data-loss rubric. The trigger requires an I/O
failure or transient sharing condition, which reduces frequency. Workspace
directories, membership settings, and Claude transcripts remain intact, so the
impact is materially below deletion of primary user data.

## Options considered

1. **Check only `ferror`, output calls, and `fclose`.** Necessary but
   insufficient: the reproduced input-open failure still looks like an absent
   file and can replace all existing rows.
2. **Read the whole file into memory, edit it, and write it atomically.** Easier
   to reason about, but abandons the existing unbounded streaming property and
   introduces a size/allocation failure mode for a file that can grow with
   session history.
3. **Fail-closed streaming transaction with explicit result.** Recommended.
   Distinguish a genuinely absent input from an unreadable existing file;
   preserve chunked streaming while dropping every chunk of a matching logical
   row; check read, write, close, and publication results; delete a failed temp;
   invalidate caches only after success; and return `bool` through the workspace
   and session wrappers so callers do not claim an unsaved edit.
4. **Add cross-process locking or conflict detection in the same change.** This
   addresses a different successful-I/O lost-update race and carries separate
   compatibility decisions. It is recorded as [DRIFT-033](DRIFT-033.md) rather
   than folded into this fix.

Using `ReplaceFile` instead of `MoveFileEx` was also considered. It is designed
to preserve attributes and ACLs while replacing a file, but its documented
partial-failure states are more complex than this app-owned metadata requires.
The current same-directory `MoveFileEx` scheme is adequate once it is reached
only after a complete temp file exists and its return value is enforced.

## Agreed design and implementation

The user approved the recommended option. `SetNameEntry`, `SetSessionName`, and
`SetWorkspaceName` now return `bool`, and the production helper:

1. Attempts to open the existing file. If that fails, it uses file attributes and
   the precise not-found errors to distinguish absence from an unreadable,
   locked, or otherwise failed existing file. It aborts on every non-absence
   error.
2. Opens the sibling temp only after the input state is known. It keeps the current
   same-directory publication boundary; cross-instance temp naming and update
   serialization remain DRIFT-033's concern.
3. Copies in chunks while maintaining both logical-line-start and
   `dropping_line` state so a matching row,
   including an over-long one, is removed through its newline rather than only
   for the first buffer chunk, while nonmatching chunks cannot be mistaken for
   new rows.
4. Treats `ferror(in)`, failed `fputs`/`fprintf`, or failed input/output
   `fclose` as transaction failure. It closes both streams, deletes the temp,
   and leaves the original untouched.
5. Calls `MoveFileEx` only after the complete temp has closed successfully. It uses
   replacement only when an existing file was read; if the file was confirmed
   absent, publish without replacement so a newly created concurrent file is
   not silently overwritten. It checks the return and deletes the temp on
   failure.
6. Invalidates `workspace_names_loaded` only after successful publication and
   returns `true` only for that success.
7. Propagates the result through every caller. Session rename updates its
   in-memory title only after persistence. Workspace/session rename failures
   show an acknowledgement banner. Workspace creation remains successful under
   its raw ID if only the optional display name fails, and session deletion
   remains complete if stale-name cleanup fails; both partial outcomes are
   reported explicitly.

This retains ANSI paths, the tab-separated on-disk format, case-insensitive key
matching, row order, empty-name deletion, streaming of arbitrarily large files,
and same-directory temp-then-rename publication.

## Production and test changes

- `drift.c:SetNameEntry` contains the checked streaming transaction and
  conditionally replacing publication.
- `drift.c:SetSessionName` and `SetWorkspaceName` expose its result; session
  rename, workspace rename, session deletion, and workspace creation handle
  failure according to the agreed operation contract.
- `tests/name_entry_test.c` includes the production translation unit and
  intercepts its actual stdio and `MoveFileEx` boundaries. It never reads or
  writes the user's real `DRIFT_HOME`.
- `tests/run_tests.bat` runs the new suite under AddressSanitizer and `/W4 /WX`.

## Data integrity, compatibility, and error paths

- **Data integrity:** no failed or partial temp reaches publication. Existing
  metadata and the in-memory workspace-name cache remain unchanged on failure.
- **Creation race:** a file confirmed absent is published without
  `MOVEFILE_REPLACE_EXISTING`, so a destination created before publication wins
  and Drift reports failure instead of overwriting it.
- **Compatibility:** paths, encoding, delimiters, case-insensitive matching,
  row order for unrelated entries, replacement-row placement, and empty-name
  deletion are unchanged. Existing files are still processed as a stream.
- **User-visible errors:** persistence failures are generic rather than exposing
  OS error codes, but they no longer appear successful. Partial create/delete
  results say which primary operation completed.
- **Security:** the change adds no new path source, process launch, parser, or
  privilege boundary. Its security effect is limited to refusing unsafe
  publication under failed I/O.

## Acceptance criteria and evidence

| Criterion | Result | Evidence |
|---|---|---|
| An unreadable existing metadata file is preserved. | Pass | Injected `fopen` denial against an existing four-row production fixture returns failure, performs no publication, keeps the cache valid, and preserves every byte. |
| Temp-open, mid-read, copy-write, append-write, input-close, and output-close failures cannot publish partial data. | Pass | Six independent fault-injection cases preserve the original and remove the ordinary test temp. |
| Failed `MoveFileEx` preserves the original, cleans the temp, and leaves the cache valid. | Pass | Publication-failure case asserts all four outcomes and the attempted move count. |
| Replacement, deletion, and first-file creation retain the exact on-disk format and unrelated-row order. | Pass | Three success cases compare the complete resulting file byte-for-byte and check replacement flags. |
| First-file publication cannot overwrite a destination created after the absence check. | Pass | The move hook creates a competing destination; production returns failure and preserves the competing file because flags are zero. |
| An over-long matching logical row leaves no orphaned suffix. | Pass | A 1,200-byte matching value spans multiple production copy chunks and is removed through its newline. |
| Rename state changes only after persistence and failed saves are visible. | Pass | `HandleRenameSession` returns before mutating `sel->name`; both rename handlers acknowledge a false result. |
| Optional workspace display-name failure leaves the workspace created under its raw ID and reports it. | Pass | `HandleCreate` reloads/selects the created folder, then reports the exact raw ID if `SetWorkspaceName` failed. |
| Session metadata-cleanup failure never restores a deleted transcript and is reported. | Pass | `HandleDeleteSession` records cleanup failure only after successful `SHFileOperation`, reloads sessions, and then reports the partial result. |
| Regression coverage exercises production and detects the original contract violation. | Pass | The suite includes `../drift.c`; the original pre-fix transient-read probe reduced four rows to one, while the permanent test requires failure, no move, and byte preservation. The pre-fix `void` API also cannot satisfy the asserted result contract. |
| Focused/full tests, optimized build, warning compile, and static analysis complete. | Pass | Commands and outcomes are recorded below. |

## Validation performed

- `cmd /c tests\run_tests.bat` — `ALL CHECKS PASSED`: 13/13 name-metadata
  cases, the complete general ASan suite, 17/17 Claude launcher cases, 13/13
  Vim resolver cases, source lint, and `/W4 /WX` production compile.
- `cmd /c build.bat` — optimized `/O2` Windows release build succeeded. The
  ignored validation executable was removed afterward.
- `cl /analyze /W4 /wd4459 /c drift.c` under the same Visual Studio toolchain —
  exit 0. It reported three diagnostics in unchanged code: the existing large
  stack frame in `HandleOldHistory` and the two known parameter/global
  shadowing sites in `GetSelectedRowPath` and `GetFilePath`. No diagnostic
  intersects the DRIFT-003 implementation.
- `git diff --check` — passed.

Safe optional manual validation for the independent reviewer:

1. Set `DRIFT_HOME` to a disposable empty directory before launching Drift.
2. Create and rename a workspace, rename and clear a session name, then inspect
   `.drift\workspace-names` and `.drift\session-names` for the documented
   tab-separated rows.
3. Prefer the deterministic fault-injection suite for failure paths; do not
   lock or modify the user's real name metadata.

## Non-goals and residual risk

- Coordinating two successful concurrent read-modify-write operations is
  DRIFT-033. This item prevents failed I/O from being published; it does not
  claim to serialize multiple Drift processes.
- Making metadata recoverable from backups or reconstructing names already lost
  before the fix is outside scope.
- Converting the ANSI/tab-separated format or removing existing `MAX_PATH`
  limits is outside scope.
- A crash before publication leaves the original in place because the temp is
  not moved until after a successful close. No stronger rename-atomicity or
  power-loss durability guarantee is claimed.
- Temp cleanup after a failed write or move is best-effort because Windows can
  itself refuse `DeleteFile`; an uncommitted sibling temp does not replace the
  original. Fixed-temp cross-process contention remains part of DRIFT-033.

## User disposition

On 2026-07-25, the user approved the recommended fail-closed streaming design
and authorized its isolated implementation and regression coverage.

## Independent review handoff

The eligible reviewer must not be Codex. Read this file and the quality
protocol, then locate the complete immutable commit set with:

```text
git log --all --reverse --format="%H %s" --grep="Audit-ID: DRIFT-003"
```

Inspect every resulting patch plus the surrounding name-file code, run
`cmd /c tests\run_tests.bat`, and evaluate every acceptance-criteria row above.
Report `Approved`, `Approved with residual risk`, or `Changes requested` in the
required review format. Confirm that the commit contains no unrelated changes
and that DRIFT-033 was not silently treated as fixed.

## Review history

No independent review rounds have been recorded yet.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible lossy metadata rewrite. |
| 2026-07-25 | Codex | `Untriaged` | `Investigating` | Confirmed the control flow and reproduced destructive replacement under transient read contention; documented the fail-closed design, tests, and separate concurrency concern. |
| 2026-07-25 | User | `Investigating` | `Fix planned` | Approved the recommended fail-closed streaming transaction and authorized implementation. |
| 2026-07-25 | Codex | `Fix planned` | `Fixing` | Began the isolated production change and production-linked regression coverage. |
| 2026-07-25 | Codex | `Fixing` | `Awaiting review` | Implemented checked streaming publication and caller feedback, added 13 production-linked regression cases, and passed focused, full, optimized, warning, and static-analysis validation. |
