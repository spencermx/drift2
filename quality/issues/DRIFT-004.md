# DRIFT-004 — Settings JSON scanner can edit the wrong object

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Verified`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Final severity:** Medium
**Primary locations:** `settings_json.h:DriftLocateSettingsJsonTarget`,
`drift.c:LoadMembersFrom`, `drift.c:SaveMembersTo`;
`tests/settings_json_test.c`, `tests/row_guard_test.c`
**Implemented by:** Codex
**Reviewed by:** Claude: approved with residual risk
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

### Round 1 — Claude, 2026-07-25

- **Reviewer:** `Claude` (Opus 5). Absent from `Implemented by`, so eligible.
- **Commit set:** `5109efc` "Investigate DRIFT-004: confirm wrong-object settings
  edits" (documentation only) and `6aea09b` "Fix DRIFT-004: target only
  permissions member array". `git log --all --reverse --format="%H %s"
  --grep="Audit-ID: DRIFT-004"` returns these two and no others.
- **Verdict:** `Approved with residual risk`.

**Acceptance criteria:** all twelve pass, each re-verified by the reviewer
rather than accepted from the evidence column. Rows 1–5 and 12 were additionally
re-tested by mutation (below), which is stronger than the recorded evidence.

**Tests run by the reviewer:**

- `cmd /c tests\run_tests.bat` — `ALL CHECKS PASSED`, seven stages: source lint,
  general AddressSanitizer regressions (including the 19 shared locator cases
  and all six member-refusal cases), 19/19 production settings-JSON cases,
  13/13 name-metadata cases, 17/17 Claude-launcher cases, 13/13 Vim-resolver
  cases, and the `/W4 /WX` production compile. DRIFT-001/002/003 coverage is
  unaffected.
- `build.bat` — optimized `/O2` build succeeded; `drift.exe` removed afterward.
  The working tree was clean before and after the review.
- `cl /analyze /W4 /wd4459 /c drift.c` — exit 0. Exactly three diagnostics, all
  in code this fix does not touch and all shifted upward by the removed scanner
  lines: C6262 at `drift.c:4098` (`HandleOldHistory` stack frame) and C6244 at
  `drift.c:4363` and `drift.c:4379`. None is in `settings_json.h`,
  `LoadMembersFrom`, or `SaveMembersTo`. This matches the implementer's claim.

**Reviewer-added property fuzzing (throwaway harness outside the repository; no
repository file was modified).** A hand-written JSON grammar is the largest risk
in this change, so the reviewer built an AddressSanitizer harness that includes
the production `settings_json.h`, replicates `SaveMembersTo`'s splice
byte-for-byte, and asserts the end-to-end safety property: *whatever Drift
writes, Drift must read back as exactly the membership it intended.* For every
accepted document it checks that the spliced result re-parses, resolves to
`REPLACE_ARRAY`, that the stored array is byte-identical to what was written,
and that saving the same membership twice is idempotent.

- Corpus of 20 hand-built documents (BOM, escaped target keys, plugin
  collision, nested decoys, `]` inside members, all JSON value types, empty
  arrays and objects) plus 400,000 random byte mutations of them.
- Result: **126,529 documents accepted, 273,491 refused, 0 property violations,
  0 AddressSanitizer reports.** No accepted document produced output Drift could
  not read back correctly.
- Depth boundary: the deepest accepted nesting is 63 against the documented cap
  of 64, and neither side of the boundary crashes. The cap is real, not nominal.

**Reviewer-added mutation testing (same isolation).** Eight mutants of
`settings_json.h` were compiled against the permanent suites:

1. *First `additionalDirectories` array anywhere wins* — the exact pre-fix
   DRIFT-004 targeting rule. Caught: "a supported plugin option cannot outrank
   the permissions path" and "nested permissions does not replace a missing root
   object" fail in the portable suite; "supported plugin option never becomes
   workspace membership", "save replaces only the root permissions array", and
   "nested target cannot replace a missing root permissions object" fail in the
   production suite. This is the decisive negative control: the headline
   fixtures do discriminate on the original defect.
2. *Last-match-wins variant of the same rule* — caught by the nested fixture.
3. *Drop the `seen_directories` guard* — caught by both duplicate-target cases.
4. *Drop the `seen_permissions` guard* — caught by the duplicate-parent case.
5. *Allow non-string members in the directories array* — caught.
6. *Drop the trailing-byte check (`cursor != end`)* — caught by the
   trailing-garbage case **and** by the embedded-NUL case (see finding 1).
7. *Remove the nesting cap* — caught by the over-deep case.
8. *Take the insertion offset from the root brace instead of the proven
   permissions brace* — caught by the `env` retargeting case and the empty
   permissions-object case.

Every mutant is detected. The regression coverage genuinely exercises production
behavior and would detect reintroduction.

**Independent verification of specific claims:**

- Both original failure shapes are reproduced as permanent fixtures with
  byte-exact expected output, not merely as boolean assertions.
- `DriftLocateSettingsJsonTarget` is reached with the real byte count
  (`(size_t)len`) at `drift.c:2186` and `drift.c:2355`, not `strlen`, so a
  document is validated over its whole length.
- Drift cannot trap itself with the new empty-file refusal: `SaveMembersTo`'s
  absent branch always writes the complete skeleton, and `EnsureWorkspaceNotes`
  creates only `CLAUDE.md`, so Drift never produces a zero-byte
  `settings.json` (see finding 2).
- `out` cannot overflow. It is `malloc(len + cap + 256)`; the replace path can
  only shrink the non-array remainder, and the largest insertion adds 49 bytes
  of literal plus `"\n  }"` and one comma — 54 bytes against 256 of headroom.
- DRIFT-005 and DRIFT-006 were not silently treated as fixed. No locking,
  conflict detection, or path-resolution change was added, and both tracker rows
  remain `Untriaged`.

**Findings — no code defects. Four residual/records-level items:**

1. *The splice's NUL-freedom is an implicit invariant carried by the
   trailing-byte check.* `SaveMembersTo` preserves the tail with
   `strcat(out, json_buf + target.array_end + 1)` (`drift.c:2372`) and
   `strcat(out, json_buf + at)` (`drift.c:2392`), both of which stop at the
   first NUL. That is safe only because a document containing any NUL cannot
   parse: a raw NUL inside a string trips `raw < 0x20`
   (`settings_json.h:76`), and outside a string it fails structurally and then
   the `parser.cursor != parser.end` check at `settings_json.h:376`. Mutant 6
   demonstrated the coupling precisely — removing that one line broke the
   embedded-NUL test as well as the trailing-garbage test. The behavior is
   correct and tested today, but the dependency is non-obvious: a future
   relaxation of the trailing-byte rule would silently reintroduce suffix
   truncation. Recorded so that line is understood as load-bearing for data
   integrity, not only for grammar strictness.
2. *An existing zero-byte `settings.json` is now refused rather than
   initialized.* The gate changed from `len > 0` to `file_exists`
   (`drift.c:2354`), so a 0-byte file now fails the locator and yields
   "(settings.json structure cannot be edited safely)" with a refused save,
   where it previously received the create-from-scratch skeleton. Strictly this
   is correct — an empty file is not valid JSON — and the implementer disclosed
   and tested it. It is still a real behavior change with a plausible trigger
   (a user or another tool creating the file with `touch`/`New-Item`), and the
   message does not tell the user that deleting the empty file would let Drift
   proceed. Drift cannot cause this state itself, as verified above. Raised for
   the maintainer's awareness; below the section 1 reporting bar, so no new ID
   was opened.
3. *Load-side member extraction still decodes short escapes lossily.*
   `LoadMembersFrom` (`drift.c:2204-2219`) writes the bare second character for
   `\n`, `\t`, `\b`, `\f`, and `\/`, while `SaveMembersTo`'s escaper
   (`drift.c:2311`) only re-escapes `\` and `"`, so such a member would
   round-trip corrupted. This is pre-existing and unchanged by DRIFT-004 —
   `\u` is separately blocked with its own reason — and these escapes cannot
   occur in a legitimate Windows path. Out of scope; noted so a future editor
   does not assume the new parser fixed it.
4. *Lone surrogates and unvalidated UTF-8 are accepted inside strings Drift does
   not own.* `\uXXXX` is decoded without surrogate pairing
   (`settings_json.h:92-102`) and non-control high-bit bytes are opaque. This
   cannot cause a wrong edit — any decoded value above `0x7f` simply fails the
   key match, and the bytes are preserved verbatim — and the record already
   declares UTF-8 validation a non-goal with Claude as the schema authority.
   No action; confirmed harmless rather than merely accepted.

**Coverage gaps:** the Unix runner `tests/run_tests.sh` still cannot execute on
this host (no `cc` in Git Bash), so the shared header is unexercised under
gcc/clang; the reviewer confirmed it does run under MSVC AddressSanitizer via
`tests/row_guard_test.c`, so the parser is sanitizer-covered, only not
cross-compiler-covered. Separately, `TestBlockedFixture` proves "load blocked
⇒ save refused" through the `json_block_reason` short-circuit at
`drift.c:2278`, so the save-side locator's own fail-closed path rests on the
single `TestSaveRevalidatesFreshRead` case. That case is the right one and it
passes, but the save-side branch is thinner than the load-side branch.

**Checked and dismissed:** the `has_member` flags are redundant but not wrong,
because the empty-object early return means they are always true where they are
read; `needs_comma` derived from object emptiness is strictly more robust than
the old "next non-whitespace character is not `}`" text probe; nested
`permissions` and nested `additionalDirectories` correctly fall through to the
generic parsers with `expected == NULL`; `ParseLiteral` and `ParseNumber` cannot
run together into a false accept because the caller immediately requires `,` or
a closing bracket; offsets are `int` but `length > INT_MAX` is rejected and
`json_buf` is 64 KiB; and `target` is memset before any field is written, so a
caller ignoring the false return sees action `NONE` rather than a stale offset.

**Scope check:** clean. `6aea09b` adds `settings_json.h`, rewrites only the two
settings functions in `drift.c` and deletes the superseded `FindArraySpan`,
replaces the copied scanner in `tests/row_guard_test.c` with the production
header, adds `tests/settings_json_test.c`, renumbers `tests/run_tests.bat` from
six to seven stages and inserts the new stage, updates the `README.md` paragraph
describing the spliced path, and updates only this issue file and its own
tracker row. `5109efc` is documentation only. No unrelated cleanup, formatting,
or refactoring.

**Resolution:** approved with residual risk. No finding requires a change to
this commit set, so the item closes as `Verified` with findings 1–4 and the
coverage gaps preserved above as reviewer-discovered residual risk, separate
from the implementer's own residual-risk section. Recorded by the reviewer under
section 6 because no implementer was present in the session; no
implementer-authored section of this file was modified.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible wrong-object settings rewrite. |
| 2026-07-25 | Codex | `Untriaged` | `Investigating` | Began tracing both array replacement and object insertion against the documented `permissions.additionalDirectories` contract. |
| 2026-07-25 | Codex | `Investigating` | `Investigating` | Confirmed both wrong-object writes through production load/save, retained Medium severity, and recommended one bounded path-aware structural locator; user disposition required before implementation. |
| 2026-07-25 | User | `Investigating` | `Fix planned` | Approved the recommended bounded path-aware locator, fail-closed structural outcomes, byte-preserving edits, and production-linked coverage. |
| 2026-07-25 | Codex | `Fix planned` | `Fixing` | Began the isolated parser, load/save integration, and regression implementation. |
| 2026-07-25 | Codex | `Fixing` | `Awaiting review` | Replaced all target searches with the shared bounded structural locator, added exact production-linked and portable regression coverage, and passed full, optimized, warning, static-analysis, and quality validation. |
| 2026-07-25 | Claude | `Awaiting review` | `Verified` | Independent review approved with residual risk: full suite, optimized build, and `/analyze` re-run; a 400,000-case AddressSanitizer property fuzz found no round-trip violation and confirmed the depth cap; eight parser mutants including the exact pre-fix targeting rule are all detected; no code defects; the NUL/trailing-byte coupling, the empty-file behavior change, pre-existing escape round-tripping, and the Unix-runner and save-side coverage gaps recorded as reviewer-discovered residual risk. |
