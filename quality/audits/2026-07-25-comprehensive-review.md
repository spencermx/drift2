# Comprehensive application review — 2026-07-25

**Discovery status:** Complete
**Audited branch:** `codex/future-work`
**Audited commit:** `d7eeb60ecec86d80a2061c56a8944339f2011e09`
**Production fixes included:** None
**Current status authority:** [`../TRACKER.md`](../TRACKER.md)

This file preserves the review as it existed when discovery completed. It is a
historical audit record, not a live task list. Current severity, disposition,
implementation, and verification status must be read from `TRACKER.md` and the
corresponding file under `../issues/`.

## Scope and method

The review covered the complete Drift repository, with separate passes over:

- Windows process launching, executable resolution, and shell handoffs.
- Filesystem mutation, path identity, metadata, deletion, and failure handling.
- Workspace and session discovery, JSON editing, and concurrent state changes.
- Rendering, resizing, keyboard input, popup state, and visible feedback.
- Wine integration, wrapper scripts, and cross-platform handoff behavior.
- Existing regression tests, compiler warnings, and static analysis.

Findings were deduplicated by root cause and normalized into permanent
`DRIFT-NNN` IDs. One original review row contained two independently fixable
quick-add defects, so the tracker contains 30 issues from 29 original rows.

## Validation baseline

At the audited commit:

- `cmd /c tests\run_tests.bat` passed the row-guard lint, AddressSanitizer
  tests, and the `/W4 /WX` build.
- MSVC `/analyze` reported a roughly 32.9 KB stack-frame warning and two known
  shadowing warnings; it did not invalidate the runtime findings below.
- No production code was changed as part of discovery.

## Finding summary

The normalized tracker contains 30 findings: 2 High, 11 Medium, and 17 Low.
These severities were initial audit assessments and can change during
investigation.

### High

| ID | Finding |
|---|---|
| DRIFT-001 | Claude launch can execute planted `cmd.exe` or `claude.*` |
| DRIFT-002 | Vim lookup can execute a planted `vim.exe` |

### Medium

| ID | Finding |
|---|---|
| DRIFT-003 | Name metadata can be replaced by a truncated file |
| DRIFT-004 | Settings JSON scanner can edit the wrong object |
| DRIFT-005 | Concurrent workspace member changes can be overwritten |
| DRIFT-006 | Relative `additionalDirectories` resolve against the wrong location |
| DRIFT-007 | Session-delete confirmation remains armed invisibly after resize |
| DRIFT-008 | Workspace rename can truncate an unchanged long name |
| DRIFT-009 | Session rename can persist malformed UTF-8 |
| DRIFT-010 | The 256-session limit can omit the newest sessions |
| DRIFT-011 | Documented bare `drift` invocation bypasses `drift.bat` |
| DRIFT-012 | Default-application launches use the wrong working directory |
| DRIFT-013 | Last-directory handoff is stale and cross-instance unsafe |

### Low

| ID | Finding |
|---|---|
| DRIFT-014 | Quick-add remains invisibly active after resize |
| DRIFT-015 | Session discovery commits to the first merely existing path encoding |
| DRIFT-016 | Failed session scans can be cached as valid |
| DRIFT-017 | Workspace preview decodes session titles as ANSI |
| DRIFT-018 | Transcript-title truncation can split UTF-8 |
| DRIFT-019 | Name-entry fields reject all non-ASCII input |
| DRIFT-020 | Shrinking a name popup leaves an invisible accepted suffix |
| DRIFT-021 | Manifest-focused input ignores key modifiers |
| DRIFT-022 | Quick-add exposes only the first nine workspaces |
| DRIFT-023 | Quick-add success banner is immediately overwritten |
| DRIFT-024 | Wine handoff loses browser and session context |
| DRIFT-025 | Partial delete retains marks for already recycled sources |
| DRIFT-026 | Failed navigation is rendered as an empty directory |
| DRIFT-027 | Workspace directory helpers report success after creation failure |
| DRIFT-028 | Create accepts trailing periods/spaces that Win32 normalizes |
| DRIFT-029 | Path identity is case- and separator-sensitive |
| DRIFT-030 | Wine launch-file short writes are treated as success |

## Handoff state

At the end of the audit, DRIFT-001 had entered investigation and all other
items remained `Untriaged`. No finding had been authorized for implementation.
See the tracker for current state and `../issues/DRIFT-001.md` for the first
investigation record.

## Audit limitations

- Passing tests establish the baseline but do not disprove path-, concurrency-,
  encoding-, or UI-state defects not represented in those tests.
- Findings based on Windows API semantics still require item-specific
  regression coverage before a fix can be considered verified.
- Initial severity is not a final disposition. Each item must be investigated
  against Drift's intended behavior before production code changes begin.
