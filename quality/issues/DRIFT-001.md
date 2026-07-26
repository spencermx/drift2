# DRIFT-001 — Claude executable-search hijack

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Verified`
**Reported:** 2026-07-25
**Initial severity:** High
**Final severity:** High
**Primary locations:** `drift.c:1163`, `drift.c:1217`, `drift.c:1269`
**Implemented by:** Codex
**Reviewed by:** Claude: approved with residual risk
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

## Approved and implemented design

Option 3 was implemented. It removes both implicit current-directory searches
without changing Claude's inherited command-resolution behavior:

1. `ResolveClaudeLauncherFromPath` parses `PATH` in directory order, including
   quoted entries containing semicolons. It ignores empty, drive-relative,
   root-relative, and other relative entries.
2. Each accepted directory is probed for `claude.exe` and then `claude.cmd`.
   The selected result carries both an absolute path and a launcher type.
3. A native `.exe` is passed directly as `CreateProcess`'s non-null application
   name. It does not pass through a command processor.
4. An npm-style `.cmd` is passed to the absolute command processor returned by
   `GetSystemDirectory`, with `/d /v:off /s /c`. This disables Cmd AutoRun and
   delayed expansion while preserving batch-shim support.
5. A `.cmd` path containing `%` is rejected because Cmd expands percent
   expressions even inside quotes. The launcher path can never be silently
   transformed into another command.
6. The workspace anchor remains only the child working directory; it is not an
   implicit executable-search directory.

## Production and test locations

- `drift.c:152` — typed launcher and process-spec records.
- `drift.c:1110` — absolute-path classification and directory probing.
- `drift.c:1163` — safe `PATH` parsing and resolution.
- `drift.c:1217` — direct `.exe` and trusted `.cmd` process construction.
- `drift.c:1269` — `LaunchClaudeIn` integration.
- `tests/claude_launcher_test.c:1` — production-linked resolver and real
  child-process regression coverage.
- `tests/run_tests.bat:65` — Windows regression-suite integration.

## Acceptance criteria and implementer evidence

| Criterion | Implementer evidence |
|---|---|
| A planted `cmd.exe` in Drift's launch directory is never selected | The `.cmd` process specification equals System32 `cmd.exe` and differs from the planted fixture; the real child launch passes |
| Planted `claude.exe` or `claude.cmd` files in the workspace are ignored | Empty and relative `PATH` tests run with planted current-directory files and resolve nothing |
| An explicitly absolute workspace entry may be selected | The explicit-current-directory test resolves its sentinel `.exe` |
| Absolute `PATH` directory order is preserved across launcher types | An earlier `.cmd` wins over a later `.exe`; reversing the directories reverses the result |
| Empty and relative `PATH` entries are ignored | Empty, `.`, and named relative entries all fail before a later absolute match is considered |
| Spaces and quoted semicolons are supported | Resolver tests cover both; a real `.cmd` under a path containing spaces, `;`, and `&` launches successfully |
| Native `.exe` and npm-style `.cmd` launchers both work | Real child processes receive the workspace cwd, `--resume`, and the session ID through both paths |
| Unsafe shell inputs fail closed | Invalid session IDs and percent-expandable `.cmd` paths are rejected before process creation |
| Wine handoff behavior is unchanged | Resolution occurs only after the existing `DRIFT_LAUNCH_FILE` early-return branch |

The old implementation would fail the first two criteria because it passed a
null application name and bare `cmd`/`claude` tokens. The new test includes the
production translation unit, so a complete reversion also fails to build rather
than silently dropping the security checks.

## Compatibility check

The current development machine resolves Claude to
`C:\Users\spencer\.local\bin\claude.exe`. A trusted System32 Cmd invocation
with current-directory search disabled also launched the installed Claude
successfully, confirming that a secure lookup is compatible with the current
installation. The stronger explicit resolver remains preferred to avoid
changing the environment inherited by Claude.

Compatibility is intentionally limited to the two launcher forms Drift already
documented: `claude.exe` and `claude.cmd`. `.com` and `.bat` are not searched.
Candidates longer than the application's existing `MAX_PATH` limit are ignored.
A `.cmd` installation in a directory containing a literal `%` is rejected;
moving it to an ordinary absolute `PATH` directory or using the native `.exe`
restores launch support.

## Security, data-integrity, and error paths

- The fix does not read or write workspace, session, or Claude configuration
  data.
- Absolute directories explicitly placed on `PATH` remain trusted executable
  sources. Protecting those directories from replacement is an operating-system
  and installation concern outside this item.
- Resolution and process creation are separate calls, so an attacker already
  able to replace a file inside a trusted `PATH` directory retains a normal
  time-of-check/time-of-use opportunity. The removed threat requires only a
  planted file in an untrusted launch or workspace directory.
- If no safe launcher is found, process construction fails, or CreateProcess
  fails, Drift restores or retains its TUI instead of falling back to unsafe
  lookup. The existing lack of a detailed launch-error message is unchanged.

## Validation performed

| Command | Result |
|---|---|
| `cmd /c tests\run_tests.bat` | Passed all four stages: row-guard lint, existing ASan suite, 17 secure-launcher tests including real `.exe`/`.cmd` child launches, and `/W4 /WX` application compilation |
| `cmd /c build.bat` | Passed; optimized `drift.exe` built successfully |
| `cl /W4 /wd4459 /analyze /c drift.c` after `vcvars64.bat` | Passed with only the established 32.9 KB stack-frame warning and two pre-existing shadowing warnings |

No live Claude session was started during automated validation, avoiding
changes to real Claude session data. A reviewer may optionally launch a new and
resumed session from `drift -c` after confirming that such integration activity
is acceptable.

## Reviewer instructions

The eligible reviewer must not be Codex. Locate the complete implementation
commit set with:

```text
git log --all --reverse --format="%H %s" --grep="Audit-ID: DRIFT-001"
git show --stat --patch <commit>
```

Re-run `cmd /c tests\run_tests.bat`, inspect every acceptance criterion above,
and verify that `LaunchClaudeIn` supplies non-null absolute application names to
`CreateProcess`. Report `Approved`, `Approved with residual risk`, or `Changes
requested` using the format in `quality/README.md`.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | `Untriaged` | `Investigating` | Validated executable-search attack paths and selected a compatible secure launch design for user consideration. |
| 2026-07-25 | User | `Investigating` | `Fix planned` | Approved the explicit absolute launcher-resolution approach and authorized implementation. |
| 2026-07-25 | Codex | `Fix planned` | `Fixing` | Began the approved production change and regression coverage. |
| 2026-07-25 | Codex | `Fixing` | `Awaiting review` | Implemented absolute launcher resolution, passed focused and full validation, and prepared the independent-review handoff. |
| 2026-07-25 | Claude | `Awaiting review` | `Verified` | Independent review approved the complete commit set with residual risk; opened DRIFT-031 for a separate concern found during review. |

## Review history

### Round 1 — Claude, 2026-07-25

- **Reviewer:** `Claude` (Opus 5). Not present in `Implemented by`, so eligible.
- **Commit set:** `a9e75bd` "Fix DRIFT-001: resolve Claude launcher safely" —
  the only commit carrying the `Audit-ID: DRIFT-001` trailer. Confirmed with
  `git log --all --grep="Audit-ID: DRIFT-001"`; no follow-ups exist.
- **Verdict:** `Approved with residual risk`.

**Acceptance criteria:** all nine pass. Criteria 1 and 2 pass at the unit level
plus code inspection of the wiring at `drift.c:1306-1307`, which supplies a
non-null absolute `lpApplicationName`; see finding 3 for why no automated test
covers that step. Criterion 9 verified by inspection: `drift.c:1270-1290` is
unchanged and its early return precedes launcher resolution.

**Tests run by the reviewer:** `cmd /c tests\run_tests.bat` — ALL CHECKS PASSED,
4/4 stages, 17/17 launcher tests under AddressSanitizer, plus the `/W4 /WX`
compile. A `'vswhere.exe' is not recognized` line precedes stage 1;
`run_tests.bat:14-23` is untouched by this commit, `cl` was still located, and
every stage ran, so this is pre-existing and out of scope.

Additional read-only checks: hand-traced the `PATH` tokenizer at
`drift.c:1163-1200` for termination and trim/rescan correctness across
quoted-semicolon, whitespace-only, and unterminated-quote entries — `start`
always advances past a separator, so it terminates; confirmed every `snprintf`
path fails closed rather than truncating, including an over-long session id;
confirmed `IsSafeSessionId` (`drift.c:1067-1075`) admits only hex digits and
`-`, excluding `%`, `&`, and `"`; confirmed `IsPathSlash` duplicates no existing
helper.

**Findings — three, all Low, none blocking:**

1. *The workspace anchor remains an implicit executable-search directory on the
   `.cmd` path.* `BuildClaudeProcessSpec` (`drift.c:1250-1256`) launches shims
   through `cmd.exe`, and `LaunchClaudeIn` (`drift.c:1306-1307`) passes `anchor`
   as the child working directory. Cmd's command search includes its current
   directory, so any bare command name the shim itself invokes resolves from the
   untrusted anchor first. Reproduced with the exact command line this code
   generates: a `claude.cmd` containing a bare `driftprobe` executed a planted
   `driftprobe.cmd` from the anchor. This makes design point 6 above accurate for
   Drift's own resolution but overstated for the command processor Drift starts.
   Tracked separately as DRIFT-031; see that file for evidence and a validation
   warning about a false negative.
2. *New silent-failure mode when no launcher resolves.* `drift.c:1294-1297`
   returns 1 before any console switch, and the callers at `drift.c:2822`,
   `drift.c:2824`, and `drift.c:2891` return that straight into the main loop
   with no message. Before this fix, `cmd /c claude` always started and cmd
   itself printed `'claude' is not recognized...` into the original screen
   buffer, leaving a diagnostic in scrollback. The statement under "Security,
   data-integrity, and error paths" that the launch-error message is unchanged is
   therefore not quite right: Drift never had its own message, but the
   cmd-supplied one is gone. A user whose Claude is a `.bat`, or reachable only
   through a relative `PATH` entry, now sees Enter and N do nothing at all.
   Impact is bounded because the native installer produces `claude.exe` and npm
   produces `claude.cmd`, both covered.
3. *No regression coverage for the `LaunchClaudeIn` wiring itself.* All 17 tests
   call `ResolveClaudeLauncherFromPath` and `BuildClaudeProcessSpec` directly.
   Restoring `CreateProcess(NULL, "cmd /c claude ...")` at `drift.c:1306` while
   leaving the resolver in place would keep every test green. The mitigation
   claimed under "Acceptance criteria and implementer evidence" — that a
   reversion fails to build — holds only for a reversion that also deletes the
   resolver. A true end-to-end test would need to intercept `CreateProcess`, so
   the gap is reasonable; it is recorded here rather than treated as a defect.

**Scope check:** clean. Five hunks in `drift.c` — header comment, type
declarations, forward declarations, the new resolver block, and `LaunchClaudeIn`
— all belonging to DRIFT-001. The `TRACKER.md` change is the single-row status
update. No unrelated cleanup, formatting, or refactoring.

**Reviewer-recorded residual risk**, preserved here under section 6 rather than
merged into the implementer's section: findings 1 and 3 above, which the
implementer record did not state. Finding 1 is the material one — the fix is a
strict improvement over the prior behavior and closes both reported lookups, but
it does not make the anchor inert for commands a `.cmd` shim invokes.

**Resolution:** approved without requested changes. No finding requires a change
to this commit set, so the item closes as `Verified` with the residual risk
recorded. Finding 1 is carried forward as DRIFT-031 per section 7's
separate-valid-concern rule; findings 2 and 3 are accepted as recorded risk. No
implementer response is required. Per section 6, this round was recorded by the
reviewer because no implementer was present in the session; no
implementer-authored section of this file was modified.
