# DRIFT-005 — Concurrent workspace member changes can be overwritten

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Verified`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Final severity:** Medium
**Primary locations:** `drift.c:AcquireMemberLock`,
`drift.c:ApplyMemberChange`, `drift.c:LoadMembersFrom`,
`drift.c:SaveMembersTo`, the three membership UI callers, and
`tests/membership_concurrency_test.c`
**Implemented by:** Codex
**Reviewed by:** Claude: approved with residual risk
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

### Round 1 — Claude, 2026-07-26

- **Reviewer:** `Claude` (Opus 5). Absent from `Implemented by`, so eligible.
- **Commit set:** `a084c52` "Investigate DRIFT-005: confirm concurrent membership
  loss" (documentation only) and `e47c219` "Fix DRIFT-005: serialize workspace
  membership changes". `git log --all --reverse --format="%H %s"
  --grep="Audit-ID: DRIFT-005"` returns these two and no others.
- **Verdict:** `Approved with residual risk`.

**Acceptance criteria:** eleven of twelve pass as claimed. Row 8 ("the lock spans
load through publication and every caller uses it") is correct in the
implementation but is **not established by the cited evidence** — see finding 2.
Row 12's headline sentence ("failures never claim success") overstates what was
verified, though its own evidence list does not include the branch that
contradicts it — see finding 1. Neither requires a change to this commit set.

**Tests run by the reviewer:**

- `cmd /d /c tests\run_tests.bat` — `ALL CHECKS PASSED`, eight stages, exit 0,
  including 13/13 membership-concurrency cases, 19/19 DRIFT-004 settings cases,
  13/13 name-metadata cases, 17/17 Claude launcher, 13/13 Vim resolver, and the
  `/W4 /WX` compile. DRIFT-001 through DRIFT-004 coverage is unaffected.
- `build.bat` — optimized `/O2` build succeeded; `drift.exe` removed afterward.
- `cl /analyze /W4 /wd4459 /c drift.c` — exit 0. Three diagnostics, all in code
  this fix does not touch: C6262 at `drift.c:4305` and C6244 at `drift.c:4571`
  and `drift.c:4587`. No `GetTickCount` wrap warning remains, confirming the
  `GetTickCount64` change the record describes.

**Reviewer-added mutation testing** (throwaway copies outside the repository;
no repository file was modified). Six mutants of the transaction were compiled
against the permanent concurrency suite:

1. *Remove the in-transaction `LoadMembersFrom` rebase* — the DRIFT-005 defect
   itself. **Caught**, 7 of 13 cases fail. This is the decisive negative
   control.
2. *Release the lock immediately before `SaveMembersTo`* — restores a
   lost-update window while keeping the load serialized. **NOT caught**; the
   entire suite passes. See finding 2.
3. *Remove the `MemberSourceMatches` conflict check* — **caught**.
4. *Make the lock non-exclusive (`FILE_SHARE_READ | FILE_SHARE_WRITE`)* —
   **caught** by both real cross-process cases, confirming they genuinely
   exercise Win32 sharing rather than simulating it.
5. *Remove the `MAX_MEMBERS` guard* — **caught**, via an AddressSanitizer abort
   on the out-of-bounds `strcpy`.
6. *Remove the idempotent-add short circuit* — **caught**.

**Independent verification of specific claims:**

- All production mutation routing was enumerated by whole-file search:
  `SaveMembersTo` is called exactly once, at `drift.c:2609`, between lock
  acquisition at `drift.c:2569` and `CloseHandle` at `drift.c:2625`. The three
  callers at `drift.c:2653`, `drift.c:2670`, and `drift.c:2845` all go through
  `ApplyMemberChange`. The remaining `LoadMembersFrom` at `drift.c:2685` is
  `EnterEditMode`'s read-only display load.
- Every exit path from `ApplyMemberChange` reaches `CloseHandle(lock)`: all five
  early returns use `goto done`, and the save path falls through to it.
- No user-blocking call happens while the lock is held. `ReportMemberChangeFailure`
  and `NotifyAndWait` run in the callers, after the handle is closed, so a
  modal prompt cannot pin the lock.
- `RememberMemberSource(arr, strlen(arr), true)` after a successful save stores
  exactly the bytes the splice wrote, so a fresh parse of the new file yields an
  identical span in all three publication shapes (replace, insert-in-permissions,
  insert-permissions, and absent-file skeleton). A second operation in the same
  process therefore cannot false-conflict.
- `OPEN_ALWAYS` with `FILE_ATTRIBUTE_HIDDEN` is safe on an already-existing
  non-hidden lock file: Win32 applies `dwFlagsAndAttributes` only on creation,
  so this does not repeat the documented `CREATE_ALWAYS` hidden-file
  `ERROR_ACCESS_DENIED` trap.
- DRIFT-006 and DRIFT-033 are not silently treated as fixed. No relative-path
  resolution and no name-metadata serialization were added; both tracker rows
  remain `Untriaged`.

**Findings — no defect introduced by this change. Three items:**

1. *A transaction whose load cannot read an existing `settings.json` publishes a
   list built from zero members and reports success.* `LoadMembersFrom`
   (`drift.c:2244-2251`) distinguishes absent from unreadable but acts only on
   the absent case, returning with `member_count == 0`,
   `json_block_reason == NULL`, and `member_source_known == false`.
   `MemberSourceMatches` (`drift.c:2210`) then returns `true` unconditionally
   for an unknown source, so DRIFT-005's new defense disables itself on exactly
   this path. A deterministic probe that fails only the transaction's own
   `fopen` and lets the save's re-read succeed returned
   `MEMBER_CHANGE_SAVED` while deleting all three pre-existing members.
   **This is not a regression:** the identical loss reproduces against
   `git show 6aea09b:drift.c`, before this transaction existed. Because the root
   cause is independent of the stale-snapshot race this item fixes — a failed
   read treated as an empty list, rather than two successful readers racing —
   rule 3 requires a separate ID rather than folding it in, and rule 6 forbids
   mixing it into these commits. Filed as
   [DRIFT-034](DRIFT-034.md) with the reproduction preserved. Worth noting that
   [DRIFT-003](DRIFT-003.md) fixed this exact shape for name metadata;
   `LoadMembersFrom` adopted only the absence half of that pattern.
2. *The lock's most important property is untested.* Acceptance row 8 claims the
   lock spans load through publication, and cites the source-text wiring guard.
   Mutant 2 above releases the lock immediately before `SaveMembersTo` — which
   reopens a real lost-update window between two cooperating Drift processes —
   and the whole suite still passes, wiring guard included. The implementation
   is correct; the coverage cannot tell correct from incorrect here. A test that
   holds the parent lock and asserts a child cannot publish *while the parent is
   mid-transaction* would close this.
3. *The wiring guard checks token counts, not the invariant it is named for.*
   `TestProductionWiring` (`tests/membership_concurrency_test.c:362-390`) counts
   occurrences of `SaveMembersTo(`, `ApplyMemberChange(`, and
   `AcquireMemberLock(` and asserts that `CloseHandle(lock);` and the lock path
   appear somewhere. It cannot see where any call sits relative to the lock, and
   would fail on a harmless rename or a comment mentioning a token. This is the
   same brittleness class raised in DRIFT-002 Round 1; effective as a tripwire,
   but not a proof of routing.

**Checked and dismissed:** the unlocked `LoadMembersFrom` on the lock-failure
path is a display refresh only and cannot publish; a workspace whose
`permissions` object gains no array between load and save is correctly treated
as matching, because inserting into it loses nothing; quick-add still leaves the
process globals describing the quick-add target rather than `edit_workspace`,
but that is pre-existing, display-only, and harmless to the transaction because
every operation reloads its own anchor first; the 1.5 s bounded wait with 25 ms
retries cannot freeze the TUI indefinitely; `GetTickCount64` subtraction is
wrap-safe; and `strcpy(members[member_count], path)` is bounded by the
`strlen(path) >= MAX_PATH` guard at the top of `ApplyMemberChange`.

**Scope check:** clean. `e47c219` touches only the membership transaction and
its callers in `drift.c`, adds `tests/membership_concurrency_test.c`, adjusts
`tests/settings_json_test.c` to establish the absent-file source through the
real loader, inserts one stage in `tests/run_tests.bat`, documents the lock
artifact in `README.md`, and updates this issue file and its own tracker row.
`a084c52` is documentation only. No unrelated cleanup or refactoring.

**Resolution:** approved with residual risk. No finding requires a change to
this commit set: finding 1 is a pre-existing independent defect now tracked as
DRIFT-034, and findings 2–3 are coverage observations. Recorded by the reviewer
under section 6 because no implementer was present in the session; no
implementer-authored section of this file was modified, including the acceptance
rows this round disputes.

**Addendum — 2026-07-26, Claude: shared-code change in a later commit.**
`0c1372c` "Fix DRIFT-006: anchor relative workspace members" materially rewrites
part of `ApplyMemberChange` (adding a path-resolution gate and replacing the
removal loop with all-equivalent compaction) and changes the semantics of
`FindMember`, which this item's rebase decision depends on. That commit carries
only `Audit-ID: DRIFT-006`, so the ordinary
`git log --grep="Audit-ID: DRIFT-005"` search does not reach it; this note
supplies the linkage until a bridge commit does. Compatibility was confirmed
during DRIFT-006 Round 1 by the same reviewer, who implemented neither item:
the lock still spans load through publication (`AcquireMemberLock` at
`drift.c:2705`, `SaveMembersTo` at `drift.c:2757`, one `CloseHandle` exit
reached by every path, and every added statement inside that window), all 13
concurrency cases pass unchanged, and they stayed green under all six DRIFT-006
mutants. DRIFT-005 therefore remains `Verified` under rule 6 rather than
returning to `Awaiting review`.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible successful-I/O lost-update race. |
| 2026-07-26 | Codex | `Untriaged` | `Investigating` | Began production control-flow tracing and a disposable concurrent-writer reproduction; no production fix authorized. |
| 2026-07-26 | Codex | `Investigating` | `Investigating` | Confirmed a lost add and a resurrected removal with both production saves reporting success; retained Medium severity and recommended a bounded per-workspace lock plus operation-level rebase; awaiting user disposition. |
| 2026-07-26 | User | `Investigating` | `Fix planned` | Approved the bounded per-workspace lock, operation-level rebase, source-change defense, typed outcomes, and production-linked concurrency tests. |
| 2026-07-26 | Codex | `Fix planned` | `Fixing` | Began the isolated production transaction, caller routing, and regression implementation. |
| 2026-07-26 | Codex | `Fixing` | `Awaiting review` | Implemented the approved transaction, routed all membership mutations through it, passed the focused and full native Windows suites, and completed the independent-review handoff. |
| 2026-07-26 | Claude | `Awaiting review` | `Verified` | Independent review approved with residual risk: full eight-stage suite, optimized build, and `/analyze` re-run; six transaction mutants confirm the rebase, conflict check, exclusivity, bounds, and idempotence are all detected, while releasing the lock before publication is not; no defect introduced. A pre-existing unreadable-read data loss found during review was reproduced, confirmed present at `6aea09b`, and split to DRIFT-034 rather than folded in. |
| 2026-07-26 | Codex | `Verified` | `Verified` | Added a documentation-only maintenance note correcting the acceptance-row reference and post-acquisition exit count, and clarifying how the 11/12 result treats the lock-lifetime coverage gap; verdict and evidence unchanged. |
| 2026-07-26 | Codex | `Verified` | `Verified` | Added the prescribed documentation-only bridge from this audit history to DRIFT-006 commit `0c1372c` and Claude's compatibility review `38cc6db`; status and verdict unchanged. |

## Record maintenance

**2026-07-26 — Codex; documentation only.** Claude's Round 1 remains intact and
attributed to Claude. Read three phrases in that immutable review record with
these corrections:

- “Row 12's headline sentence” refers to acceptance criterion **11**, “Failures
  never claim success or leave the pane on an invented state.” Criterion 12 is
  the original-bug regression-coverage criterion.
- “Eleven of twelve pass as claimed” counts criterion 11 as the single
  overbroad criterion. Criterion 8 passes through Claude's independent source
  inspection: the lock is held across `SaveMembersTo` and closed afterward.
  The lock-release mutant establishes a coverage gap—the permanent suite does
  not enforce that ordering—but does not establish a second implementation
  failure.
- “All five early returns use `goto done`” should read: all **four**
  post-acquisition early outcome branches use `goto done`. The function's
  direct returns occur before lock ownership, plus the final return after
  `CloseHandle(lock)`.

This note changes no production code, regression test, status, verdict,
acceptance criterion, implementer attribution, or reviewer-authored text.

**2026-07-26 — Codex; shared-code audit bridge.** Claude's additive Round 1
addendum correctly identifies `0c1372c` as a later change to
`ApplyMemberChange`, `FindMember`, and membership removal semantics and records
the eligible compatibility review in `38cc6db`. The review commit already
carries both `Audit-ID: DRIFT-005` and `Audit-ID: DRIFT-006`, so it supplies a
discoverable route from this item to the immutable implementation. The bridge
commit adding this paragraph also repeats DRIFT-004/005/006 together, making
the complete shared-code relationship explicit and symmetric across all three
records. It changes no production code, test, status, verdict, attribution, or
reviewer-authored text.
