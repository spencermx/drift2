# DRIFT-010 — The 256-session limit can omit the newest sessions

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Untriaged`
**Reported:** 2026-07-25; comprehensive review
([`audits/2026-07-25-comprehensive-review.md`](../audits/2026-07-25-comprehensive-review.md)).
Record created 2026-08-01 by Claude to preserve control-flow evidence found
while surveying the ledger.
**Initial severity:** Medium
**Primary locations:** `src/drift.c:LoadSessionsFor` (enumeration loop and its
`MAX_SESSIONS` bound), `src/drift.c:CompareSessions`
**Implemented by:** —
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

**Trigger:** A workspace anchor accumulates more than `MAX_SESSIONS` (256)
`.jsonl` transcripts under its encoded `~/.claude/projects/<encoded-anchor>`
directory. The user opens that workspace's session list.

**Impact:** The list shows an arbitrary 256 of the available transcripts rather
than the 256 most recent. The truncation is silent — nothing in the pane
indicates that sessions were dropped — so a recent session can simply be absent
from the only view that offers resume, rename, and delete. Since the session
list exists almost entirely to return to recent work, a miss here defeats the
view's purpose rather than degrading it at the margin.

The tracker title says the limit *can* omit the newest sessions. The stronger
claim below is that once the cap is exceeded, retention is uncorrelated with
recency, so the newest session is no more likely to survive than the oldest.

**Evidence:** Static control flow at `b5649927`.

`LoadSessionsFor` fills the global `sessions[]` array inside a `do/while`
(`src/drift.c:1807-1826`). The loop's continuation condition is
`FindNextFile(hFind, &fd) && session_count < MAX_SESSIONS`, so enumeration stops
as soon as 256 entries have been accepted. The mtime sort runs afterward, at
`src/drift.c:1829`:

```c
} while (FindNextFile(hFind, &fd) && session_count < MAX_SESSIONS);
FindClose(hFind);

qsort(sessions, session_count, sizeof(SessionEntry), CompareSessions);
```

`CompareSessions` (`src/drift.c:2354-2358`) orders newest-first by
`ftLastWriteTime`, but it only ever sees the entries that survived truncation.
The set is therefore chosen by `FindFirstFile`/`FindNextFile` enumeration order,
which is a directory index order with no relationship to modification time. On
NTFS that index is keyed by filename, and these filenames are Claude's session
UUIDs, assigned independently of when the transcript was last written. The
retained 256 are consequently arbitrary with respect to recency.

The four rejection paths inside the loop (`src/drift.c:1808` directories,
`1811` path overflow, `1816` id overflow, `1822` unsafe id) all `continue`
without incrementing `session_count`, so skipped files correctly do not consume
slots. That part behaves as intended and is not implicated.

Not reproduced against a live directory of more than 256 transcripts; the
finding rests on control flow and documented enumeration semantics rather than
observation. A reproduction would need a synthetic project directory with more
than 256 UUID-named `.jsonl` files whose mtimes are deliberately anti-correlated
with their names.

**Open questions:**

- Should the cap be preserved at all, or should the array grow dynamically?
  `sessions[]` is a fixed global (`src/drift.c:456`), so keeping a bound is the
  smaller change.
- If the bound stays, the fix is a bounded top-N retention: enumerate every
  candidate, keep the 256 newest by mtime, and discard the rest. Should that be
  an insertion into a sorted window, or a full collect-then-sort using a
  temporary allocation?
- Cost of a full enumeration is the real design constraint.
  `ReadSessionName` is called inside the loop for every accepted entry
  (`src/drift.c:1824`) and opens each transcript to scan up to 50 lines. A naive
  fix that enumerates all files the current way would pay that per-file cost
  across the entire history. Retention should be decided on `WIN32_FIND_DATA`
  mtimes alone, with `ReadSessionName` deferred until the surviving set is
  known. Is that split acceptable given `ApplySessionNames` runs afterward at
  `src/drift.c:1830`?
- Should the pane disclose truncation when it occurs, or is silent retention of
  the newest 256 sufficient once the selection is correct?

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Comprehensive review | — | `Untriaged` | Reported in the initial audit as one of 30 items. |
| 2026-08-01 | Claude | `Untriaged` | `Untriaged` | Issue record created to preserve control-flow evidence that truncation precedes the mtime sort, making retention arbitrary rather than merely biased. Status unchanged; no investigation performed and no severity reassessment made. |
