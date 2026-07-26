# DRIFT-004 — Settings JSON scanner can edit the wrong object

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Investigating`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Final severity:** Medium
**Primary locations:** `drift.c:FindArraySpan`, `drift.c:LoadMembersFrom`,
`drift.c:SaveMembersTo`; `tests/row_guard_test.c`
**Implemented by:** —
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

## Recommended design

Replace `FindArraySpan` and the insertion `strstr`/`strchr` pair with one shared
locator used by both load and save:

1. Parse the entire nonempty document as a root JSON object, with a documented
   nesting limit that fails closed rather than risking unbounded recursion.
   Allow unknown keys and values; Drift is not a schema validator for settings
   it does not own.
2. Compare object keys by their JSON string value, including legal escape forms,
   so `permiss\u0069ons` cannot cause Drift to create a second semantic key.
3. Record exactly one direct root `permissions` member. If it is present, its
   value must be an object. More than one semantic `permissions` key is
   ambiguous and must refuse editing.
4. Within that object, record exactly one direct `additionalDirectories`
   member. If present, its value must be an array of strings and its exact `[`
   and `]` offsets become the replacement span. A duplicate or wrong type must
   refuse editing rather than fall through to insertion.
5. Return distinct results for: existing target array; permissions object exists
   but target is absent; root object exists but permissions is absent; and
   malformed, over-deep, duplicate, or wrong-typed input.
6. Make `LoadMembersFrom` set `json_block_reason` for the refusal result. Make
   `SaveMembersTo` re-run the locator on its fresh read and use only the
   returned structural insertion/replacement offset. This preserves the
   current protection against a file changing between load and save.
7. Retain the existing size, member-count, path-length, unsupported member-value
   escape, checked-write, close, temp cleanup, and publication protections.
8. Add a Windows production-linked `settings_json` suite that includes
   `drift.c`, works only below a unique disposable directory, and exercises
   actual load/save output. Keep useful pure scanner cases, but do not treat a
   copied helper alone as proof of production behavior.

The parser should implement the documented JSON grammar. The official docs say
invalid JSON is reported as a settings error and do not promise JSON-with-
comments or trailing commas. Refusing such files is therefore the proposed
contract; accepting undocumented JSONC would require an explicit compatibility
decision and corresponding grammar tests.

## Acceptance criteria and planned regression coverage

| Criterion | Required evidence |
|---|---|
| Only root `permissions.additionalDirectories` is loaded. | The supported plugin-option collision loads `permission-original`, never `plugin-original`. |
| Existing membership edits replace only the intended array. | Production save changes the permissions array while the earlier plugin array and every byte outside the target span remain unchanged. |
| Text equal to `permissions` cannot choose an object. | The announcement/`env` fixture inserts under the root permissions object and leaves `env` byte-for-byte unchanged. |
| A missing root permissions object is created only at the root. | Fixtures with nested `permissions` keys or values receive one new direct root object without modifying the nested values. |
| An existing permissions object receives the missing child. | Other permissions fields and their ordering/content survive while the new direct child is inserted. |
| Wrong target or parent types fail closed. | `permissions: []` and `permissions.additionalDirectories: null` return failure, publish nothing, and never create duplicate keys. |
| Duplicate semantic target keys fail closed. | Literal and JSON-escaped duplicate spellings both leave the file unchanged and expose a block reason. |
| Malformed, trailing-garbage, or over-deep input fails closed. | Each case returns failure with no temp publication; valid unknown nested settings remain accepted. |
| String escapes and structural characters cannot desynchronize parsing. | Escaped quotes, backslashes, brackets/braces in strings, nested arrays/objects, whitespace, and Unicode-escaped target keys are covered. |
| Load and save cannot disagree about the target. | Production-linked tests call both functions for the same fixtures and assert the same exact path is selected after the save re-read. |
| Existing safe-write and member refusal behavior remains intact. | The full Windows suite, warning compile, and all current `FindArraySpan`/`LoadMembersFrom` cases pass or are migrated to equivalent production-linked coverage. |
| The tests detect the original bug. | At least the plugin replacement and announcement insertion cases fail against `d26bb16` and pass after the fix. |

Safe optional manual validation should use a disposable `DRIFT_HOME` and
workspace, never the user's real settings. Confirm a workspace member can be
added and removed when hooks, env, plugin configuration, and permissions rules
coexist; compare the file before and after to verify only the intended span or
insertion changed.

## Validation performed during investigation

- Disposable production probe described above — both wrong-object paths
  confirmed; exit 0; all temporary artifacts removed.
- `cmd /c tests\run_tests.bat` — `ALL CHECKS PASSED`: source lint, general
  AddressSanitizer regressions, 13 name-metadata cases, 17 Claude-launcher
  cases, 13 Vim-resolver cases, and `/W4 /WX` production compile.
- Existing history and commit `715a0e2` inspected to separate this parent-path
  defect from the already-fixed key/value token defect.

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
- A hand-written JSON grammar is itself correctness-sensitive. A full grammar
  fixture matrix, a nesting cap, production-linked tests, and independent patch
  review are required residual controls.

## User disposition

Decision needed. The recommendation is option 2: implement the bounded shared
path-aware locator, preserve all unrelated bytes, fail closed on ambiguous or
invalid target structure, and add production-linked regression coverage. No
production code or permanent regression test has been changed during this
investigation.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible wrong-object settings rewrite. |
| 2026-07-25 | Codex | `Untriaged` | `Investigating` | Began tracing both array replacement and object insertion against the documented `permissions.additionalDirectories` contract. |
| 2026-07-25 | Codex | `Investigating` | `Investigating` | Confirmed both wrong-object writes through production load/save, retained Medium severity, and recommended one bounded path-aware structural locator; user disposition required before implementation. |
