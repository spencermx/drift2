# Drift bug tracker

This is Drift's permanent, living bug ledger. It was initialized from the
comprehensive review completed on 2026-07-25 and must also receive every future
bug report, regardless of who or what system discovers it. Each item has a
stable ID so discussion, fixes, tests, and commits can refer to it without
depending on line numbers that may move.

`README.md` is the canonical protocol for adding new reports, deduplicating and
assigning IDs, investigating issues, implementing isolated fixes, creating
review-ready issue records, and recording independent verdicts. Do not add or change an
item without following that workflow.

The review reported 29 rows. One row contained two independent quick-add
issues, so they are tracked separately here for a total of 30 items. IDs past
DRIFT-030 come from later reports and reviews rather than that initial audit.

## Workflow

Only one item should normally be `Investigating` or `Fixing` at a time.

| Status | Meaning |
|---|---|
| `Untriaged` | Reported but not yet analyzed together |
| `Investigating` | Reproducing, validating impact, and considering options |
| `Fix planned` | Agreed bug and approach; implementation has not started |
| `Fixing` | Code and regression test are in progress |
| `Awaiting review` | Fix and tests pass; an eligible independent reviewer should inspect it |
| `Decision needed` | Reviewer and implementer materially disagree; the issue awaits adjudication |
| `Verified` | Independent review approved the fix and required tests pass |
| `Accepted risk` | The user deliberately closed an unresolved risk without calling it verified |
| `Won't fix` | Intentionally accepted; rationale recorded |
| `Not a bug` | Rejected after analysis; rationale recorded |
| `Deferred` | Valid issue deliberately postponed |

`TRACKER.md` is intentionally an index, not the home for long-form issue
records. Once investigation starts, all evidence, options, decisions, tests,
commit references, and review results belong in that bug's
`issues/DRIFT-NNN.md`.

`Implemented by` names every identity that changed production code or
regression tests for the item. `Reviewed by` names reviewers and their latest
verdicts. These columns are cumulative and use stable, filterable names such as
`Codex` and `Claude`; see `README.md` for attribution and conflict rules.

## Independent review handoff

This folder is a self-contained handoff for Claude or another independent
reviewer. Its `README.md` defines how to discover ready fixes,
locate all commits associated with an audit ID, validate each issue record, run the
required tests, and report a verdict. The user should only need to say:

> Review every bug fix awaiting independent review in the `quality/` folder.

Every investigated item gets an `issues/DRIFT-NNN.md` file. By the time a fix
reaches `Awaiting review`, that file records the original behavior, agreed
design, acceptance criteria, tests, non-goals, and residual risks. Every fix or
review-follow-up commit carries an `Audit-ID: DRIFT-NNN` trailer so the reviewer
can discover the complete commit set without the user supplying hashes.

## Findings

| ID | Severity | Status | Finding | Primary location | Implemented by | Reviewed by | Decision / fix |
|---|---|---|---|---|---|---|---|
| [DRIFT-001](issues/DRIFT-001.md) | High | Verified | Claude launch can execute planted `cmd.exe` or `claude.*` | `drift.c:1163`, `drift.c:1269` | Codex | Claude: approved with residual risk | Fixed in `a9e75bd`; residual risk carried to DRIFT-031 |
| DRIFT-002 | High | Untriaged | Vim lookup can execute a planted `vim.exe` | `drift.c:375`, `drift.c:2993` | — | — | — |
| DRIFT-003 | Medium | Untriaged | Name metadata can be replaced by a truncated file | `drift.c:998` | — | — | — |
| DRIFT-004 | Medium | Untriaged | Settings JSON scanner can edit the wrong object | `drift.c:1908`, `drift.c:2156` | — | — | — |
| DRIFT-005 | Medium | Untriaged | Concurrent workspace member changes can be overwritten | `drift.c:1965`, `drift.c:2075` | — | — | — |
| DRIFT-006 | Medium | Untriaged | Relative `additionalDirectories` resolve against the wrong location | `drift.c:1965`, `drift.c:2221` | — | — | — |
| DRIFT-007 | Medium | Untriaged | Session-delete confirmation remains armed invisibly after resize | `drift.c:1632` | — | — | — |
| DRIFT-008 | Medium | Untriaged | Workspace rename can truncate an unchanged long name | `drift.c:1523` | — | — | — |
| DRIFT-009 | Medium | Untriaged | Session rename can persist malformed UTF-8 | `drift.c:1403` | — | — | — |
| DRIFT-010 | Medium | Untriaged | The 256-session limit can omit the newest sessions | `drift.c:1218` | — | — | — |
| DRIFT-011 | Medium | Untriaged | Documented bare `drift` invocation bypasses `drift.bat` | `README.md:29`, `drift.bat:1` | — | — | — |
| DRIFT-012 | Medium | Untriaged | Default-application launches use the wrong working directory | `drift.c:3027` | — | — | — |
| DRIFT-013 | Medium | Untriaged | Last-directory handoff is stale and cross-instance unsafe | `drift.c:4033`, `drift.bat:2` | — | — | — |
| DRIFT-014 | Low | Untriaged | Quick-add remains invisibly active after resize | `drift.c:2379` | — | — | — |
| DRIFT-015 | Low | Untriaged | Session discovery commits to the first merely existing path encoding | `drift.c:1196` | — | — | — |
| DRIFT-016 | Low | Untriaged | Failed session scans can be cached as valid | `drift.c:1182` | — | — | — |
| DRIFT-017 | Low | Untriaged | Workspace preview decodes session titles as ANSI | `drift.c:2520` | — | — | — |
| DRIFT-018 | Low | Untriaged | Transcript-title truncation can split UTF-8 | `drift.c:1743` | — | — | — |
| DRIFT-019 | Low | Untriaged | Name-entry fields reject all non-ASCII input | `drift.c:1487`, `drift.c:3713` | — | — | — |
| DRIFT-020 | Low | Untriaged | Shrinking a name popup leaves an invisible accepted suffix | `drift.c:1424` | — | — | — |
| DRIFT-021 | Low | Untriaged | Manifest-focused input ignores key modifiers | `drift.c:2732` | — | — | — |
| DRIFT-022 | Low | Untriaged | Quick-add exposes only the first nine workspaces | `drift.c:2367` | — | — | — |
| DRIFT-023 | Low | Untriaged | Quick-add success banner is immediately overwritten | `drift.c:2451` | — | — | — |
| DRIFT-024 | Low | Untriaged | Wine handoff loses browser and session context | `run.sh:50` | — | — | — |
| DRIFT-025 | Low | Untriaged | Partial delete retains marks for already recycled sources | `drift.c:3445` | — | — | — |
| DRIFT-026 | Low | Untriaged | Failed navigation is rendered as an empty directory | `drift.c:3182`, `drift.c:3248` | — | — | — |
| DRIFT-027 | Low | Untriaged | Workspace directory helpers report success after creation failure | `drift.c:959` | — | — | — |
| DRIFT-028 | Low | Untriaged | Create accepts trailing periods/spaces that Win32 normalizes | `drift.c:3781` | — | — | — |
| DRIFT-029 | Low | Untriaged | Path identity is case- and separator-sensitive | `drift.c:3335`, `drift.c:4210` | — | — | — |
| DRIFT-030 | Low | Untriaged | Wine launch-file short writes are treated as success | `drift.c:1103` | — | — | — |
| [DRIFT-031](issues/DRIFT-031.md) | Low | Untriaged | A `.cmd` launcher shim resolves bare commands from the workspace anchor | `drift.c:1250`, `drift.c:1306` | — | — | Found reviewing DRIFT-001; residual of the same threat class |
| [DRIFT-032](issues/DRIFT-032.md) | Low | Untriaged | Failure to resolve a safe Claude launcher produces no visible error | `drift.c:1294`, `drift.c:2822` | — | — | Found reviewing DRIFT-001; preserved as an independent UX regression |
