# DRIFT-033 — Concurrent name metadata updates can overwrite each other

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Untriaged`
**Reported:** 2026-07-25; Codex, during investigation of
[DRIFT-003](DRIFT-003.md)
**Initial severity:** Medium
**Primary locations:** `drift.c:SetNameEntry`
**Implemented by:** —
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

**Trigger:** Two Drift processes update `workspace-names` or `session-names`
concurrently. Both can read the same original snapshot before either publishes
its edit.

**Impact:** The later publisher rewrites its stale snapshot over the earlier
publisher, silently losing the first process's otherwise successful rename or
cleanup. The fixed sibling `.tmp` name also lets instances contend over the same
staging path, but merely giving each process a unique temp would not prevent the
stale-snapshot lost update.

**Evidence:** The existing algorithm is an unlocked read-modify-write
transaction. A valid interleaving is: A reads the original; B reads the same
original; A writes and replaces it with edit A; B writes its stale copy and
replaces edit A with edit B. Every individual I/O call may succeed. DRIFT-003's
fail-closed error checks therefore cannot detect or fix this race.

This is independent from DRIFT-003: that item prevents failed or incomplete I/O
from being published, while this item requires serialization, conflict
detection, or a different storage/update design for two successful operations.

**Open questions:**

- Is running multiple Drift instances against one `DRIFT_HOME` a supported
  workflow that must preserve simultaneous metadata edits?
- Should updates take an exclusive lock with bounded retry, detect that the
  source changed before publication, or move to one-record-per-file storage?
- What user feedback is appropriate when a concurrent edit cannot be merged or
  retried safely?

## Cross-linked observations

**2026-07-25 — Claude; reviewer note from [DRIFT-003](DRIFT-003.md) Round 1.**
DRIFT-003's fix added a second cross-process symptom that belongs to this item's
scope rather than its own. When `SetNameEntry` confirmed the metadata file was
absent, it now publishes with `flags = 0` and returns `false` if another process
created the destination first (`drift.c:1070-1074`). That correctly refuses to
overwrite the competing file, but it returns without clearing
`workspace_names_loaded`, so this process keeps serving display names from a
cache that predates the competing file until some later successful write
invalidates it. It is not a regression — the pre-fix code did invalidate the
cache unconditionally, but only because it also overwrote the competing file
unconditionally. Triage of this item should decide cache invalidation on refused
publication alongside serialization, since a losing writer generally needs to
re-read rather than keep its stale snapshot.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Reported while separating DRIFT-003's failed-I/O corruption from an independently fixable successful-I/O race. |
| 2026-07-25 | Claude | `Untriaged` | `Untriaged` | Cross-linked a reviewer observation from DRIFT-003 Round 1: refused publication now leaves the workspace-name cache stale. Status unchanged; no investigation performed. |
