# DRIFT-032 — Failure to resolve a safe Claude launcher produces no visible error

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Untriaged`
**Reported:** 2026-07-25; Claude, during independent review of
[DRIFT-001](DRIFT-001.md); assigned a permanent ID by Codex
**Initial severity:** Low
**Primary locations:** `drift.c:LaunchClaudeIn` (resolution failure),
`drift.c:HandleInput` (callers returning directly to the main loop)
**Implemented by:** —
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

**Trigger:** The user starts or resumes a Claude session when Drift cannot
resolve an allowed `claude.exe` or `claude.cmd`. Examples include Claude not
being installed, being reachable only through an empty or relative `PATH`
entry, being installed only as an unsupported `.bat`, or having a `.cmd`
launcher in a percent-expandable path that DRIFT-001 correctly rejects.

**Impact:** Enter or N appears to do nothing. `LaunchClaudeIn` returns before
switching console buffers, and its callers return directly to the main loop
without displaying an error. Before DRIFT-001, `cmd /c claude` started Cmd even
when Claude could not be resolved, so Cmd printed a command-not-found diagnostic
into the original console buffer. Drift did not previously own that message,
but the user still received visible failure feedback that is now absent.

This is an independently fixable UX regression introduced while closing the
unsafe executable searches. It does not invalidate DRIFT-001's security fix and
must not be addressed by falling back to bare `cmd` or `claude` lookup.

**Evidence:** Claude identified the control flow during independent review of
DRIFT-001 and recorded it in that issue's Round 1 findings. In
`LaunchClaudeIn`, either resolver or process-spec failure returns `1`
immediately. The `HandleInput` launch callers propagate that return into the
normal input loop without setting a status banner or entering a notification
path.

**Open questions:**

- Should resolution and process-creation failures use a transient status banner
  or a blocking notification that remains visible until acknowledged?
- Should the message distinguish “Claude not found” from a launcher that was
  found but rejected or failed to start?
- Can focused tests validate the visible TUI state without coupling them to the
  exact wording of the message?

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Assigned a permanent ID to Claude's independent DRIFT-001 review finding because the silent failure is a separate, independently fixable behavior. |
