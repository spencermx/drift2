# DRIFT-003 — Name metadata can be replaced by a truncated file

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Investigating`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Final severity:** Medium
**Primary locations:** `drift.c:SetNameEntry`, workspace and session name files
**Implemented by:** —
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

## Trigger and impact

`SetNameEntry` rewrites `.drift\workspace-names` or `.drift\session-names`
through a sibling `.tmp` file. It treats every failure to open the existing file
as if the file did not exist, and it does not check whether streaming the old
contents, appending the new row, closing the streams, or publishing the temp
file succeeded. If a read or write fails and the destination becomes replaceable
before `MoveFileEx`, Drift publishes incomplete metadata over the original.

Workspace display names or session display names after the failure point can be
silently lost. The underlying workspace directories and Claude transcripts are
not modified, but the loss persists and can affect name uniqueness checks and
what the user sees in Drift.

## Confirmed control flow

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

The nearby comment says a write error prevents the rename and preserves the
original. No branch currently enforces that claim.

The fixed input buffer is not itself a whole-file limit: nonmatching long lines
are copied in chunks. It does create a second correctness edge, however. If a
matching row exceeds the buffer, only its first chunk is dropped; its suffix is
copied as a malformed orphan row. The fix should preserve streaming while
tracking whether the current logical line is being removed.

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

## Recommended implementation

Change `SetNameEntry`, `SetSessionName`, and `SetWorkspaceName` to return
`bool`. The production helper should:

1. Attempt to open the existing file. If that fails, use file attributes and
   the precise not-found errors to distinguish absence from an unreadable,
   locked, or otherwise failed existing file. Abort on every non-absence error.
2. Open the sibling temp only after the input state is known. Keep the current
   same-directory publication boundary; cross-instance temp naming and update
   serialization remain DRIFT-033's concern.
3. Copy in chunks while maintaining a `dropping_line` state so a matching row,
   including an over-long one, is removed through its newline rather than only
   for the first buffer chunk.
4. Treat `ferror(in)`, failed `fputs`/`fprintf`, or failed input/output
   `fclose` as transaction failure. Close both streams, delete the temp, and
   leave the original untouched.
5. Call `MoveFileEx` only after the complete temp has closed successfully. Use
   replacement only when an existing file was read; if the file was confirmed
   absent, publish without replacement so a newly created concurrent file is
   not silently overwritten. Check the return and delete the temp on failure.
6. Invalidate `workspace_names_loaded` only after successful publication and
   return `true` only for that success.
7. Have rename/create/delete callers update their in-memory view only after a
   successful result and provide visible failure feedback appropriate to the
   operation. Never fall back to a destructive or in-place rewrite.

This retains ANSI paths, the tab-separated on-disk format, case-insensitive key
matching, row order, empty-name deletion, streaming of arbitrarily large files,
and same-directory temp-then-rename publication.

## Proposed acceptance criteria and regression coverage

- An existing metadata file that cannot be opened for reading is never treated
  as absent and is byte-for-byte preserved.
- A simulated mid-read error, `fputs`/`fprintf` failure, or output-close failure
  never reaches publication; the original remains byte-for-byte unchanged.
- A failed `MoveFileEx` reports failure, preserves the original, removes the
  temp file, and does not invalidate the workspace-name cache.
- Successful replacement, deletion, and first-file creation still produce the
  exact tab-separated format and preserve all unrelated rows in order.
- A matching logical row longer than the copy buffer is removed completely;
  no orphaned suffix remains.
- Workspace/session rename state changes only after persistence succeeds.
  Failed saves produce visible feedback rather than appearing accepted.
- Workspace creation still succeeds if only its optional display-name overlay
  fails, but the user is told that the workspace exists under its raw ID.
- Session deletion never restores a deleted transcript merely because stale
  name cleanup failed; that cleanup failure is reported without damaging other
  metadata.
- A production-linked Windows test injects each failure at the actual
  `SetNameEntry` I/O boundary and includes the transient-lock negative control
  that fails against the pre-fix code.
- The focused tests, full regression suite, optimized build, warning-clean
  compile, and static analysis pass.

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

## User disposition needed

The diagnosis, severity, reproduction, and recommended design are ready for the
user's decision. No production code or permanent regression test has been
changed for DRIFT-003.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible lossy metadata rewrite. |
| 2026-07-25 | Codex | `Untriaged` | `Investigating` | Confirmed the control flow and reproduced destructive replacement under transient read contention; documented the fail-closed design, tests, and separate concurrency concern. |
