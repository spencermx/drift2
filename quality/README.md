# Drift quality workflow

This folder is Drift's canonical workspace for audits, bug reports, analysis,
fixes, tests, and independent reviews. It is deliberately tool- and
author-neutral: a human, Codex, Claude, or another system must follow the same
process. Conversation history is never required to use it.

Someone should be able to give a new reviewer only this instruction:

> Review every bug fix awaiting independent review in the `quality/` folder.

Someone should be able to give a bug-finding or implementation system only this
instruction:

> Follow the workflow documented in the `quality/` folder.

No additional process explanation from the user should be required.

## Folder contents

- `README.md` — this operating protocol; always read it first.
- `TRACKER.md` — the compact canonical index of every reported issue: ID,
  severity, status, short description, primary location, and disposition.
- `issues/DRIFT-NNN.md` — the permanent detailed record for one bug. It is
  created when investigation begins, then grows to contain analysis, decisions,
  fix design, commits, tests, and independent-review results.
- `audits/YYYY-MM-DD-description.md` — an immutable snapshot of a broad review:
  its scope, baseline, methods, limitations, and the IDs it reported. The
  tracker—not an audit report—remains authoritative for current status.

## Start here

First read this file and `TRACKER.md`, then follow the route matching the work
the user requested:

| Role | Required action |
|---|---|
| Bug finder | Deduplicate and add an `Untriaged` tracker row under section 1 |
| Broad auditor | Also preserve the audit under `audits/` with scope and validation evidence |
| Investigator | Select an `Untriaged` item, create its file under `issues/`, and follow section 2 |
| Implementer | Work only on a user-accepted `Fix planned` item and follow sections 3–4 |
| Independent reviewer | Review each `Awaiting review` item not implemented by that reviewer under sections 5–7 |

Do not silently change roles. Reporting or investigating a bug does not itself
authorize a production-code change, and implementing a fix does not authorize
the implementer to approve its own work.

## Non-negotiable rules

1. Every bug receives a permanent `DRIFT-NNN` ID. IDs are monotonically
   increasing, never reused, and never renumbered.
2. Search the ledger before adding a bug. Record duplicates under the existing
   item rather than creating competing IDs.
3. Keep independent root causes separate. If one report contains two fixes
   that can succeed or fail independently, split it into two IDs.
4. Discovery and analysis do not authorize implementation. Do not change
   production code until the item is accepted and marked `Fix planned`.
5. Normally only one item may be `Investigating` or `Fixing` at a time.
6. Each accepted bug is fixed in isolated commits carrying its audit ID. Do not
   mix unrelated cleanup, formatting, refactoring, or another bug into them.
7. Add regression coverage that would fail against the pre-fix behavior when
   practical. Explain explicitly when that is not practical.
8. Tests passing is not independent approval. A different reviewer must inspect
   the actual patch, original failure mode, compatibility, and residual risk.
9. Never amend a commit after it has been independently reviewed. Address
   requested changes in follow-up commits with the same audit ID.
10. Keep `TRACKER.md` compact. Never put long-form evidence, analysis, design,
    or history there; put it in the bug's `issues/DRIFT-NNN.md` file.
11. Keep the tracker and individual bug files accurate as part of the work; do
    not rely on conversation history as the only record.
12. Attribute implementation to every human or AI system that changes
    production code or regression tests for the item. Never replace an earlier
    implementer when another contributor makes a follow-up change.
13. An implementer cannot independently approve that item. It must be reviewed
    by an identity absent from the item's `Implemented by` field.

## Attribution

Use the stable name the user would use when assigning work, such as `Codex`,
`Claude`, or `Human: Spencer`; do not write only `AI` or `assistant`. A model or
tool version can be added in the individual issue file when useful, but the
stable name must remain easy to filter.

- `Implemented by` means the identity authored or materially changed production
  code or regression tests for this bug. Discovery, analysis, review, and
  documentation-only bookkeeping do not make someone an implementer.
- `Reviewed by` records each independent reviewer and latest verdict, for
  example `Claude: changes requested` or `Codex: approved`.
- Both fields are cumulative. If Claude revises a Codex fix, both are
  implementers and neither may independently approve the final combined fix.
- Every implementation commit carries both `Audit-ID: DRIFT-NNN` and
  `Implemented-by: <stable identity>` trailers. An approval-recording commit may
  carry `Reviewed-by: <stable identity>` only after that verdict actually
  exists.

## Status model

`TRACKER.md` defines the authoritative item statuses. The usual lifecycle is:

```text
Untriaged -> Investigating -> Fix planned -> Fixing
          -> Awaiting review -> Verified
                              -> Decision needed
```

Valid exits are `Not a bug`, `Won't fix`, `Deferred`, and `Accepted risk`.
Independent review can send `Awaiting review` back to `Fixing` when feedback is
accepted or to `Decision needed` when a material finding is disputed.

## 1. Reporting a new bug

The reporting system must perform all of these steps, even when it reports many
bugs from one audit:

1. Read `TRACKER.md` and search for the same root cause, trigger, and affected
   behavior.
2. Choose the next ID by finding the highest existing numeric ID and adding
   one. Never fill gaps or reuse a closed ID.
3. Give the issue one concrete title describing the broken behavior, not a
   proposed solution.
4. Assign an initial severity:
   - `High`: credible arbitrary code execution, destructive security boundary
     failure, or comparable impact.
   - `Medium`: data loss/corruption, invisible destructive behavior, or a
     materially broken core workflow.
   - `Low`: bounded edge case, misleading feedback, compatibility problem, or
     recoverable state/UX defect.
5. Add one row to the Findings table with status `Untriaged`, the primary source
   location, and no presumed disposition.
6. Keep supporting notes outside `TRACKER.md` until the item is selected for
   investigation. If the report contains evidence that would otherwise be
   lost, create its `issues/DRIFT-NNN.md` immediately using the intake fields
   below.

For a broad audit, also create `audits/YYYY-MM-DD-description.md`. Record the
audited branch and commit, scope, methods, commands and results, limitations,
and every resulting `DRIFT-NNN` ID. Treat it as a dated snapshot: do not update
old audit conclusions merely because an item's current status later changes.

Required intake record when an individual bug file is created:

```text
### DRIFT-NNN — Short title

**Current status:** `Untriaged`
**Reported:** YYYY-MM-DD; reporter/tool if useful
**Initial severity:** High | Medium | Low
**Primary locations:** file and line/function
**Implemented by:** —
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

**Trigger:** Minimal conditions that expose the behavior.
**Impact:** What the user, filesystem, process, or data experiences.
**Evidence:** Direct control flow, reproduction, diagnostics, or authoritative
platform behavior supporting the report.
**Open questions:** Facts that triage still needs to establish.
```

Do not inflate the ledger with speculative possibilities. If evidence is weak,
say so in the record and identify the validation needed.

## 2. Investigating and deciding an item

Before editing code:

1. Create `issues/DRIFT-NNN.md` from the intake template if it does not exist,
   mark the tracker row and file `Investigating`, and add a decision-history
   entry.
2. Reproduce the behavior when safe and practical. Otherwise establish it from
   exact control flow and authoritative platform semantics.
3. Decide whether it violates Drift's intended contract rather than merely
   reflecting a documented limitation or deliberate tradeoff.
4. Reassess severity and record any change.
5. Document viable fixes, compatibility costs, rejected alternatives, and the
   recommended approach in the bug's individual file.
6. Define acceptance criteria, regression tests, non-goals, and residual risks
   before implementation.
7. Obtain the user's disposition: fix, defer, won't fix, or not a bug.

Only an accepted approach moves to `Fix planned`. A rejected or deferred item
keeps its record and ID permanently so it is not rediscovered without context.

## 3. Implementing an accepted fix

1. Change the tracker and issue-file status from `Fix planned` to `Fixing` and
   log it.
2. Add the implementer's stable identity to `Implemented by` when that identity
   first changes production code or regression tests. Implement only that item
   and its tests.
3. Run the issue file's focused tests plus the full relevant regression suite.
4. Complete the existing `issues/DRIFT-NNN.md` using the review requirements
   below.
5. Change the ledger status to `Awaiting review`.
6. Commit the production change, regression tests, updated bug file, and that
   item's tracker update together as one isolated fix commit.

Commit format:

```text
Fix DRIFT-NNN: concise behavior description

Problem:
<trigger, impact, and why the old behavior was wrong>

Implementation:
<what changed and why this design was chosen>

Tests:
<commands and coverage, including pre-fix failure where established>

Residual risk:
<remaining limitations or "None identified">

Audit-ID: DRIFT-NNN
Implemented-by: Codex
```

Every follow-up that changes the same fix must also carry
`Audit-ID: DRIFT-NNN` and its actual implementer's `Implemented-by` trailer. Do
not use `Reviewed-by` or similar approval trailers until that review actually
occurred.

## 4. Required review record

When a fix reaches `Awaiting review`, its `issues/DRIFT-NNN.md` must be
sufficient for an independent reviewer who has not seen the original
conversation. It must contain:

1. Current tracker status, bug ID, all implementers, all reviewers, and the
   current decision owner.
2. Original behavior, minimal trigger, impact, and final severity.
3. Relevant production and test locations.
4. The agreed design and why alternatives were rejected.
5. Security, data-integrity, compatibility, and error-path considerations.
6. Explicit acceptance criteria, each independently pass/fail reviewable.
7. Tests the implementer ran and the expected results.
8. Instructions for any safe manual or integration validation.
9. Known non-goals, remaining limitations, and residual risk.
10. Instructions to locate all commits by the `Audit-ID` trailer.
11. A chronological review-round section containing reviewer identity, commit
    set, verdict, findings, responses, and resolution.

The bug file is an implementation claim to audit, not evidence that the claim
is correct.

## 5. Independent reviewer workflow

1. Read this file, then `TRACKER.md`.
2. Identify yourself using a stable attribution name. Review every tracker item
   marked `Awaiting review` whose `Implemented by` field does not contain that
   identity, unless the user narrows the scope. Disclose and skip any item you
   implemented.
3. Read the item's `issues/DRIFT-NNN.md` file.
4. Locate every commit carrying that audit ID. Do not assume the newest commit
   alone contains the entire fix.
5. Inspect the actual commits and surrounding production code.
6. Run every required test and add safe, read-only checks where useful.
7. Verify that regression coverage exercises the production behavior and would
   detect reintroduction of the bug.
8. Do not edit code, commits, issue records, or statuses unless the user explicitly
   asks. Report the verdict in the conversation.

Useful commands:

```text
git log --all --reverse --format="%H %s" --grep="Audit-ID: DRIFT-NNN"
git show --stat --patch <commit>
```

Required review output for each item:

1. **Reviewer, audit ID, and verdict:** stable reviewer identity plus
   `Approved`, `Approved with residual risk`, or `Changes requested`.
2. **Findings:** severity-ranked with exact file and line references. Say
   explicitly when there are no findings.
3. **Acceptance criteria:** pass/fail for every issue-record criterion.
4. **Tests:** commands run and results.
5. **Residual risk and coverage gaps:** anything not proven.
6. **Scope check:** whether the commits contain unrelated changes.

A review is not an approval merely because tests pass. Re-evaluate the original
failure mode and the chosen design independently.

## 6. Recording review results

After the reviewer reports in the conversation, the implementing system or
maintainer updates the files; the reviewer does not need to do so.

- `Approved`: set the tracker item and bug file to `Verified`, and add the
  reviewer identity, verdict, commit set, and test evidence to the bug file's
  review history and tracker `Reviewed by` field.
- `Approved with residual risk`: do the same, but preserve the stated residual
  risk in the bug file.
- `Changes requested`: record the reviewer and verdict. If every material
  finding is accepted, return the item to `Fixing`. If any material finding is
  disputed, use `Decision needed` and follow section 7.

## 7. Disagreement and final decisions

Do not resolve a disagreement by vote, model confidence, seniority claims, or
which system spoke last. Preserve both positions and decide from reproducible
evidence, the accepted product contract, and an explicitly named authority.

1. In the issue file, record each disputed finding with the reviewer's claim,
   severity, evidence, affected acceptance criterion, and requested change.
2. Record the implementer's response as `Accept` or `Dispute`, with concrete
   evidence. The implementer may not erase or paraphrase away the original
   finding.
3. Set both the tracker and issue file to `Decision needed`. No disputed item
   may be marked `Verified` while this state remains.
4. For an objective technical dispute, the user may appoint a neutral
   adjudicator who is neither an implementer nor the original reviewer. For
   intended behavior, scope, compatibility, or risk tolerance, the user is the
   final authority unless the user explicitly delegates that decision.
5. Record the final ruling in the issue file with date, decision-maker identity,
   evidence considered, rationale, and required action. Never record only
   “user decided” or “reviewer won.”
6. Apply the ruling:
   - Accepted review finding: return to `Fixing`, then obtain a fresh independent
     review of the complete fix.
   - Rejected review finding: return to `Awaiting review` under the recorded
     ruling; the same reviewer or another eligible reviewer must still approve.
   - Separate valid concern: create a new tracker ID and cross-link both issues.
   - Deliberately unresolved risk: use `Accepted risk`, not `Verified`, and
     preserve the user's rationale.

`Verified` always means an eligible independent reviewer approved the complete
implemented commit set. The user retains final product authority, while the
record makes any decision to accept rather than verify risk explicit.
