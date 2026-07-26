# DRIFT-005 — Concurrent workspace member changes can be overwritten

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Awaiting review`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Final severity:** Medium
**Primary locations:** `drift.c:AcquireMemberLock`,
`drift.c:ApplyMemberChange`, `drift.c:LoadMembersFrom`,
`drift.c:SaveMembersTo`, the three membership UI callers, and
`tests/membership_concurrency_test.c`
**Implemented by:** Codex
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

## Trigger and impact

Two Drift processes open or otherwise act on the same workspace. Each obtains a
valid snapshot of `permissions.additionalDirectories`; one publishes a change,
then the other publishes a different change derived from its older snapshot.
Every read, write, close, and rename can succeed.

The later save reports success but replaces the complete membership array with
its stale version. A folder added by the first writer can disappear, or a folder
the first writer removed can reappear. The former silently removes Claude's
access from a workspace; the latter silently restores access the user explicitly
removed. Unrelated settings survive because DRIFT-004 correctly targets only the
membership array, but that does not protect concurrent changes *inside* the
array.

The same stale-state shape can occur when another tool edits the array while a
Drift process remains in workspace edit mode. Quick-add narrows the interval by
loading immediately before its change, but it has the same unguarded gap between
load and publication.

## Confirmed control flow

1. `EnterEditMode` calls `LoadMembersFrom` once. The returned member list stays
   in process-global `members` for the entire edit session; there is no refresh
   before a later toggle or removal.
2. `ToggleMemberUnderCursor` and `RemoveMemberAt` mutate that global array and
   call `SaveMembersTo` directly.
3. `HandleQuickAdd` reloads immediately before adding, which shortens but does
   not close the race. It also mutates the globals and calls the same save.
4. `SaveMembersTo` serializes the global `members` array *before* re-reading
   `settings.json`. Its fresh read validates the complete JSON and preserves
   unrelated keys, but never compares the current on-disk membership with the
   membership that the caller originally saw.
5. The checked temp write and `MoveFileEx(..., MOVEFILE_REPLACE_EXISTING)` then
   publish the stale complete array. There is no lock, version check, merge, or
   conditional replacement, so success means only that this publication won —
   not that it included an intervening membership edit.
6. Every membership save uses the same sibling `settings.json.tmp`. Concurrent
   writers can therefore contend over both the logical snapshot and the staging
   filename. A unique temp name alone would not fix the stale-snapshot overwrite.

There are exactly three production save call sites: removal in
`RemoveMemberAt`, add/toggle in `ToggleMemberUnderCursor`, and quick-add in
`HandleQuickAdd`. A fix must route all three through the same transaction rather
than repairing only edit mode or only quick-add.

## Reproduction

Confirmed on branch `codex/future-work` at `d12f71a`. A disposable Windows probe
included the actual `drift.c` translation unit and used production
`LoadMembersFrom` and `SaveMembersTo` below a uniquely named `%TEMP%` workspace.
It compiled with `/W4 /WX /wd4459` and AddressSanitizer.

The probe modeled two separate process address spaces by capturing writer A's
global member snapshot, letting writer B independently reload and call the
production save, then restoring A's snapshot before A called the same production
save. This is the exact valid interleaving two processes produce without
requiring timing-sensitive threads.

Scenario 1 started with `base`. B added `B-added` and saved successfully. A then
added `A-added` from its stale `base` snapshot and also saved successfully. The
final production load contained `base` and `A-added`; `B-added` was gone.

Scenario 2 started with `base` and `victim`. B removed `victim` and saved
successfully. A then added `A-added` from its stale snapshot and saved
successfully. The final production load contained `victim` again.

Exact probe result:

```text
lost add: B save=true, A save=true, B survives=no => CONFIRMED
resurrected removal: B save=true, A save=true, victim present=yes => CONFIRMED
DRIFT-005 result: CONFIRMED
```

The probe source, executable, object, PDB, settings file, temp file, and
directories were removed after the run. No real Drift or Claude configuration
was read or changed.

## Platform behavior relevant to a fix

Microsoft documents that a successful `CreateFile` with `dwShareMode == 0`
prevents another open until the handle closes, and a conflicting open fails with
`ERROR_SHARING_VIOLATION`:
<https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea>.
That provides a small dependency-free cooperative lock when all Drift writers
open the same per-workspace lock file exclusively.

Microsoft also documents that process termination closes open kernel-object
handles automatically:
<https://learn.microsoft.com/en-us/windows/win32/procthread/terminating-a-process>.
A persistent lock *file* can therefore remain on disk without becoming a stale
logical lock after a crash; ownership is the open no-sharing handle, not file
existence.

`MoveFileEx` can replace an existing destination, but its documented contract
does not provide a compare-and-replace precondition tied to the bytes a caller
read:
<https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexa>.
The current checked rename protects against partial publication, not lost
updates.

## Severity assessment

Medium remains appropriate. The defect silently loses persistent configuration
and can undo an explicit membership removal while both operations report
success. Membership is a core Drift workflow, and edit mode leaves the race
window open for an arbitrary amount of time. The affected file and folders are
not deleted, the configuration is manually recoverable, and a resurrected path
was previously selected by the user rather than attacker-controlled, so High
would overstate the impact.

## Options considered

1. **Give every save a unique temp filename.** This avoids staging-file
   collisions but does nothing about two complete stale arrays replacing each
   other. Rejected as insufficient.
2. **Compare the on-disk array with the load-time snapshot and refuse a
   mismatch.** This detects many edits but two writers can both compare before
   either publishes, then still overwrite each other. It also turns independent
   additions into avoidable user retries. Useful only as defense in depth for
   non-cooperating writers.
3. **Take a lock only around the existing `SaveMembersTo`.** This serializes
   publication but still saves the stale array assembled before the lock. The
   second writer would reliably overwrite the first. Rejected.
4. **Serialize an operation-level read/modify/write transaction.** Recommended.
   Represent the user's intent as “add this exact path” or “remove this exact
   path,” acquire one per-workspace exclusive lock, reload the latest array while
   holding it, reapply that intent, publish, and release. Independent changes
   merge naturally; same-path changes become explicit idempotent outcomes.
5. **Move settings into a Drift-owned database or one-record-per-file store.**
   Claude, not Drift, owns the settings format and consumes this exact JSON path.
   A second canonical store would require synchronization back into the same
   file and would not remove the race. Rejected.

## Approved design

Introduce one operation API, conceptually
`ApplyMemberChange(anchor, path, ADD|REMOVE)`, and make it the only production
route that changes membership:

1. Ensure the workspace's `.claude` directory exists, then acquire a
   `GENERIC_READ | GENERIC_WRITE`, no-sharing `CreateFile` handle on a stable sibling such as
   `.claude\.drift-members.lock`. Retry only `ERROR_SHARING_VIOLATION` for a
   short bounded interval; never block the TUI indefinitely.
2. Keep the lock file. Deleting it after release would create a race in which
   one process holds the old file object while another creates and locks a new
   object at the same name. Closing the handle releases ownership; a crash does
   so through normal Windows process cleanup.
3. With the handle held, call the production loader and reject malformed,
   ambiguous, oversized, or otherwise blocked JSON exactly as DRIFT-004 does.
   Do not use the member list that happened to be visible before lock
   acquisition as the transaction base.
4. Reapply the requested path to the freshly loaded list. An add already present
   and a removal already absent are no-change outcomes, not reasons to rewrite a
   stale snapshot. Preserve the current case-insensitive identity and maximum
   member limit.
5. Call the checked structural save while still holding the lock. The lock must
   cover fresh load, operation rebase, temp creation/write/close, and final
   rename. Close the handle on every exit path.
6. Return a typed outcome so callers can distinguish changed, already in the
   requested state, full, settings blocked, lock busy, and write failure. Update
   the in-memory pane from the fresh transaction state and keep all failures
   visibly reported.
7. Retain `SaveMembersTo`'s fresh structural validation and byte-preserving
   splice. As defense in depth, record the membership source read inside the
   transaction and refuse or retry if the save's second read sees that a
   non-cooperating writer changed the target array before publication.
8. The existing fixed `.tmp` path is safe among updated Drift processes because
   it is used only while holding the per-workspace lock. The implementation must
   enforce that no production caller bypasses the operation API.

This gives a strong guarantee for concurrent updated Drift processes and a
best-effort conflict check for other settings writers. No ordinary file API can
make a non-cooperating program honor Drift's separate lock, so the record must
not claim an absolute guarantee against an external atomic replacement in the
last read-to-rename window.

## Implementation

- `LoadMembersFrom` now records the exact raw target-array bytes, or the fact
  that the target is absent, from the same bounded structural parse that fills
  the visible member list. The dynamic source buffer is replaced on every load
  and freed during application cleanup.
- `SaveMembersTo` compares its fresh parse with that recorded membership source
  before constructing a splice. A changed, newly created, or removed target
  array is refused with a typed conflict path, while changes to unrelated JSON
  remain allowed and are preserved from the fresh read.
- `AcquireMemberLock` opens `.claude\.drift-members.lock` with
  `GENERIC_READ | GENERIC_WRITE`, no sharing, and `OPEN_ALWAYS`. It retries only
  sharing violations for at most 1.5 seconds, using wrap-safe
  `GetTickCount64`; all other errors fail immediately. The persistent file is
  marked hidden/not-indexed when first created and is never deleted by Drift.
- `ApplyMemberChange` owns the complete transaction. It acquires the lock,
  reloads, re-evaluates an exact add or removal against the latest list, calls
  the checked structural save while still locked, captures a typed outcome,
  resyncs on failure, and closes the lock handle on every path.
- Same-state operations return `MEMBER_CHANGE_NO_CHANGE`; full, unsafe, busy,
  source-conflict, and ordinary I/O outcomes remain distinguishable. Edit-mode
  callers display specific blocking notifications, and quick-add retains its
  contextual success/refusal banner.
- `RemoveMemberAt`, `ToggleMemberUnderCursor`, and `HandleQuickAdd` are the only
  production mutation call sites, and all now call `ApplyMemberChange`. No
  production mutation caller bypasses that operation API to call
  `SaveMembersTo` directly.
- `README.md:Where things live` documents the lock artifact and handle-owned
  lifetime. `tests/settings_json_test.c` now establishes the absent-file source
  through the real loader before testing first publication.
- `tests/membership_concurrency_test.c` includes production `drift.c` and uses a
  unique `%TEMP%` workspace. Its child modes exercise the actual executable's
  lock across processes; no user's workspace or Claude configuration is read.
- `tests/run_tests.bat` adds the new AddressSanitizer suite as stage 4 and runs
  the complete Windows validation in eight stages.

## Acceptance criteria and implementer evidence

| Criterion | Result | Evidence |
|---|---|---|
| Two Drift writers adding different folders preserve both additions and the original list. | Pass | Two real child processes begin behind a parent-held production lock; after release both return `MEMBER_CHANGE_SAVED`, and the final production load contains `base`, `child-one`, and `child-two`. |
| A concurrent removal cannot be resurrected by an unrelated stale add. | Pass | A deliberately stale snapshot contains `victim`, the external fixture removes it, and `ApplyMemberChange(ADD)` preserves that removal while adding the unrelated path. |
| A concurrent addition cannot be erased by a stale removal. | Pass | A stale removal rebases over an external `B-added`; the final list removes only `victim` and retains `base` plus `B-added`. |
| Every mutation rebases on a load performed after lock acquisition. | Pass | All three deterministic stale-display cases modify the file after the visible load and require exact merged membership from the operation API; the cross-process test requires both children to succeed rather than conflict. |
| Same-path concurrent operations have defined idempotent results. | Pass | Add-present and remove-absent both return `MEMBER_CHANGE_NO_CHANGE` and leave the compact fixture byte-for-byte unchanged. |
| Lock contention is bounded and visible. | Pass | A child times out with `MEMBER_CHANGE_BUSY` while the parent owns the real lock, leaves settings and temp unchanged, then a post-release operation succeeds. Production callers map busy to explicit feedback. |
| Process exit cannot leave a permanent logical lock. | Pass | A child signals after acquiring the production lock, is forcibly terminated, and the parent immediately acquires the same persistent lock file. |
| The lock spans load through publication and every caller uses it. | Pass | Production source has only the declaration, definition, and one transaction-internal `SaveMembersTo` reference; guards require the exact counts for `SaveMembersTo`, `ApplyMemberChange`, and `AcquireMemberLock`, plus lock close and stable path. |
| Concurrent updated Drift writers cannot share the temp transaction. | Pass | The real child-process union test and parent-held contention test prove that only the lock owner reaches save/temp publication. |
| DRIFT-004 target and preservation guarantees remain intact. | Pass | All 19 production settings cases, 19 shared locator cases, six member-refusal cases, and exact plugin/nested/malformed/NUL/depth fixtures pass unchanged. |
| Failures never claim success or leave the pane on an invented state. | Pass | Full-list, structurally unsafe, lock-open failure, lock-busy, and target-conflict cases return typed non-success, preserve exact settings, leave no temp, and resync through production load. The pre-existing checked write/close/move branches are unchanged and were inspected; this item does not claim new fault-injection coverage for those branches. |
| Regression coverage detects the original bug. | Pass | The pre-fix production probe at `d12f71a` loses an add and resurrects a removal. The permanent stale-state cases require the opposite outcomes, and the pre-fix source lacks the required transaction API/wiring. |

## Validation performed

- Disposable pre-fix production probe at `d12f71a` — both saves returned true,
  one addition was lost, and one removal was resurrected; exit 0; all artifacts
  removed.
- `cmd /d /c tests\run_tests.bat` — `ALL CHECKS PASSED`, eight stages:
  source lint, general AddressSanitizer regressions, 19/19 DRIFT-004 production
  settings cases, 13/13 DRIFT-005 production/cross-process cases, 13/13 name
  metadata cases, 17/17 Claude launcher cases, 13/13 Vim resolver cases, and
  `/W4 /WX` production compilation. The general suite also includes the 19
  shared locator and six member-refusal cases.
- `cmd /d /c build.bat` — optimized `/O2` build succeeded; the ignored
  executable was removed afterward.
- `cl /analyze /W4 /wd4459 /c drift.c` — exit 0. The first run identified the
  new lock timeout's `GetTickCount` wrap warning; the implementation changed to
  `GetTickCount64`. The final run reports only the three established diagnostics
  in unchanged code: the `HandleOldHistory` stack frame and parameter/global
  shadowing in `GetSelectedRowPath` and `GetFilePath`.
- `bash tests/run_tests.sh` — unavailable on this Windows host because `cc` is
  not installed in the Bash environment; it stopped at stage 1 before compiling.
  The changed transaction and concurrency coverage is Windows-specific and is
  exercised by the complete native Windows suite above.
- `git diff --check` — passed.

Safe optional manual validation may open the same disposable workspace in two
Drift processes, add different folders, and remove one while the other remains
in edit mode. Never use a real workspace for failure-path testing; the automated
child-process suite already exercises deterministic contention and crash paths.

## Compatibility, security, and error paths

- DRIFT-004's path-aware parser, fail-closed structure handling, and preservation
  of all unrelated JSON bytes are required compatibility contracts.
- DRIFT-006 relative-path semantics are unchanged and remain separately tracked.
- A persistent `.drift-members.lock` is a new app-owned coordination artifact;
  Claude should ignore it, and the README documents its purpose and handle-owned
  lifetime.
- A bounded wait keeps another stuck or slow process from freezing the UI. A
  timeout must leave settings unchanged and produce explicit feedback.
- An exclusive lock file is not a security boundary. Another process with the
  user's filesystem rights can ignore or delete it after release, edit settings,
  or replace the directory. The goal is data integrity among cooperating Drift
  instances, not protection from a malicious local process.
- Wine and unusual network filesystems may emulate Win32 sharing differently;
  their behavior requires validation and remains residual if this host cannot
  exercise it.

## Non-goals and residual risk

- Serializing name metadata is DRIFT-033 and is not authorized by this item,
  even if a helper could later be shared.
- Resolving relative membership paths is DRIFT-006.
- Coordinating arbitrary third-party editors that do not take Drift's lock is
  not fully solvable with the current file contract. A source comparison can
  detect many such changes but cannot create an atomic compare-and-replace.
- Compatibility with an older concurrently running Drift binary that does not
  know the lock cannot be guaranteed.
- The existing temp-and-rename path does not claim power-loss durability beyond
  its checked write/close/publication behavior.

## User disposition

On 2026-07-26, the user approved the recommended bounded per-workspace lock,
operation-level rebase, fresh-source conflict check, typed outcomes, and
production-linked concurrency coverage, and authorized implementation.

## Independent review handoff

The eligible reviewer must not be Codex. Read this file and
`quality/README.md`, then locate every immutable commit with:

```text
git log --all --reverse --format="%H %s" --grep="Audit-ID: DRIFT-005"
```

Inspect the investigation and implementation commits plus surrounding settings
code. Pay particular attention to the lock's path and lifetime, acquisition
before the transaction load, closure on every exit, operation rebase semantics,
the exact source-array comparison, all three production callers, and
DRIFT-004 compatibility. Run `cmd /d /c tests\run_tests.bat` and evaluate all
twelve acceptance rows independently. Confirm that the change does not claim to
fix DRIFT-006, DRIFT-033, old concurrently running binaries, or arbitrary
non-cooperating external replacements. Report `Approved`, `Approved with
residual risk`, or `Changes requested` using the workflow's required format.

## Review history

No independent review rounds yet.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible successful-I/O lost-update race. |
| 2026-07-26 | Codex | `Untriaged` | `Investigating` | Began production control-flow tracing and a disposable concurrent-writer reproduction; no production fix authorized. |
| 2026-07-26 | Codex | `Investigating` | `Investigating` | Confirmed a lost add and a resurrected removal with both production saves reporting success; retained Medium severity and recommended a bounded per-workspace lock plus operation-level rebase; awaiting user disposition. |
| 2026-07-26 | User | `Investigating` | `Fix planned` | Approved the bounded per-workspace lock, operation-level rebase, source-change defense, typed outcomes, and production-linked concurrency tests. |
| 2026-07-26 | Codex | `Fix planned` | `Fixing` | Began the isolated production transaction, caller routing, and regression implementation. |
| 2026-07-26 | Codex | `Fixing` | `Awaiting review` | Implemented the approved transaction, routed all membership mutations through it, passed the focused and full native Windows suites, and completed the independent-review handoff. |
