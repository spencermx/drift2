# DRIFT-006 — Relative additionalDirectories resolve against the wrong location

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Awaiting review`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Final severity:** Medium
**Primary locations:** `drift.c:ResolveMemberPath`, `drift.c:LoadMembersFrom`,
`drift.c:FindMember`, `drift.c:ApplyMemberChange`, `drift.c:JumpToMemberAt`,
`tests/membership_path_test.c`, `tests/run_tests.bat`
**Implemented by:** Codex
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

## Trigger and impact

A workspace's `.claude\settings.json` contains a supported ordinary relative
entry such as `..\shared` or `../shared` in root
`permissions.additionalDirectories`. The user opens Drift's workspace editor
and browses to the directory that entry denotes, or focuses the manifest and
presses Enter on the entry.

Claude is launched with the workspace anchor as its current directory, so the
relative entry denotes a path from that anchor. Drift instead keeps the raw
relative string while its browser produces fully qualified paths. The same
directory therefore has two identities inside Drift:

- The browser does not mark the intended directory as a member.
- Space on that directory appends its absolute spelling beside the relative
  spelling, silently creating a semantic duplicate.
- Space again removes only the absolute spelling. The browser now presents the
  directory as unselected even though the original relative grant remains and
  Claude retains access.
- Enter on the manifest entry passes the raw relative string to Drift's file
  browser. Win32 resolves file enumeration against Drift's process current
  directory—the shell directory on native Windows and the repository directory
  under `run.sh`—rather than the workspace anchor. If a directory exists there,
  Drift opens the wrong location under an “editing workspace” banner.

Removing the relative row directly from the focused manifest works. The JSON is
recoverable by hand and no file is automatically deleted, but the primary
add/remove interaction can falsely report the effective permission state and
the manifest can navigate to an unrelated directory.

## Intended contract and platform semantics

- The current [Claude Code settings reference](https://code.claude.com/docs/en/configuration#permission-settings)
  documents `permissions.additionalDirectories` as additional working
  directories and gives `[ "../docs/" ]` as its example. Relative entries are
  therefore supported input, not malformed legacy data.
- Claude's [working-directories documentation](https://code.claude.com/docs/en/permissions#working-directories)
  says the default file-access root is the directory where Claude is launched,
  and describes `--add-dir`, `/add-dir`, and the persistent setting as three
  forms of the same extension.
- `LaunchClaudeIn` passes the workspace anchor as `lpCurrentDirectory` to
  `CreateProcess`. The Wine wrapper equivalently executes `cd "$ANCHOR_UNIX"`
  before starting Claude. For Drift workspaces, the anchor is therefore the
  only stable base for an ordinary relative member entry.
- Microsoft's [Windows path-format documentation](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file#fully-qualified-vs-relative-paths)
  distinguishes fully qualified drive/UNC paths, root-relative paths,
  drive-relative paths such as `C:folder`, and ordinary current-directory
  relative paths. Drive-relative paths depend on hidden per-drive state and are
  not interchangeable with `C:\folder`.
- Microsoft documents that [`GetFullPathName`](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfullpathnamea)
  applies the process current directory to relative input. A safe resolver must
  first combine an ordinary relative value with the known workspace anchor and
  then normalize that already anchored path; it must not call
  `GetFullPathName` on the raw setting value.

The installed native Claude Code used for this investigation is version
`2.1.220`. No real Claude session or configuration was needed for the
reproduction.

## Confirmed production control flow

1. `LoadMembersFrom(anchor)` decodes each JSON string into `members` but does
   not retain `anchor` as its operational base or qualify ordinary relative
   values.
2. `GetSelectedRowPath` constructs a fully qualified browser path.
3. `FindMember` compares that full path with each stored string using only
   `_stricmp`; `..\shared` cannot equal
   `C:\...\workspace\..\shared` or its normalized absolute target.
4. `ToggleMemberUnderCursor` therefore requests an add. DRIFT-005's transaction
   correctly reloads under its lock, but its same `FindMember` comparison still
   misses the relative entry and appends the absolute path.
5. `SaveMembersTo` preserves both strings in the target array. A later browser
   removal finds and removes only the absolute row, leaving the effective
   relative grant behind.
6. The manifest Enter path copies `members[manifest_selected]` directly into
   `ChangeCurrentDirectory`. That function stores the string unchanged and file
   enumeration consumes it relative to the process current directory.
7. Claude itself does not receive Drift's mistaken browser resolution: it reads
   the unchanged relative setting while running with the anchor as its current
   directory. This is a Drift management and navigation defect, not evidence
   that Claude resolves the setting incorrectly.

## Reproduction

A disposable Windows probe included the production `drift.c` translation unit
and was compiled with MSVC `/W4 /WX`, debug information, and AddressSanitizer.
It created this layout beneath a unique `%TEMP%\drift006-probe-*` directory:

```text
workspaces\workspace\.claude\settings.json  ["..\\shared"]
workspaces\shared\intended.txt              Claude/anchor target
shared\wrong.txt                            Drift process-cwd target
browse-start\                               process current directory
```

The probe called the production loader, `FindMember`,
`ApplyMemberChange(ADD)`, `ApplyMemberChange(REMOVE)`, and
`ChangeCurrentDirectory`. Result:

```text
DRIFT-006 production probe
  relative entry loaded literally:       YES
  intended absolute path recognized:     NO
  semantic duplicate persisted on add:   YES
  removing absolute leaves relative:     YES
  manifest jump opened process-cwd path: YES
DRIFT-006 result: CONFIRMED
```

The probe exited 0. Its source, runner, executable, object, symbols, lock file,
settings fixture, and unique workspace were all removed. No real Drift or
Claude configuration was read or written.

The unchanged `cmd /d /c tests\run_tests.bat` baseline also passes all eight
stages. No permanent test currently exercises relative membership identity or
manifest navigation, which explains why the defect is not detected.

## Severity assessment

**Final severity: Medium**, unchanged from the audit.

This is a core workspace-membership failure: a user can perform the normal
toggle-to-remove interaction, see the directory become unmarked, and still
leave Claude's access grant in force. The same entry can send Drift's browser to
an unrelated existing location while the UI continues to name the workspace
being edited. Low would understate that permission-state mismatch.

High would overstate it. The user or another settings writer must first supply
a relative entry, relative paths are an intentional Claude feature, the defect
does not grant an attacker access beyond the configured relative target, and no
file operation occurs merely by jumping. The relative row remains visible in
the manifest, can be removed there or edited by hand, and the settings file is
not structurally corrupted.

## Options considered

1. **Document that Drift supports absolute members only.** Rejected. Claude's
   current reference explicitly presents a relative value, and silently
   mismanaging a valid setting is not an acceptable compatibility rule.
2. **Convert every relative entry to an absolute string during load or the next
   save.** Simple, but it turns portable project configuration into a
   machine-specific path and rewrites user-authored spelling merely because an
   unrelated member changed. Rejected as unnecessarily destructive.
3. **Resolve only the manifest Enter action.** This prevents wrong navigation
   but leaves duplicate additions and false removal state intact. Rejected as
   incomplete.
4. **Reject every settings file containing a relative member.** This fails
   closed but disables a documented Claude feature and prevents ordinary Drift
   management of otherwise valid workspaces. Rejected for supported ordinary
   relative paths; appropriate only for ambiguous or unrepresentable path
   forms.
5. **Use anchor-aware operational identity while preserving stored spelling.**
   Recommended. Resolve paths transiently for comparisons and navigation,
   while leaving `members` in the form that `SaveMembersTo` already serializes.
   This fixes behavior without converting portable entries to absolute JSON.

## Approved design

1. Add one length-checked, lexical `ResolveMemberPath(anchor, stored, out)`
   helper. It must never consult or change the process current directory and
   must not require the target to exist.
2. For ordinary relative values (`folder`, `.\folder`, `..\folder`, and `/` as
   an internal separator), combine with the workspace anchor first, then
   normalize the fully anchored candidate. Preserve the configured relative
   string in `members` for display and serialization.
3. Preserve existing fully qualified drive and UNC behavior. Preserve the Wine
   wrapper's existing `/host/path` to `DRIFT_HOST_DRIVE` translation before
   operational resolution. Treat a single-root path against the anchor's drive
   where that is deterministic. Refuse drive-relative forms such as `C:folder`
   and root-relative forms without a usable anchor volume rather than inheriting
   mutable process/per-drive state.
4. Retain the active member anchor whenever `LoadMembersFrom` succeeds. Make
   `FindMember` compare resolved operational paths, falling back to exact text
   only when both values are identical. All add/remove transactions already
   reload the correct anchor under DRIFT-005's lock.
5. An add whose absolute browser path is already represented by a relative row
   returns `MEMBER_CHANGE_NO_CHANGE` and leaves the settings bytes unchanged.
   An explicit removal deletes every row resolving to that same operational
   path so duplicates created by the old behavior cannot leave a hidden grant.
6. Resolve a focused manifest entry against `edit_workspace` before calling
   `ChangeCurrentDirectory`. Refuse visibly if resolution is ambiguous or does
   not fit within `MAX_PATH`; never hand a relative member to the browser.
7. Fail membership editing closed when a configured entry cannot be resolved
   safely within Drift's path bounds. Preserve the exact settings file and show
   a specific blocking reason.
8. Do not change the JSON target locator, DRIFT-005 lock/source transaction, or
   configured spelling of surviving rows. General filesystem identity through
   junctions, symlinks, short names, trailing separators, and every absolute
   spelling remains outside this item.

This is deliberately a lexical configuration fix. It aligns Drift with the
known launch anchor without performing filesystem canonicalization that could
follow links, require access, or turn a nonexistent but valid configured path
into an editing failure.

## Implementation

- `ResolveMemberPath` classifies and normalizes a configured member without
  opening it. Ordinary relative values are first combined with the workspace
  anchor; only that fully qualified candidate reaches `GetFullPathNameA`, so
  the process working directory cannot affect the result. Drive-absolute, UNC,
  and deterministic drive-rooted forms remain supported. Drive-relative,
  unanchorable root-relative, empty, and overlong forms fail closed.
- `LoadMembersFrom` retains the active workspace in `member_anchor`, leaves each
  decoded setting in its authored form, and validates that every row has a safe
  operational resolution. Invalid values remain visible but set a specific
  blocking reason, preserving the original settings bytes on attempted edits.
  Wine's existing `/host/path` conversion still runs before validation; a
  conversion that cannot fit is now blocking rather than reinterpreted.
- `FindMember` first prefers an exact configured spelling, then compares
  anchor-resolved forms. Browser paths therefore recognize existing relative
  grants without changing their JSON representation.
- `ApplyMemberChange` validates the requested path under the DRIFT-005 lock. An
  equivalent add returns `MEMBER_CHANGE_NO_CHANGE` before publication. One
  explicit removal compacts away every operationally equivalent row, repairing
  relative/absolute duplicates created by older Drift versions while retaining
  the exact spelling and order of every survivor.
- `JumpToMemberAt` resolves the selected row against `edit_workspace` before
  calling the browser. An unsafe resolution keeps manifest focus and produces
  visible refusal instead of passing a relative path to Win32 enumeration.
- `tests/membership_path_test.c` includes the production translation unit and
  exercises the real load, locked mutation, save, lookup, and jump paths below
  one unique `%TEMP%` tree. `tests/run_tests.bat` adds it as AddressSanitizer
  stage 4 and expands the complete suite from eight to nine stages.

## Acceptance criteria and implementer evidence

| Criterion | Result | Evidence |
|---|---|---|
| `..\shared`, `.\child`, bare names, and forward-slash relatives resolve from the workspace anchor, never Drift's process cwd. | Pass | The focused resolver case requires four spelling variants to equal independently constructed anchor targets. The production jump case changes the process CWD to a distinct fixture and still requires `current_directory` to equal the anchor-derived target. |
| The absolute browser path for a relative row is recognized as already present. | Pass | A real settings fixture loads `..\shared`; production `FindMember` receives the independently built absolute target and returns row 0 while the stored row remains relative. |
| Adding an equivalent absolute path is idempotent and byte-preserving. | Pass | `ApplyMemberChange(MEMBER_CHANGE_ADD)` returns `MEMBER_CHANGE_NO_CHANGE`; the compact settings fixture, including unrelated `env`, remains byte-for-byte identical and no temp file exists. |
| Removing an absolute path revokes all equivalent relative/absolute rows. | Pass | A fixture contains `..\shared`, its absolute spelling, `../shared/`, and an unrelated row. One production removal leaves only the unrelated row and no lookup match for the target. |
| Manifest Enter navigates to the anchor-derived target. | Pass | `JumpToMemberAt` is called with `..\shared` after changing the process CWD. It stores the absolute anchor target in `current_directory` and releases manifest focus. A marker makes the intended target an enumeratable real directory. |
| Surviving relative spellings remain relative after another member changes. | Pass | A production add and removal of an unrelated absolute path leave the sole member exactly `../shared/`; the serialized value and unrelated `env.KEEP` key remain present. |
| Fully qualified drive, UNC, and Wine host paths keep their established behavior. | Pass | Resolver cases cover drive-rooted, drive-absolute, and UNC normalization. A `DRIFT_HOST_DRIVE=Z:` load/save round trip keeps `/Users/example/project` host-style and `../shared/` relative in JSON. |
| Ambiguous drive-relative, unanchorable root-relative, invalid, and overlong results fail closed and visibly. | Pass | Direct cases reject `C:folder` and a root-relative value under a UNC anchor. Empty and overlong settings fixtures set `MEMBER_PATH_BLOCK_REASON`; locked operations return `MEMBER_CHANGE_SETTINGS_BLOCKED`, preserve exact bytes, and leave no temp. Production callers map that typed result to visible feedback. |
| DRIFT-004 structural preservation remains intact. | Pass | All 19 production settings JSON cases and the shared locator/refusal cases pass unchanged under AddressSanitizer. |
| DRIFT-005 serialization and rebasing remain intact. | Pass | All 13 membership-concurrency cases pass unchanged, including two real child writers, bounded contention, crash release, stale rebase, and source conflict. |
| The permanent regression detects the original bug. | Pass | The pre-fix production probe recorded in the investigation commit demonstrates all four wrong outcomes. The permanent suite requires their opposites through production APIs and also requires resolver/jump wiring absent from the pre-fix source. |
| Investigation and tests never touch real configuration. | Pass | Both probe and permanent fixtures use unique `%TEMP%` roots. The focused suite completed cleanup; a post-run search found no `drift-member-path-test-*` directory. |

## Validation performed

- Disposable pre-fix production-linked AddressSanitizer probe recorded at
  investigation commit `7b17f57` — relative lookup failed, an equivalent
  absolute duplicate was published, removing it retained the relative grant,
  and manifest navigation used the process-CWD interpretation. Exit 0 meant
  the negative control reproduced every expected failure; all artifacts were
  removed.
- `cmd /c tests\run_tests.bat` — `ALL CHECKS PASSED`, nine stages. The new
  DRIFT-006 production-linked suite passes 12/12 cases under AddressSanitizer;
  the unchanged coverage includes 19/19 DRIFT-004 production settings cases,
  13/13 DRIFT-005 membership-concurrency cases, 13/13 name metadata cases,
  17/17 Claude launcher cases, 13/13 Vim resolver cases, general regressions,
  source lint, and `/W4 /WX` production compilation.
- `cmd /c build.bat` — optimized `/O2` application build succeeded. The ignored
  executable produced by this validation was removed afterward.
- `cl /analyze /W4 /wd4459 /c drift.c` — exit 0. It reports only the three
  established diagnostics in unchanged code: the `HandleOldHistory` stack
  frame and parameter/global shadowing in `GetSelectedRowPath` and
  `GetFilePath`; no diagnostic points into the DRIFT-006 change.
- `git diff --check` — passed apart from Git's informational LF-to-CRLF working
  tree notices.
- Post-run artifact check — no `drift-member-path-test-*` directory or
  generated `drift.exe` remains.

Safe optional manual validation may create a disposable workspace whose
settings contain `../sibling/`, launch Drift from a different directory, and
confirm that the sibling is already marked, Space removes the relative grant,
and manifest Enter opens the sibling. Do not use a real workspace for
failure-path validation; the automated suite already covers deterministic
ambiguous and overlong refusals.

## Compatibility, security, and error paths

- Relative JSON spellings must survive because they are useful when a project
  tree moves between machines. The fix changes Drift's operational view, not
  Claude's stored configuration contract.
- Removing all equivalent rows is an intentional repair for duplicates the old
  behavior could create. Equivalent entries grant the same effective directory,
  so retaining one would contradict an explicit removal.
- Resolution must be lexical and anchored before any Win32 normalization call.
  Calling `GetFullPathName` on raw relative input would reproduce the wrong-base
  bug and depend on mutable process state.
- A relative path may legitimately escape above the anchor with `..`; that is
  the purpose of many additional-directory configurations, not a traversal
  attack. The user-authored setting already grants that target to Claude.
- No resolution step should open the target or follow junctions. Existence,
  reparse-point identity, 8.3 aliases, broad separator/case identity, and paths
  beyond Drift's `MAX_PATH` architecture remain residual or separately scoped.
- DRIFT-004 continues to own structural JSON safety, DRIFT-005 concurrency and
  source conflicts, DRIFT-029 broader path identity, and DRIFT-034 unreadable
  settings loads. This item must not silently claim any of them fixed.
- Native Windows and Wine use different external spellings. Tests must cover
  ordinary relative values in both modes without converting them into host- or
  drive-specific JSON.

## Non-goals and residual risk

- Redesigning workspace storage or asking Claude to consume a Drift-owned
  canonical database.
- Resolving symlink, junction, short-name, or filesystem-object identity.
- Removing the existing `MAX_PATH` limit throughout Drift.
- Defining undocumented Claude behavior for tilde expansion or ambiguous
  cross-drive `C:relative` paths; the safe response is refusal until a stable
  contract exists.
- Fixing DRIFT-029 outside workspace membership or DRIFT-034's failed-read
  state.

Lexical equality can still differ from filesystem-object identity when links or
aliases are involved. That limitation does not justify retaining the confirmed
anchor/process-cwd mismatch for ordinary documented relative paths.

The implementation was validated against current documented Claude behavior,
but the automated suite does not start a real Claude process and interrogate
its internal permission roots. Windows case-insensitive comparison is retained;
case-sensitive host identity under Wine and other alias forms remain owned by
DRIFT-029 rather than being broadened into this fix.

## User disposition

On 2026-07-26, the user approved the recommended anchor-aware operational
identity, preservation of configured relative spellings, semantic duplicate
removal, visible fail-closed behavior for ambiguous/unrepresentable paths, and
production-linked regression coverage, and authorized implementation.

## Independent review handoff

The eligible reviewer must not be Codex. Read this file and
`quality/README.md`, then locate the complete immutable commit set with:

```text
git log --all --reverse --format="%H %s" --grep="Audit-ID: DRIFT-006"
```

Inspect both the investigation and implementation commits plus the surrounding
membership transaction. Confirm independently that raw JSON strings survive,
that only fully anchored candidates reach `GetFullPathNameA`, and that browser
lookup, add, all-equivalent removal, and manifest jump share the same identity.
Pay particular attention to drive/UNC/root/drive-relative classification,
trailing-root handling, Wine conversion, length bounds, empty values, and
failure feedback. Verify that the removal compaction cannot retain a semantic
duplicate or disturb an unrelated row, and that no new save path bypasses the
DRIFT-005 lock/source checks.

Run `cmd /c tests\run_tests.bat`, evaluate all twelve acceptance rows rather
than treating a green suite as approval, and inspect the pre-fix reproduction
evidence. Confirm scope: this commit must not claim filesystem-object identity
from links/aliases (DRIFT-029), failed-read safety (DRIFT-034), or a general
`MAX_PATH` redesign. Report `Approved`, `Approved with residual risk`, or
`Changes requested` using the required quality workflow format.

## Review history

No independent review rounds have been recorded yet.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible relative-path resolution and identity mismatch. |
| 2026-07-26 | Codex | `Untriaged` | `Investigating` | Began contract research, production control-flow tracing, and disposable relative-path reproduction; no production fix authorized. |
| 2026-07-26 | Codex | `Investigating` | `Investigating` | Confirmed wrong-base navigation, semantic duplicate publication, and ineffective browser removal with a production-linked AddressSanitizer probe; retained Medium severity and recommended anchor-aware operational identity that preserves relative JSON spelling; awaiting user disposition. |
| 2026-07-26 | User | `Investigating` | `Fix planned` | Approved anchor-aware operational identity, preservation of relative JSON spelling, removal of equivalent duplicates, fail-closed ambiguous/overlong handling, and production-linked tests. |
| 2026-07-26 | Codex | `Fix planned` | `Fixing` | Began the isolated resolver, comparison/removal/navigation wiring, and regression implementation. |
| 2026-07-26 | Codex | `Fixing` | `Awaiting review` | Implemented anchor-aware operational identity with raw-spelling preservation, all-equivalent removal, safe manifest navigation, typed fail-closed behavior, and 12 production-linked regression cases; all nine validation stages pass. |
