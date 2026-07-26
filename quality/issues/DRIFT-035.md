# DRIFT-035 — Quick-add from armed edit mode leaves the manifest describing another workspace

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Not a bug`
**Reported:** 2026-07-26; Claude, while independently reviewing
[DRIFT-006](DRIFT-006.md)
**Initial severity:** Low
**Final severity:** Low
**Primary locations:** `drift.c:HandleQuickAdd`, `drift.c:ApplyMemberChange`,
`drift.c:JumpToMemberAt`, `drift.c:HandleInput`
**Implemented by:** —
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

**Trigger:** Arm workspace edit mode on workspace A, then press `Shift+W` to
quick-add the cursor directory to a different workspace B, then `Tab` into the
manifest. The manifest now lists workspace B's folders under workspace A's
banner. Pressing Enter navigates using workspace A's anchor.

**Impact:** Display and navigation only; no settings file is written
incorrectly. The manifest pane misrepresents which workspace's membership the
user is looking at, and `Enter` on a relative row opens a directory that is
neither the row's own target nor, necessarily, anything meaningful. Pressing
`X`/`Space` on such a row runs a removal transaction against workspace A using a
path shown from workspace B, which normally resolves to
`MEMBER_CHANGE_NO_CHANGE` but can remove a genuine A member if both workspaces
happen to share that folder.

**Evidence:** `Shift+W` is not consumed by the armed-edit branch, because that
branch's fall-through guard is `if (!ctrl && !shift)` at `drift.c:3316`, so the
key reaches `HandleQuickAdd()` at `drift.c:3379` with `edit_armed` still true.
`HandleQuickAdd` calls `ApplyMemberChange(anchor_of_B, …)`, which calls
`LoadMembersFrom(anchor_of_B)`, replacing the process-global `members[]`,
`member_count`, and the `member_anchor` that DRIFT-006 introduced — while
`edit_workspace` still names A. Nothing restores A's state.

A reviewer probe drove exactly that sequence with two disposable workspaces that
both store the same relative spelling `.\shared`:

```text
after EnterEditMode(A): member_anchor=[...\wsA]
after quick-add:        member_anchor=[...\wsB]
  edit_workspace=[...\wsA]
  member_anchor != edit_workspace: YES
  row [.\shared] resolves via member_anchor -> ...\wsB\shared
  row [.\shared] resolves via edit_workspace -> ...\wsA\shared
  JumpToMemberAt returned true, current_directory=[...\wsA\shared]
  VERDICT: jump used edit_workspace (NOT where members came from)
```

Both workspaces lived under a unique `%TEMP%` root; no real Drift or Claude
configuration was read or written, and all artifacts were removed.

**Two contributing parts, one root cause:**

1. *Pre-existing:* quick-add clobbers the edit-mode membership globals. This
   predates DRIFT-006 — the earlier code also called `LoadMembersFrom(anchor)`
   for the quick-add target — and is the actual root cause.
2. *Introduced by DRIFT-006, but only a symptom:* `JumpToMemberAt`
   (`drift.c:2807`) resolves against `edit_workspace` rather than
   `member_anchor`, the variable DRIFT-006 added specifically to record which
   workspace `members[]` came from. Using `member_anchor` would make the jump
   internally consistent with the row being displayed. It would not fix part 1.

**Why this is not a data-integrity defect:** DRIFT-005's transaction reloads the
correct anchor under its lock before every write, so an add or remove still
rebases onto the true contents of whichever workspace it targets. The damage is
confined to what the pane shows and where `Enter` navigates.

**Open questions:**

- Should `HandleQuickAdd` save and restore the edit-mode membership state, or
  should quick-add be refused (or scoped to the edited workspace) while
  `edit_armed` is set?
- Should the manifest pane derive its contents from `edit_workspace` on every
  draw rather than trusting whatever last populated the globals?
- Should `JumpToMemberAt` and `RemoveMemberAt` use `member_anchor` instead of
  `edit_workspace`, so the operation always matches the rows on screen?
- Is the membership state better held per-workspace rather than in one set of
  process globals?

## Investigation and disposition

The reported production trigger does not reach the state described above.
`HandleInput` does allow modified keys to fall through the ordinary armed-edit
key block, so `Shift+W` reaches the call expression for `HandleQuickAdd`.
However, the first executable guard inside `HandleQuickAdd` is:

```c
if (claude_mode != CM_OFF || edit_armed || anchor_armed) return;
```

With the report's required `edit_armed == true` precondition, the function
returns before reading a target, opening the workspace chooser, calling
`ApplyMemberChange`, or invoking `LoadMembersFrom`. Consequently `members[]`,
`member_count`, `member_anchor`, and `edit_workspace` remain the state loaded
for workspace A. `JumpToMemberAt` continues resolving A's displayed rows from
A's anchor, and focused removal continues operating on A's displayed list.

Repository history confirms this is not a later repair. `git blame` attributes
the `edit_armed` guard to `5cc15db` (2026-07-25 07:26 -0600), and
`git merge-base --is-ancestor 5cc15db 0c1372c` succeeds. The guard therefore
predates the comprehensive audit baseline and the DRIFT-006 implementation.
Both `git show 0c1372c^:drift.c` and `git show 0c1372c:drift.c` contain it.

The reviewer probe remains useful evidence about what would happen if the
membership globals were forced to describe B while `edit_workspace` still
described A. It does not establish that the stated `Shift+W` sequence can
produce that prerequisite through production control flow. The probe must have
bypassed the early guard or constructed the divergent globals directly.

Whole-file call-site enumeration found no alternate production route that can
load a different workspace while edit mode is armed. `LoadMembersFrom` is
called by `EnterEditMode`, by `ApplyMemberChange` for edit-mode operations on
`edit_workspace`, and by `HandleQuickAdd` only after the blocking guard.

The user authorized closing this report after the contradiction was identified.
DRIFT-035 is therefore **Not a bug as reported**. No production or regression
change is required. If a different reachable path is later found that can
diverge `member_anchor` from `edit_workspace`, it should be recorded from that
concrete trigger rather than reopening this disproven one without new evidence.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-26 | Claude | — | `Untriaged` | Reported while independently reviewing DRIFT-006; reproduced the anchor divergence and wrong-anchor jump through production functions. The root cause is pre-existing global clobbering, so it is tracked separately rather than folded into that fix. |
| 2026-07-26 | Codex | `Untriaged` | `Investigating` | Traced the complete `Shift+W` path, enumerated membership-load call sites, and checked the guard's Git ancestry after the reviewer report conflicted with current production control flow. |
| 2026-07-26 | Codex | `Investigating` | `Investigating` | Confirmed `HandleQuickAdd` returns immediately whenever `edit_armed` is true and that the guard predates both the audit and DRIFT-006; the probe demonstrated only a manually constructed divergent state, not the reported UI trigger. |
| 2026-07-26 | User | `Investigating` | `Not a bug` | Authorized the proposed documentation correction and closure after the production guard and historical evidence showed the reported trigger cannot mutate membership state. |
