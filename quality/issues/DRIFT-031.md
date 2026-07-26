# DRIFT-031 — A `.cmd` launcher shim resolves bare commands from the workspace anchor

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Untriaged`
**Reported:** 2026-07-25; Claude, during independent review of
[DRIFT-001](DRIFT-001.md)
**Initial severity:** Low
**Primary locations:** `drift.c:BuildClaudeProcessSpec` (`.cmd` branch),
`drift.c:LaunchClaudeIn` (`anchor` passed as the child working directory)
**Implemented by:** —
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

**Trigger:** Claude is installed as an npm-style `claude.cmd` shim rather than a
native `claude.exe`, and the user launches or resumes a session in a workspace
anchor whose contents are untrusted.

**Impact:** DRIFT-001 removed Drift's own implicit current-directory lookups,
but the `.cmd` branch necessarily starts a command processor, and that processor
runs with the anchor as its working directory. Cmd's command search includes its
current directory ahead of `PATH`, so any bare command name the shim itself
invokes can be satisfied by a planted executable in the anchor. npm's generated
shims are the motivating case: they probe for an interpreter next to the shim and
otherwise fall back to a bare command name.

This is a narrower residual of the DRIFT-001 threat class rather than a
regression. It matches the exposure a user already has when running the same
launcher from a shell opened in that directory, and it does not affect the
resolved-`.exe` path, which never involves a command processor.

**Evidence:** Reproduced with the exact command line `BuildClaudeProcessSpec`
generates for a `.cmd` launcher — `cmd.exe /d /v:off /s /c "" <shim> " --resume
" <id> ""` with the child working directory set to the anchor. A shim containing
a bare `driftprobe` executed a planted `driftprobe.cmd` from the anchor rather
than failing to resolve. A direct control (`cmd /d /c "cd /d <anchor> &&
driftprobe"`) behaves identically, confirming the cause is ordinary cmd command
search and not something specific to the constructed command line.

> **Validation warning — this reproduces as a false negative under some agent
> harnesses.** Claude Code sets `NoDefaultCurrentDirectoryInExePath=1` in its
> shell environment, which suppresses exactly the current-directory search this
> item is about. The first attempt at the reproduction above appeared to show
> the vector already closed. Clear that variable before concluding anything
> about this item, and confirm the control case resolves the planted file.

Option 2 in DRIFT-001's rejected alternatives — setting
`NoDefaultCurrentDirectoryInExePath` for the child — would close this, and was
rejected there because Claude inherits the variable and it would change how
Claude's own child commands resolve project-local executables. That tradeoff is
the substance of this item and should be re-decided on its own terms rather than
re-litigated as part of DRIFT-001.

**Open questions:**

- Does the currently shipped npm shim for Claude actually reach a bare-command
  fallback, or does it always resolve an interpreter beside itself? Not
  determinable on the development machine, which has no npm or node installed
  and resolves Claude to a native `C:\Users\spencer\.local\bin\claude.exe`.
- Is the `.cmd` branch worth keeping at all if native installation is the
  supported path, or would restricting Drift to `claude.exe` remove this class
  of risk more cleanly than any launch-time mitigation?
- If a mitigation is wanted, is the inherited-variable cost of
  `NoDefaultCurrentDirectoryInExePath` acceptable, given it changes behavior for
  every command Claude subsequently runs?

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Claude | — | `Untriaged` | Reported from independent review of DRIFT-001 as a separate valid concern under section 7; evidence preserved because the reproduction is easy to get wrong. |
