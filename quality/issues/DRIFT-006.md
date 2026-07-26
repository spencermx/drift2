# DRIFT-006 — Relative additionalDirectories resolve against the wrong location

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Investigating`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Final severity:** Medium
**Primary locations:** `drift.c:LoadMembersFrom`, `drift.c:FindMember`,
`drift.c:ApplyMemberChange`, `drift.c:ToggleMemberUnderCursor`,
`drift.c:HandleInput` (manifest jump), `drift.c:ChangeCurrentDirectory`
**Implemented by:** —
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

## Recommended design

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

## Acceptance criteria and regression plan

| Criterion | Planned evidence |
|---|---|
| `..\shared`, `.\child`, bare names, and forward-slash relatives resolve from the workspace anchor, never Drift's process cwd. | Production-linked cases vary the process cwd and require the same anchor-derived output. |
| The absolute browser path for a relative row is recognized as already present. | Load a relative fixture, call production `FindMember` with the resolved absolute target, and require a match. |
| Adding an equivalent absolute path is idempotent and byte-preserving. | `ApplyMemberChange(ADD)` must return `MEMBER_CHANGE_NO_CHANGE` and leave the compact relative JSON exact. |
| Removing an absolute path revokes all equivalent relative/absolute rows. | Seed semantic duplicates, remove through the production transaction, and require no equivalent row in the saved array. |
| Manifest Enter navigates to the anchor-derived target. | Plant distinct markers at the anchor and process-cwd interpretations and require only the anchor marker after the production jump path. |
| Surviving relative spellings remain relative after another member changes. | Add/remove an unrelated path and inspect the serialized target value plus unrelated JSON bytes. |
| Fully qualified drive, UNC, and Wine host paths keep their established behavior. | Focused resolver and round-trip fixtures cover native and `DRIFT_HOST_DRIVE` modes. |
| Ambiguous drive-relative, unanchorable root-relative, invalid, and overlong results fail closed and visibly. | Each fixture preserves settings byte-for-byte, leaves no temp file, and returns a typed/blocking outcome. |
| DRIFT-004 structural preservation remains intact. | All production settings JSON tests and shared locator cases pass unchanged. |
| DRIFT-005 serialization and rebasing remain intact. | All 13 membership-concurrency cases, including real child processes, pass unchanged. |
| The permanent regression detects the original bug. | Run the new production-linked test against the pre-fix source or use a negative control that restores raw textual identity/process-cwd jumping. |
| Investigation and tests never touch real configuration. | Fixtures use a unique `%TEMP%` workspace and verify cleanup. |

The proposed permanent suite is a focused Windows production-linked file such
as `tests/membership_path_test.c`, added to `tests/run_tests.bat`. Portable pure
path-resolution cases may also be added to the general suite if the helper can
be isolated without duplicating production logic.

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

## User disposition

Pending. No production code or permanent regression test has been changed.
The recommended anchor-aware, spelling-preserving design requires explicit user
approval before implementation.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible relative-path resolution and identity mismatch. |
| 2026-07-26 | Codex | `Untriaged` | `Investigating` | Began contract research, production control-flow tracing, and disposable relative-path reproduction; no production fix authorized. |
| 2026-07-26 | Codex | `Investigating` | `Investigating` | Confirmed wrong-base navigation, semantic duplicate publication, and ineffective browser removal with a production-linked AddressSanitizer probe; retained Medium severity and recommended anchor-aware operational identity that preserves relative JSON spelling; awaiting user disposition. |
