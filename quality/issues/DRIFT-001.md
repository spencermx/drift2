# DRIFT-001 — Claude executable-search hijack

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Fix planned`
**Reported:** 2026-07-25
**Initial severity:** High
**Primary location:** `drift.c:1122`
**Implemented by:** —
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

## Trigger and impact

There are two separate executable lookups in the native Windows launch path:

1. `CreateProcess(NULL, "cmd /c claude ...", ...)` asks Windows to locate
   `cmd.exe`. With a null application name, the application directory and
   Drift's process current directory are searched before System32. Drift never
   changes its process current directory, so launching Drift from an untrusted
   repository containing `cmd.exe` is sufficient to execute that file when a
   Claude session is started or resumed.
2. After the real command processor starts with the workspace anchor as its
   working directory, bare `claude` is resolved by command-search rules that
   normally include the current directory before `PATH`. A planted
   `claude.exe` or `claude.cmd` in the anchor can therefore replace the intended
   installation.

## Validation and scope

Confirmed. This is within Drift's normal threat model and should be fixed.

The session ID is not an injection path here: it is restricted to hexadecimal
digits and hyphens and is quoted. The Wine path is also separate; when
`DRIFT_LAUNCH_FILE` is set, Drift writes a handoff request and never enters the
native `CreateProcess` branch.

## Options considered

1. Resolve only System32 `cmd.exe`. Small, but leaves the anchor-side `claude`
   lookup vulnerable.
2. Resolve System32 `cmd.exe` and set
   `NoDefaultCurrentDirectoryInExePath` in the child. This preserves Cmd's
   native `PATH` and extension behavior, but the variable is inherited by
   Claude and can change how Claude's own child commands resolve project-local
   executables.
3. Resolve `claude.exe` or `claude.cmd` from explicit absolute `PATH` entries,
   excluding empty and relative entries. Launch an `.exe` directly; launch an
   absolute `.cmd` through an explicitly resolved System32 `cmd.exe /d`.

## Recommended approach

Option 3. It removes both implicit current-directory searches without changing
Claude's inherited command-resolution behavior. The resolver should preserve
directory order, support the two documented launcher forms, reject relative or
empty `PATH` entries, and return a typed absolute result (`.exe` or `.cmd`).
`GetSystemDirectory` should supply the command processor; `COMSPEC` should not
be trusted because it is an environment variable.

## Proposed regression coverage

- A launch directory containing a sentinel `cmd.exe` must never be selected.
- An anchor containing sentinel `claude.exe` and `claude.cmd` files must never
  be selected unless that exact absolute directory is explicitly on `PATH`.
- An earlier `PATH` directory containing `claude.cmd` must win over a later
  directory containing `claude.exe`, preserving directory-order semantics.
- Empty and relative `PATH` components must be ignored.
- Absolute paths containing spaces must launch correctly.
- Both a native `.exe` and an npm-style `.cmd` launcher must work.

## Compatibility check

The current development machine resolves Claude to
`C:\Users\spencer\.local\bin\claude.exe`. A trusted System32 Cmd invocation
with current-directory search disabled also launched the installed Claude
successfully, confirming that a secure lookup is compatible with the current
installation. The stronger explicit resolver remains preferred to avoid
changing the environment inherited by Claude.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | `Untriaged` | `Investigating` | Validated executable-search attack paths and selected a compatible secure launch design for user consideration. |
| 2026-07-25 | User | `Investigating` | `Fix planned` | Approved the explicit absolute launcher-resolution approach and authorized implementation. |

## Review history

None. This item has not been implemented and is not ready for independent
review.
