# DRIFT-034 — An unreadable existing settings.json is edited as an empty member list

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Untriaged`
**Reported:** 2026-07-26; Claude, while independently reviewing
[DRIFT-005](DRIFT-005.md)
**Initial severity:** Medium
**Primary locations:** `drift.c:LoadMembersFrom`, `drift.c:MemberSourceMatches`,
`drift.c:ApplyMemberChange`
**Implemented by:** —
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

**Trigger:** A workspace's `.claude\settings.json` exists but cannot be opened
at the moment `LoadMembersFrom` runs — a transient sharing violation from
antivirus, a backup agent, or Claude itself holding the file — and the condition
clears before `SaveMembersTo` re-reads a few milliseconds later. No concurrent
Drift process is required.

**Trigger clarification — 2026-07-26, Codex:** The destructive publication also
requires an add operation while the transaction sees the failed read as an
empty list. A removal against that apparent empty list returns
`MEMBER_CHANGE_NO_CHANGE` and does not call `SaveMembersTo`. This narrows the
minimal trigger without changing the reported root cause, impact, or severity.

**Impact:** The membership transaction publishes a list built from zero members
and returns `MEMBER_CHANGE_SAVED`. Every folder previously granted to that
workspace is silently removed from `permissions.additionalDirectories`, and the
user is told the edit succeeded. Unrelated settings survive, because DRIFT-004's
splice is byte-preserving; only the membership array is destroyed. Claude then
loses access to every one of those directories.

**Evidence:** Control flow is direct. `LoadMembersFrom` distinguishes a
genuinely absent file from an unreadable one, but only acts on the absent case:

```c
FILE* f = fopen(file, "rb");
if (f == NULL) {
    /* ... */
    if (absent) RememberMemberSource(NULL, 0, false);
    return;                 // exists-but-unreadable: no member source recorded
}
```

That early return leaves `member_count == 0`, `json_block_reason == NULL`, and
`member_source_known == false`. `ApplyMemberChange` sees no block reason and
proceeds. DRIFT-005's new defense then disables itself, because
`MemberSourceMatches` opens with:

```c
if (!member_source_known) return true;
```

so the save cannot detect that the on-disk array differs from what the
transaction believed it read.

A deterministic reviewer probe included the production `drift.c` translation
unit, intercepted `fopen` to fail exactly the first open of `settings.json`
inside the transaction, and allowed the save's re-read to succeed. Against a
three-member fixture:

```text
settings.json open attempts inside the transaction: 2
ApplyMemberChange result: 0 (MEMBER_CHANGE_SAVED)
pre-existing members retained: one=NO two=NO three=NO
new member written: yes   unrelated env preserved: yes
VERDICT: CONFIRMED - transaction reported SAVED while deleting existing members
```

**This is not a DRIFT-005 regression.** The same probe run against
`git show 6aea09b:drift.c` — the DRIFT-004 state, before the locking
transaction existed — loses the identical three members with
`SaveMembersTo` returning `true`. The defect predates DRIFT-005; that item
neither introduced nor closed it, and its new source-conflict check silently
no-ops on this path rather than covering it.

The probe workspace lived under a unique `%TEMP%` directory. No real Drift or
Claude configuration was read or written, and every artifact was removed.

**Relationship to other items:** independent of DRIFT-005, whose root cause is a
stale snapshot shared between two *successful* readers. Here a single process
with a *failed* read treats "cannot read" as "no members". It is the same shape
that [DRIFT-003](DRIFT-003.md) fixed for name metadata, where `SetNameEntry`
learned to abort on any non-absence open error; `LoadMembersFrom` adopted only
the absence half of that pattern. Distinct from DRIFT-004, which governs
structural targeting once the bytes are in hand.

**Open questions:**

- `LoadMembersFrom` returns `void` and has no way to report "the file exists but
  could not be read". Should it gain a status, set `json_block_reason`, or record
  an explicit unknown-source state that `MemberSourceMatches` treats as a
  conflict rather than a match?
- Should an unknown member source fail closed at save time in general, making
  every path that skipped `RememberMemberSource` refuse publication?
- Should the transaction retry a failed read under the lock before giving up,
  given the lock already bounds how long it will wait?
- Does the same "unreadable is not empty" gap affect any other reader that feeds
  a full-array rewrite?

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-26 | Claude | — | `Untriaged` | Reported while independently reviewing DRIFT-005; reproduced through production load/save and confirmed pre-existing at `6aea09b`, so it is tracked separately rather than folded into that fix. |
| 2026-07-26 | Codex | `Untriaged` | `Untriaged` | Clarified that the destructive failed-read path requires an add operation; a removal returns no-change without publication. No investigation or disposition performed. |
