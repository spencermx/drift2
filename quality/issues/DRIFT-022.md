# DRIFT-022 — Quick-add exposes only the first nine workspaces

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Fix planned`
**Reported:** 2026-07-25; comprehensive review
([`audits/2026-07-25-comprehensive-review.md`](../audits/2026-07-25-comprehensive-review.md)).
Record created 2026-08-01 by Claude to preserve ordering evidence found while
surveying the ledger.
**Initial severity:** Low
**Primary locations:** `src/drift.c:HandleQuickAdd` (candidate collection and
digit selection), `src/drift.c:CompareFiles` (the ordering that decides which
nine appear)
**Implemented by:** —
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

**Trigger:** More than nine workspace folders exist under the workspaces root.
The user highlights a folder in the ordinary browser and presses the quick-add
key.

**Impact:** The popup offers nine workspaces and silently omits the rest. It
draws no count of what was hidden and no way to page, so from this view the
omitted workspaces do not exist. The user's only recourse is to leave, enter the
workspace list, arm edit mode on the intended workspace, and navigate back to
the folder — which is the whole workflow quick-add exists to shortcut.

The ordering evidence below is what makes this more than a bounded edge case:
the nine shown are the nine **oldest** workspaces, so the omitted set is exactly
the recently created ones a user is most likely to be filling.

**Evidence:** Static control flow at `b5649927`.

The candidate arrays are fixed at nine and the collection loop stops there
(`src/drift.c:3463-3466`):

```c
char names[9][MAX_PATH];
char labels[9][MAX_PATH];
int count = 0;
for (int i = 0; i < total && count < 9; i++) {
```

Selection is gated to the digits that can address them,
`ch >= '1' && ch < '1' + count` (`src/drift.c:3527`), so nine is a ceiling
imposed by the single-keystroke design as well as by the arrays. The popup is
sized `count + 5` rows (`src/drift.c:3480`) and its body draws only the `count`
listed entries (`src/drift.c:3505-3509`) — there is no "n more" line.

Which nine survive is decided upstream. Candidates come from
`GetFilesInDirectory(root, preview_files)` (`src/drift.c:3459`), which sorts its
results with `CompareFiles` (`src/drift.c:4893-4905`): directories first, then
`_stricmp` ascending by name. Workspace folder ids are minted timestamps — the
function's own comment calls them that at `src/drift.c:3461-3462`, and a live
example is `2026-08-01_22-05-25`. For that format, ascending lexicographic order
is ascending chronological order. The nine collected are therefore the nine
oldest workspaces by creation time, and they stay fixed as new workspaces are
created: every workspace made after the ninth is permanently unreachable from
quick-add.

Note that `labels[]` holds display names from `WorkspaceDisplayName` while
`names[]` holds the timestamp ids. Renaming a workspace changes only the label,
not the id, so a rename cannot move a workspace into or out of the visible nine.

Not reproduced interactively; established from control flow and the documented
id format. A reproduction needs ten or more workspace folders and an observation
of which nine the popup lists.

**Open questions:**

- Is the single-digit selection a deliberate constraint worth preserving? If so
  the fix is about *which* nine and about disclosing the remainder, not about
  raising the cap.
- If ordering is the fix, should candidates be sorted by recency of creation, by
  most recent use, or by the display name the user actually recognizes? Sorting
  by label would make the visible set stable and predictable but still arbitrary
  past nine.
- Should the popup page or scroll instead, accepting a second keystroke?
- Should it at minimum draw the number of workspaces not shown, so the omission
  stops being silent even before the ordering is settled?
- Does the same first-nine-by-name assumption appear anywhere else that consumes
  `GetFilesInDirectory` output for workspaces? Not surveyed as part of this
  record.

## Investigation — 2026-08-01, Claude

**Contract question.** Quick-add's stated purpose, from its own guard comment at
`src/drift.c:3448-3452`, is to add the highlighted browser folder to a workspace
without leaving the browser. A popup that cannot address most of the user's
workspaces does not merely limit that shortcut, it makes the shortcut unusable
for the newest workspaces specifically. This is a real contract violation rather
than a documented limitation: nothing in the UI states a cap, and the popup
renders identically whether or not workspaces were omitted.

**Severity.** Recorded initial severity is `Low` ("bounded edge case"). The
ordering evidence argues for `Medium` under "a materially broken core workflow",
since the omission is permanent, silent, and targets exactly the workspaces in
active use. Not changed unilaterally — this is the decision owner's call, and it
is raised here rather than applied.

**The cap is not where the real work is.** `preview_files` holds `MAX_FILES`
(4096) entries (`src/drift.c:421`, `110`) and `GetFilesInDirectory` has already
populated it with every workspace by the time the nine are chosen. The full set
is in hand; only the collection loop and the digit gate discard it.

**`HandleOldHistory` is the working reference for this popup.** It solves the
same problem — a centered, digit-selected popup over the alternate buffer — and
solves it more completely (`src/drift.c:5557-5649`):

- It paints from *inside* the input loop behind a `repaint` flag, and treats
  `WINDOW_BUFFER_SIZE_EVENT` as `DrawScreen()` plus repaint
  (`src/drift.c:5621-5625`).
- It cancels outright when the console has shrunk below the popup size, with the
  comment "cancel rather than leave the number keys live behind nothing"
  (`src/drift.c:5593-5595`).
- It offers **ten** slots, `1`–`9` plus `0` (`src/drift.c:5630-5640`), so ten is
  the house precedent for digit selection, not nine.

`HandleQuickAdd` does none of these. It paints once before its loop
(`src/drift.c:3475-3517`) and its loop reads keys without handling
`WINDOW_BUFFER_SIZE_EVENT` at all (`src/drift.c:3520-3561`).

**This couples DRIFT-022 to [DRIFT-014](../TRACKER.md).** That item —
"Quick-add remains invisibly active after resize", `Untriaged`, same function —
is precisely the missing repaint described above. Any fix for DRIFT-022 must
restructure the paint and input loop that DRIFT-014 also lives in. Under README
rule 6 the two cannot be silently bundled into one commit, which makes ordering
a decision to take now rather than discover mid-implementation.

**Recommended sequencing: fix DRIFT-014 first, then DRIFT-022 on top.** Fixing
022 first means writing the popup paint code once for the nine-to-N change and
then again when 014 moves it inside the loop. Fixing 014 first establishes the
`HandleOldHistory`-shaped structure, after which 022 is a change to candidate
selection and one drawn line, with no paint restructuring left in it. This also
keeps each commit honestly isolated with a single `Audit-ID`.

### Options considered for DRIFT-022 itself

| # | Approach | Cost | Leaves unreachable? |
|---|---|---|---|
| 1 | Reverse the collection loop so it takes the newest nine | One-line change | Yes, past nine |
| 2 | Option 1 plus a `+N more` line and a tenth slot (`0`) | Small | Yes, past ten |
| 3 | Scrollable window over all workspaces, digits as accelerators for the visible page | Moderate | No |

Option 1 is rejected as a standalone fix: it corrects *which* workspaces are
offered but leaves the omission silent, so the user still cannot tell the popup
is hiding anything. Option 2 is the minimum honest fix.

**Recommended: option 3.** Quick-add's value is that it is fast, so digit
selection must survive — but a cap of any size reintroduces this same bug at a
higher threshold, and the workspaces root is expected to grow indefinitely.
Scrolling with `j`/`k` over a ten-row window, with `1`–`9`/`0` selecting within
the currently visible page, keeps the one-keystroke path for the common case
while making every workspace reachable. Ordering should be newest-first so the
fast path lands on recent work.

The tradeoff against option 2: option 3 makes the digits *positional within a
scrolling window* rather than stable identifiers, so a user who has memorized
"`3` is the ArcFM workspace" loses that once the window scrolls. Option 2
preserves stable digits at the price of a permanent ceiling. If stable digits
matter more than completeness, take option 2 and accept the ceiling explicitly
as the contract.

### Acceptance criteria (accepted 2026-08-01)

1. With more than ten workspaces present, every workspace can be reached and
   selected from the quick-add popup without leaving the browser.
2. Candidates are ordered newest-first by workspace folder id.
3. The popup never silently hides candidates: either all are reachable by
   scrolling, or the count of hidden candidates is drawn.
4. Digit selection applies to the currently visible entries and cannot select a
   candidate that is not on screen.
5. Selecting a candidate adds the highlighted folder to that workspace, with the
   existing `MEMBER_CHANGE_*` refusal messages unchanged
   (`src/drift.c:3530-3553`).
6. Renaming a workspace changes only its displayed label and never its position
   or reachability, since `names[]` holds the timestamp id and `labels[]` the
   display name.

### Proposed regression tests

No existing test covers `HandleQuickAdd`; the suite under `tests/` is unit-level
against helpers rather than the popup loop. Candidate ordering and window
arithmetic should be extracted into a pure helper that a new
`tests/quick_add_test.c` can drive with a synthetic workspace list — asserting
newest-first order, that a workspace beyond the first window is reachable, and
that digit selection maps to the visible page. The popup's drawing and input
handling stay outside test coverage, consistent with the rest of the UI layer;
say so explicitly in the fix record rather than implying the loop is covered.

### Non-goals

- Most-recently-*used* ordering. No usage timestamps exist; creation order via
  the folder id is the only signal available without new state.
- Search or filtering within the popup.
- Changing `CompareFiles`, which serves the ordinary file browser correctly and
  is only implicated here through reuse.
- Fixing DRIFT-014 under this ID.

### Residual risks

- Option 3 changes a memorized-digit interaction. If the workspaces root is
  small in practice, the scrolling machinery is unused complexity.
- Newest-first ordering is derived from the timestamp id format. A workspace
  folder created by other means, or a future change to the id format, would
  order arbitrarily. The fix should not assume the format is parseable — string
  ordering is sufficient and should be relied on rather than date parsing.

### Disposition — 2026-08-01

The decision owner accepted **option 3 (scrolling)** and the recommended
sequencing: **DRIFT-014 is fixed first**, then this item on top of the
restructured paint loop. Two isolated commits, one `Audit-ID` each; the coupled
single-commit alternative permitted by rule 6 was declined.

The stable-digit tradeoff was accepted as the cost of completeness. Digits
select within the visible window and their meaning changes as the window
scrolls; this is intended behavior, not a defect to be reported later.

Severity remains `Low` as recorded. The argument for `Medium` above stands in
the record but was not adopted, and this item should not be re-escalated without
a fresh decision.

**This item is blocked until DRIFT-014 reaches `Verified`.** Implementation must
not begin while the paint loop it depends on is still being restructured.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Comprehensive review | — | `Untriaged` | Reported in the initial audit as one of 30 items. |
| 2026-08-01 | Claude | `Untriaged` | `Untriaged` | Issue record created to preserve evidence that `CompareFiles` ordering over timestamp ids makes the visible nine the nine oldest workspaces, so newly created ones are permanently unreachable. Status unchanged; no investigation performed and no severity reassessment made. |
| 2026-08-01 | Claude | `Untriaged` | `Investigating` | Investigation opened. Established that the full workspace set is already in `preview_files`, that `HandleOldHistory` is a more complete implementation of the same popup, and that the fix is coupled to DRIFT-014 through the shared paint loop. Three options recorded with option 3 recommended; severity reassessment to `Medium` raised but not applied. Awaiting disposition. |
| 2026-08-01 | Claude | `Investigating` | `Fix planned` | Decision owner accepted option 3 (scrolling window, newest-first, digits selecting within the visible page) and the DRIFT-014-first sequencing. Stable-digit loss accepted as intended behavior. Severity left at `Low`. Blocked until DRIFT-014 is `Verified`. |
