# DRIFT-004 — Settings JSON scanner can edit the wrong object

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Awaiting review`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Final severity:** Medium
**Primary locations:** `settings_json.h:DriftLocateSettingsJsonTarget`,
`drift.c:LoadMembersFrom`, `drift.c:SaveMembersTo`;
`tests/settings_json_test.c`, `tests/row_guard_test.c`
**Implemented by:** Codex
**Reviewed by:** —
**Decision owner:** User unless explicitly delegated

## Trigger and impact

Drift owns one exact setting in a workspace's `.claude\settings.json`:
`permissions.additionalDirectories`. The defect has three closely related
triggers:

1. Another supported nested setting contains an array-valued property also
   named `additionalDirectories` before the root `permissions` member. Drift
   loads and replaces that earlier array instead of the permissions array.
2. The intended array is absent, but the complete JSON string value
   `"permissions"` occurs before an unrelated object. Drift treats the string
   as the key, selects the next `{`, and inserts `additionalDirectories` into
   that unrelated object.
3. `permissions.additionalDirectories` exists but is not an array. The scanner
   returns the same result as "missing," and the insertion path adds a second
   property with the same name instead of refusing the ambiguous document.

In the first two cases `SaveMembersTo` publishes the wrong edit and returns
`true`. The intended permission remains unchanged, so Claude does not receive
the workspace access the user believes Drift saved. The first case also
destroys the previous plugin-option value; the second can make a previously
valid setting, such as `env`, fail schema validation. On the next load, Drift
can continue displaying the wrong nested array as if it were workspace
membership, concealing the mismatch.

The replacement is still written through the existing checked temp-file path;
this is not a partial-write issue. The complete, wrong JSON transformation is
successfully published.

## Intended contract and authoritative format

Drift's source comment at `drift.c:Workspace Designer` and the README both name
`permissions.additionalDirectories` and promise to splice only that array so
all other settings survive.

The current official Claude Code documentation agrees:

- [Claude Code settings](https://code.claude.com/docs/en/settings) lists
  `additionalDirectories` as a child of `permissions`, and describes
  `.claude/settings.json` as JSON whose other top-level settings include
  `companyAnnouncements`, `env`, `permissions`, and `pluginConfigs`.
- [Configure permissions](https://code.claude.com/docs/en/permissions) states
  that persistent additional working directories come from
  `permissions.additionalDirectories`.
- [Plugins reference](https://code.claude.com/docs/en/plugins-reference) allows
  a plugin user-configuration option to be an array of strings when its
  `multiple` flag is set, stores it below
  `pluginConfigs[plugin-id].options`, and permits any valid identifier as the
  option name. `additionalDirectories` is therefore a supported collision, not
  merely malformed JSON invented for the probe.

## Confirmed control flow

1. `FindArraySpan` walks strings safely enough to recognize a property-shaped
   `"additionalDirectories": [` token, but it tracks neither object depth nor
   the parent key. The first matching property anywhere wins.
2. `LoadMembersFrom` calls that global locator directly and parses the selected
   span into the visible workspace-member list.
3. `SaveMembersTo` re-reads the file, calls the same locator, and replaces the
   returned byte span. A structurally wrong span is indistinguishable from the
   intended one, so the checked write and rename correctly report success.
4. When the locator returns false, the save path calls
   `strstr(json_buf, "\"permissions\"")` and then `strchr(..., '{')`.
   Neither operation establishes that the match is a root object key or that
   the brace begins its value.
5. `FindArraySpan` returns false both when the target is absent and when its
   value is present but not an array. `SaveMembersTo` therefore cannot fail
   closed on a wrong type and can create a duplicate target key.

Commit `715a0e2` fixed an earlier, separate targeting defect: the old bare
`strstr` for `additionalDirectories` could match the name inside a value and
then overwrite the next array, or walk from a non-array value into a later
array. Its seven regression cases all still pass. They do not establish the
required parent path and do not exercise `SaveMembersTo`'s insertion branch.

## Reproduction

Investigation used branch `codex/future-work` at `d26bb16`. A disposable Windows
probe included the actual `drift.c` translation unit, renamed only its program
entry point, and called production `LoadMembersFrom` and `SaveMembersTo`. It
created a unique workspace below `%TEMP%`, compiled with `/W4 /WX /wd4459`, and
removed its source, executable, object, settings fixture, and directories after
the run.

The replacement fixture used a supported multiple-value plugin option before
the real permissions list:

```json
{
  "pluginConfigs": {
    "example@local": {
      "options": {
        "additionalDirectories": ["plugin-original"]
      }
    }
  },
  "permissions": {
    "additionalDirectories": ["permission-original"]
  }
}
```

Production loaded `plugin-original` as the workspace member. Saving the member
`edited-by-drift` replaced the plugin option and left `permission-original`
unchanged.

The insertion fixture used only documented setting shapes:

```json
{
  "companyAnnouncements": ["permissions"],
  "env": {"KEEP": "yes"},
  "permissions": {"allow": []}
}
```

Production matched the announcement string, selected `env`'s opening brace,
inserted the directory array into `env`, left `permissions` without the array,
and returned success. The exact probe output was:

```text
replacement: loaded plugin option instead of permission list: YES
replacement: rewrote plugin option and left permission list unchanged: YES
insertion: inserted into env and left permissions without the list: YES
probe result: CONFIRMED (0 unexpected results)
```

The permanent six-stage test suite still reports `ALL CHECKS PASSED`. This is a
coverage gap, not contradictory evidence: `tests/row_guard_test.c` contains a
copy of `FindArraySpan`, checks only key-token targeting, and never supplies two
same-named properties under different parents or runs the insertion transaction.

## Severity assessment

Medium remains appropriate. The defect can silently overwrite persistent,
user- or plugin-authored settings and materially breaks Drift's core workspace
membership workflow while reporting success. A supported plugin configuration
can trigger it without corruption or hostile input. The settings file and
workspace data remain present, the wrong edit is recoverable from source
control or manual correction, and no privilege escalation or primary workspace
deletion was established, so High would overstate the impact.

## Options considered

1. **Add parent-depth checks to the current text searches.** This can patch the
   two demonstrated fixtures, but leaves separate ad hoc scanners for loading,
   replacement, and insertion. It still has no reliable distinction among a
   missing target, wrong type, duplicate target, malformed suffix, or escaped
   spelling of a JSON key. Rejected as too easy to retarget again.
2. **Use one bounded, path-aware JSON parser/locator and preserve the original
   bytes.** Recommended. Parse the complete document, locate only direct root
   `permissions` and its direct `additionalDirectories` child, and return
   explicit states and byte offsets for replace, insert-child, insert-root, or
   refuse. Splice only at those proven offsets so formatting and unknown
   settings remain byte-for-byte intact.
3. **Vendor a full DOM library and serialize the entire document after the
   edit.** A mature parser reduces grammar risk, but a DOM rewrite changes
   whitespace, ordering, and possibly number/string representations throughout
   Claude's file, contrary to Drift's narrow-splice promise. It also adds a
   dependency to a deliberately small C application. Not preferred unless the
   path-aware parser proves unmaintainable.
4. **Replace the file with a Drift-owned skeleton or refuse every file with
   other settings.** This avoids structural parsing by discarding or forbidding
   supported Claude configuration. It violates the product's preservation
   contract and is rejected.

## Agreed design and implementation

The user approved option 2. The implementation replaces `FindArraySpan` and the
insertion `strstr`/`strchr` pair with one shared, length-bounded structural
locator in `settings_json.h`:

1. `DriftLocateSettingsJsonTarget` validates the complete document as one root
   JSON object. It reads by explicit byte bounds rather than C-string length,
   accepts and preserves an optional UTF-8 BOM, allows unknown JSON keys and
   value types, and refuses nesting beyond 64 levels.
2. Object-key comparison uses the decoded JSON string value. Literal and
   escaped spellings such as `permissions` and `permiss\u0069ons` therefore
   identify the same semantic key.
3. The locator accepts at most one direct root `permissions` object and at most
   one direct `additionalDirectories` child. The child, when present, must be an
   array containing only strings. Wrong types or semantic duplicates make the
   entire edit unsafe.
4. A successful parse returns one explicit action: replace the proven array;
   insert a child at the proven permissions-object brace; or insert a new
   permissions object at the proven root brace. Malformed, over-deep,
   duplicate, or wrongly typed structure returns failure rather than being
   conflated with a missing target.
5. `LoadMembersFrom` uses that action and parses members only from the exact
   root path. Unsafe structure sets `json_block_reason`, leaving the manifest
   read-only rather than displaying a decoy nested array.
6. `SaveMembersTo` reruns the same locator on its fresh bounded read, including
   an existing zero-byte file. It splices or inserts only at returned offsets.
   A structurally changed file between load and save is revalidated and refused
   if unsafe.
7. All bytes outside the replacement span or new insertion remain untouched.
   There is no DOM and no whole-document serialization. The existing checked
   read, 64 KiB limit, path/member refusal rules, temp write, close, cleanup,
   and publication path remain in force.
8. Documented JSON is the compatibility boundary. Comments, trailing commas,
   malformed escapes, duplicate target keys, and other invalid JSON are left
   unchanged. Claude's documentation does not promise JSONC, and accepting it
   would make safe structural offsets a separate grammar contract.

## Production, documentation, and test changes

- `settings_json.h` contains the dependency-free parser and edit-action API
  shared by production and portable tests.
- `drift.c:LoadMembersFrom` and `SaveMembersTo` consume the shared structural
  result; the old global scanner and text-based insertion targeting are gone.
- `README.md:Where things live` now names the exact
  `permissions.additionalDirectories` path and the fail-closed behavior for
  malformed, ambiguous, or wrongly typed structure.
- `tests/row_guard_test.c` includes the production header instead of carrying a
  copy of the old scanner. Nineteen path/grammar cases cover parent identity,
  escaping, insertion actions, semantic duplicates, unknown JSON values,
  number grammar, and BOM offsets; the six existing member-refusal cases now
  use the exact path-aware locator.
- `tests/settings_json_test.c` includes `drift.c` and uses actual
  `LoadMembersFrom`/`SaveMembersTo` filesystem behavior below a unique `%TEMP%`
  workspace. Nineteen cases cover both original wrong-object writes, exact
  byte-preserving output, escaped keys, wrong types, duplicates, non-string
  members, trailing/malformed/empty/NUL-containing/over-deep files, save-time
  revalidation, first-file creation, and exact comma-free insertion into empty
  root and permissions objects. It clears `DRIFT_HOST_DRIVE` only in the test
  process and restores it during cleanup.
- `tests/run_tests.bat` adds that AddressSanitizer suite as stage 3 and numbers
  the complete Windows run as seven stages.

## Data integrity, compatibility, security, and error paths

- **Data integrity:** no replacement or insertion offset is used before the
  complete bounded document validates. Embedded NUL bytes are rejected rather
  than letting later `strcat`/`strlen` truncate a preserved suffix. Every unsafe
  production fixture asserts the original byte length and contents, no temp
  publication, and a false save result.
- **Target compatibility:** documented root-path files retain the same array
  formatting generated by Drift, unknown settings remain byte-for-byte intact,
  semantic escaped target keys retain their original spelling, and a missing
  file still receives the existing skeleton. An existing empty file was
  previously overwritten as if absent; it is now correctly treated as invalid
  JSON and preserved.
- **Grammar compatibility:** the parser validates structure and target types,
  not Claude's entire evolving schema. Valid unknown objects, arrays, strings,
  booleans, nulls, and JSON numbers remain accepted. Undocumented JSONC and
  invalid UTF/control-byte structures are refused rather than normalized.
- **Error paths:** load-time refusal populates the existing manifest-pane reason.
  Save-time refusal sets the same reason and returns false through the existing
  callers, which resync or report that settings were not updated.
- **Security:** the change adds no process launch, path source, filesystem root,
  or privilege transition. Length bounds and the nesting limit reduce parser
  exposure to hostile settings content; no security elevation was claimed.

## Acceptance criteria and evidence

| Criterion | Result | Evidence |
|---|---|---|
| Only root `permissions.additionalDirectories` is loaded. | Pass | The production plugin-option collision loads `permission-original`, never `plugin-original`; the portable locator extracts the same exact span. |
| Existing membership edits replace only the intended array. | Pass | The production save result is compared byte-for-byte with an expected document in which the earlier plugin array and every byte outside the permissions-array span are unchanged. |
| Text equal to `permissions` cannot choose an object. | Pass | The announcement/`env` production fixture inserts under root permissions; its full expected output preserves `env` exactly. |
| A missing root permissions object is created only at the root. | Pass | A nested decoy target remains exact while the full production output contains one new direct root object before it. |
| An existing permissions object receives the missing child. | Pass | The announcement fixture retains `allow` and inserts the direct child at the proven permissions brace. |
| Wrong target or parent types fail closed. | Pass | `permissions: []`, a null target, and a non-string target member all return failure, preserve every byte, leave no temp, and expose a block reason. |
| Duplicate semantic target keys fail closed. | Pass | Literal/escaped duplicates of both `permissions` and `additionalDirectories` preserve the file and refuse publication. |
| Malformed, trailing-garbage, or over-deep input fails closed. | Pass | Malformed, trailing, empty, embedded-NUL, and 65+-level fixtures are preserved; valid unknown values remain editable. |
| String escapes and structural characters cannot desynchronize parsing. | Pass | Portable cases cover escaped quotes, `]` in strings, nested arrays/objects, whitespace, Unicode-escaped target keys, and a UTF-8 BOM. |
| Load and save cannot disagree about the target. | Pass | Production tests call both functions, and a file changed from an editable object to `permissions: []` after load is revalidated and refused at save. |
| Existing safe-write and member refusal behavior remains intact. | Pass | All six member refusal cases, the full seven-stage suite, optimized build, warning compile, and static analysis complete as recorded below. |
| The tests detect the original bug. | Pass | The investigation probe confirmed both production failures at `d26bb16`; the permanent production fixtures now require the opposite exact outcomes. |

Safe optional manual validation should use a disposable `DRIFT_HOME` and
workspace, never the user's real settings. Confirm a workspace member can be
added and removed when hooks, env, plugin configuration, and permissions rules
coexist; compare the file before and after to verify only the intended span or
insertion changed.

## Validation performed

- Investigation's disposable production probe — both wrong-object paths
  confirmed against `d26bb16`; exit 0; every temporary artifact removed.
- `cmd /c tests\run_tests.bat` — `ALL CHECKS PASSED`, seven stages: source lint,
  general AddressSanitizer regressions, 19/19 production settings-JSON cases,
  13/13 name-metadata cases, 17/17 Claude-launcher cases, 13/13 Vim-resolver
  cases, and `/W4 /WX` production compile. The general suite includes 19 shared
  locator cases and all six member-refusal cases.
- `cmd /c build.bat` — optimized `/O2` Windows build succeeded; the ignored
  validation executable was removed afterward.
- `cl /analyze /W4 /wd4459 /c drift.c` — exit 0. It reported only the same three
  diagnostics in unchanged code, shifted by removed scanner lines: the large
  stack frame in `HandleOldHistory` and the two known parameter/global
  shadowing sites in `GetSelectedRowPath` and `GetFilePath`. No diagnostic
  intersects the parser or settings transaction.
- `git diff --check` — passed.
- `tests/run_tests.sh` could not start because this Windows host's Git Bash has
  no `cc`. The shared parser nevertheless runs under MSVC AddressSanitizer in
  the portable row-guard suite, and the Windows production-linked suite covers
  the actual application transaction.

## Non-goals and residual risk

- Concurrent successful membership edits are DRIFT-005; this fix should re-read
  before saving as it does now but does not serialize writers or detect lost
  updates.
- Relative member-path semantics are DRIFT-006.
- Raising the 64 KiB settings limit, `MAX_MEMBERS`, or `MAX_PATH`, and decoding
  currently unsupported `\u` escapes inside directory *values* are outside
  scope.
- Drift will not validate the schema of settings it does not own. It will
  validate enough JSON structure and target types to prove safe byte offsets.
- The structural parser treats non-control high-bit bytes inside strings as
  opaque and preserves them; it does not independently validate UTF-8 for
  settings Drift does not own. Claude remains the schema/encoding authority.
- The 64-level cap can refuse technically valid but exceptionally deep settings.
  This is an intentional fail-closed resource bound, not a claimed Claude limit.
- A hand-written JSON grammar is correctness-sensitive. The shared grammar
  matrix, production-linked tests, bounded recursion, static analysis, and
  independent patch review are the residual controls.

## User disposition

On 2026-07-25, the user approved option 2 and authorized its isolated
implementation: use the bounded shared path-aware locator, preserve all
unrelated bytes, fail closed on ambiguous or invalid target structure, and add
production-linked regression coverage.

## Independent review handoff

The eligible reviewer must not be Codex. Read this record and the quality
protocol, then locate the complete immutable commit set with:

```text
git log --all --reverse --format="%H %s" --grep="Audit-ID: DRIFT-004"
```

Inspect every resulting patch, especially `settings_json.h`, both production
callers, the exact-output fixtures in `tests/settings_json_test.c`, and the
portable shared-header cases. Run `cmd /c tests\run_tests.bat` and evaluate all
twelve acceptance-criteria rows independently. Confirm the parser proves the
direct root path before every edit, malformed/ambiguous files remain unchanged,
the regression suite exercises production and detects both original failure
shapes, and the commits contain no unrelated change. Report `Approved`,
`Approved with residual risk`, or `Changes requested` in the protocol's full
review format.

Do not treat this fix as resolving DRIFT-005 concurrency or DRIFT-006 relative
path semantics. The investigation commit is documentation only; implementation
attribution begins with the production/test commit.

## Review history

No independent review rounds yet.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible wrong-object settings rewrite. |
| 2026-07-25 | Codex | `Untriaged` | `Investigating` | Began tracing both array replacement and object insertion against the documented `permissions.additionalDirectories` contract. |
| 2026-07-25 | Codex | `Investigating` | `Investigating` | Confirmed both wrong-object writes through production load/save, retained Medium severity, and recommended one bounded path-aware structural locator; user disposition required before implementation. |
| 2026-07-25 | User | `Investigating` | `Fix planned` | Approved the recommended bounded path-aware locator, fail-closed structural outcomes, byte-preserving edits, and production-linked coverage. |
| 2026-07-25 | Codex | `Fix planned` | `Fixing` | Began the isolated parser, load/save integration, and regression implementation. |
| 2026-07-25 | Codex | `Fixing` | `Awaiting review` | Replaced all target searches with the shared bounded structural locator, added exact production-linked and portable regression coverage, and passed full, optimized, warning, static-analysis, and quality validation. |
