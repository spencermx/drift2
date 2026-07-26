# DRIFT-003 — Name metadata can be replaced by a truncated file

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Verified`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Final severity:** Medium
**Primary locations:** `drift.c:SetNameEntry`, `SetSessionName`,
`SetWorkspaceName`, rename/create/delete callers; `tests/name_entry_test.c`
**Implemented by:** Codex
**Reviewed by:** Claude: approved with residual risk
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

### Round 1 — Claude, 2026-07-25

- **Reviewer:** `Claude` (Opus 5). Absent from `Implemented by`, so eligible.
- **Commit set:** `52db0ae` "Investigate DRIFT-003: confirm lossy name rewrite"
  (documentation only; also carries `Audit-ID: DRIFT-033`) and `6387846`
  "Fix DRIFT-003: preserve names on failed rewrites". `git log --all --reverse
  --format="%H %s" --grep="Audit-ID: DRIFT-003"` returns these two and no
  others.
- **Verdict:** `Approved with residual risk`.

**Acceptance criteria:** all eleven pass. Rows 1–6 and 10–11 were re-verified
directly by the reviewer rather than accepted from the evidence column; rows
7–9 were confirmed by reading the production callers, since they have no
automated coverage (see the coverage gap below).

**Tests run by the reviewer:**

- `cmd /c tests\run_tests.bat` — `ALL CHECKS PASSED`, six stages: source lint,
  general AddressSanitizer regressions, 13/13 name-metadata cases, 17/17 Claude
  launcher cases, 13/13 Vim resolver cases, and the `/W4 /WX` production
  compile. DRIFT-001 and DRIFT-002 coverage is unaffected.
- `build.bat` — optimized `/O2` build succeeded. `drift.exe` was removed
  afterward; the working tree was clean before and after the review.
- `cl /analyze /W4 /wd4459 /c drift.c` — exit 0. Three diagnostics, all in code
  this fix does not touch: C6262 (32,900-byte stack frame) at `drift.c:4149` in
  `HandleOldHistory`, and C6244 shadowing at `drift.c:4414` and `drift.c:4430`.
  None intersects `SetNameEntry` or any changed caller. This matches the
  implementer's claim.
- **Mutation testing (reviewer-added, run against throwaway copies of `drift.c`
  outside the repository; the repository was never modified).** The issue record
  correctly notes the suite cannot be run against the literal pre-fix source,
  because the pre-fix `void` API does not compile against the new assertions. To
  test the claim anyway, four targeted mutants were built and run:
  1. Delete the `GetFileAttributes` absence check so any failed open is again
     treated as an absent file — this is the original DRIFT-003 defect. Result:
     "an unreadable existing file is preserved and reported as failure" FAILS.
  2. Force `flags = MOVEFILE_REPLACE_EXISTING` unconditionally. Result: the two
     first-file cases FAIL.
  3. Ignore the `MoveFileEx` return value. Result: the publication-failure and
     concurrent-creation cases FAIL.
  4. Evaluate the key match per buffer chunk instead of per logical line.
     Result: "an over-long matching row is removed through its newline" FAILS.
  All four mutants exit non-zero, so the suite does discriminate on the actual
  production behavior and would detect reintroduction.

**Independent verification of specific claims:**

- The over-long-row case is genuinely multi-chunk. The production buffer is
  `MAX_PATH + SESSION_NAME_LEN + 64` = 260 + 96 + 64 = 420 bytes, so `fgets`
  reads at most 419 at a time; the 1,208-byte fixture row spans three chunks.
- Test isolation holds. `GetDriftDir` (`drift.c:GetDriftDir`) reads `DRIFT_HOME`
  on every call rather than caching it, so the suite's per-run temp root really
  does divert every name-file path away from the user's real data, and the root
  is removed at the end.
- All four production call sites were enumerated with a whole-file search; none
  was left ignoring the new `bool`. `HandleRenameSession` (`drift.c:1753`)
  returns before touching `sel->name`; `HandleRenameWorkspace` (`drift.c:1872`)
  acknowledges failure; `HandleDeleteSession` (`drift.c:1945`) records cleanup
  failure only inside the successful-`SHFileOperation` branch and reports it
  after the reload; `HandleCreate` (`drift.c:4021`) short-circuits so a
  non-custom name never calls the setter and the workspace still exists under
  its raw id.
- Failure paths close both streams. The early `break` on a failed `fputs` still
  falls through to `ferror(in)`/`fclose(in)`, and `fclose(out)` is
  unconditional, so no handle leaks on any failure route.
- DRIFT-033 was not silently treated as fixed. No locking, conflict detection,
  or unique temp naming was added, and its tracker row remains `Untriaged`.

**Findings — no code defects. Three residual/records-level items:**

1. *An embedded NUL byte desynchronizes the new logical-line tracker.*
   `drift.c:1044-1045` derive `chunk_len` from `strlen(line)` and `ends_line`
   from `strchr(line, '\n')`. If a row contains a NUL before its newline,
   `strchr` cannot see the newline, `at_line_start` is left `false`, and the
   *next* logical row is never tested against `key` — so a matching row placed
   immediately after a NUL-containing row is retained while the replacement row
   is also appended, yielding a duplicate. This needs external corruption:
   both of Drift's write paths (`fputs` of a `fgets` chunk, and the `fprintf`
   append) emit C strings, so Drift never writes a NUL into these files. The
   pre-fix code truncated at a NUL through the same `fputs` and could also
   mis-match a continuation chunk, so this is not a regression, and the outcome
   is a duplicate row rather than data loss. Judged below the section 1
   reporting bar, so no new ID was opened; recorded here so it is not lost.
2. *The create-race path leaves the in-memory name cache stale.* At
   `drift.c:1070-1074`, when the file was confirmed absent and `MoveFileEx`
   fails because another process created the destination first, the function
   correctly returns `false` without publishing — but `workspace_names_loaded`
   is not cleared, so Drift keeps serving display names from a cache that
   predates the competing file until something else invalidates it. Refusing to
   claim success is the right behavior for this item, and the pre-fix
   unconditional invalidation was paired with unconditionally overwriting the
   competing file, so this is not a net regression. It belongs to the
   cross-process class already owned by [DRIFT-033](DRIFT-033.md) and is
   cross-linked there rather than given a new ID.
3. *A final row with no trailing newline still concatenates with the appended
   row.* If the existing file's last line lacks `\n`, the `fprintf` at
   `drift.c:1060` appends directly onto it. This is pre-existing and unchanged
   by this fix — Drift always terminates its own rows — so it is out of scope,
   noted only so a future editor of this function does not assume it was
   handled.

**Coverage gap:** acceptance rows 7–9 (rename ordering, workspace-create partial
success, session-delete partial success) have no automated coverage. They are
console handlers with no test seam, and the reviewer confirmed each by reading
the production code, but a future refactor of those handlers would not be caught
by the suite. This is a disclosed limitation, not a defect in the fix.

**Checked and dismissed:** the `GetLastError()` at `drift.c:1025` is read
immediately after `GetFileAttributes`, so it is that call's error and not stale
`fopen` state; a `fopen` failure with the file still present (sharing violation,
`EMFILE`, access denied) correctly yields `absent == false` and aborts; the
directory-missing case is moot because `GetDriftDir` creates `.drift` before
`GetNameFile` returns; `DeleteFile(tmp)` can never target the real file because
`tmp` is `file` plus a `.tmp` suffix; a leftover temp after a refused
`DeleteFile` self-heals because the next attempt opens it `"wb"`; a key can
never straddle a chunk boundary because `klen <= MAX_PATH` (260) is always below
the 419-byte read; and over-invalidating `workspace_names_loaded` on a
session-names write is pre-existing and harmless.

**Scope check:** clean. `6387846` touches `drift.c` only in `SetNameEntry`, the
two wrappers, and the four callers; adds `tests/name_entry_test.c`; renumbers
`tests/run_tests.bat` from five to six stages and inserts the new stage; and
updates only this issue file and its own tracker row. `52db0ae` is documentation
only. No unrelated cleanup, formatting, or refactoring.

**Resolution:** approved with residual risk. No finding requires a change to
this commit set, so the item closes as `Verified` with findings 1–3 and the
coverage gap preserved above as reviewer-discovered residual risk, separate from
the implementer's own residual-risk section. Recorded by the reviewer under
section 6 because no implementer was present in the session; no
implementer-authored section of this file was modified.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible lossy metadata rewrite. |
| 2026-07-25 | Codex | `Untriaged` | `Investigating` | Confirmed the control flow and reproduced destructive replacement under transient read contention; documented the fail-closed design, tests, and separate concurrency concern. |
| 2026-07-25 | User | `Investigating` | `Fix planned` | Approved the recommended fail-closed streaming transaction and authorized implementation. |
| 2026-07-25 | Codex | `Fix planned` | `Fixing` | Began the isolated production change and production-linked regression coverage. |
| 2026-07-25 | Codex | `Fixing` | `Awaiting review` | Implemented checked streaming publication and caller feedback, added 13 production-linked regression cases, and passed focused, full, optimized, warning, and static-analysis validation. |
| 2026-07-25 | Claude | `Awaiting review` | `Verified` | Independent review approved with residual risk: full suite, optimized build, and `/analyze` re-run; four targeted mutants confirmed the suite detects reintroduction; no code defects; NUL-byte line-tracking edge, create-race cache staleness, and the caller-side coverage gap recorded as reviewer-discovered residual risk. |
