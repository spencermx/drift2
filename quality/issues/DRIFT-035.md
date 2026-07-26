# DRIFT-035 — Quick-add from armed edit mode leaves the manifest describing another workspace

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Untriaged`
**Reported:** 2026-07-26; Claude, while independently reviewing
[DRIFT-006](DRIFT-006.md)
**Initial severity:** Low
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

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-26 | Claude | — | `Untriaged` | Reported while independently reviewing DRIFT-006; reproduced the anchor divergence and wrong-anchor jump through production functions. The root cause is pre-existing global clobbering, so it is tracked separately rather than folded into that fix. |
