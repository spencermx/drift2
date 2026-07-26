# DRIFT-007 — Session-delete confirmation remains armed invisibly after resize

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Verified`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Confirmed severity:** Medium
**Primary locations:** `drift.c:HandleDeleteSession`, session-view input and
rendering paths; `tests/session_delete_test.c` and `tests/run_tests.bat`
**Implemented by:** Codex
**Reviewed by:** Claude: approved with residual risk
**Decision owner:** User unless explicitly delegated

## Trigger and impact

Open the session list, press `D` on a session, resize the console while the
confirmation is visible, and then press `Y`. The console reflow can remove the
popup, but `HandleDeleteSession` consumes the resize without repainting or
cancelling. It remains inside its private input loop and accepts the later
`Y`, initiating deletion while no confirmation is visible.

The selected transcript is stable during this sequence: the nested modal owns
the input loop, ignores `J`/`K` and other unrelated keys, and `DrawScreen` does
not reload the session array in session view. This is therefore a
visibility-and-consent defect, not evidence that Drift switches the deletion
to a different row.

The shell operation requests the recycle bin with `FOF_ALLOWUNDO`; when that is
unavailable, `FOF_WANTNUKEWARNING` requests a warning before permanent
deletion. Those mitigations reduce the likely loss, but do not make an
invisible armed destructive action acceptable. A recycled transcript also
leaves the session list immediately and may require manual recovery outside
Drift.

## Intended contract and platform semantics

- Drift enables `ENABLE_WINDOW_INPUT`, so the console emits
  `WINDOW_BUFFER_SIZE_EVENT` records into the same input stream as key events.
  Microsoft documents both the
  [`WINDOW_BUFFER_SIZE_RECORD`](https://learn.microsoft.com/en-us/windows/console/window-buffer-size-record-str)
  payload and resize records in the
  [console input buffer](https://learn.microsoft.com/en-us/windows/console/console-input-buffer).
- [`WriteConsoleOutput`](https://learn.microsoft.com/en-us/windows/console/writeconsoleoutput)
  writes a rectangular block at the supplied buffer coordinates and clips it
  to the current console dimensions. A one-time popup write is not durable
  modal state across a later window reflow.
- A destructive confirmation may proceed only while the currently sized
  prompt was drawn successfully and remains the active modal. A resize must
  either redraw the underlying screen and prompt at current geometry or cancel
  the modal.
- `N`, Escape, an unreadable console, allocation failure, an unrepresentable
  window, or a failed/partial popup paint must fail closed without invoking the
  shell deletion operation.

## Confirmed production control flow

1. The main loop calls `DrawScreen()` and then `HandleInput()`. A normal
   non-key event returns from `HandleInput`, allowing the next main-loop screen
   redraw.
2. In `CM_SESSIONS`, an unmodified `D` instead calls
   `HandleDeleteSession()` synchronously. That function becomes the sole owner
   of subsequent console input until confirmation or cancellation.
3. `HandleDeleteSession` snapshots a pointer to
   `sessions[session_selected]`, queries the window, allocates and renders a
   44-by-7 popup once, calls `WriteConsoleOutputW` once without checking its
   result, and frees the popup buffer.
4. Its following `while` loop calls `ReadConsoleInput` directly. Every event
   that is not a key-down is discarded by one `continue`, including
   `WINDOW_BUFFER_SIZE_EVENT`.
5. The main loop cannot redraw while this nested handler is waiting. If resize
   reflow removes the one-time popup, no code restores the screen or prompt,
   recomputes its geometry, verifies that it still fits, or cancels the armed
   state.
6. The next `Y` reaches `SHFileOperation(FO_DELETE, ...)` even though no prompt
   is visible. `N` and Escape still cancel; unrelated key-down events are
   ignored and leave confirmation armed.
7. The general file-delete `ConfirmDelete` already implements the missing
   pattern: draw inside a `repaint` loop, call `DrawScreen` on resize, recenter
   the popup, and cancel when it cannot fit. Session/workspace rename and
   creation modals likewise have explicit resize handling. The session-delete
   handler was introduced earlier and was missed by those repairs.

## Reproduction

A disposable production-linked probe included the real `drift.c` translation
unit and intercepted only the console geometry/output/input calls, input-buffer
flush, and `SHFileOperation`. It installed one in-memory fake session and
delivered this deterministic event sequence:

1. The production popup write succeeds and marks the confirmation visible.
2. A `WINDOW_BUFFER_SIZE_EVENT` models the console reflow removing that
   overlay.
3. A `Y` key-down follows.
4. The intercepted shell function records the call, refuses it, and performs
   no filesystem operation.

The probe was compiled with MSVC `/W4 /WX /wd4459`, debug information, and
AddressSanitizer. Result:

```text
DRIFT-007 production event-sequence probe
  one popup paint before resize:       YES
  resize consumed, then Y accepted:    YES
  recycle call reached while hidden:   YES
  real filesystem operation performed: NO (intercepted)
DRIFT-007 result: CONFIRMED
```

It exited 0 under AddressSanitizer. Its source, executable, object, symbols,
and uniquely named temporary paths were removed. `DRIFT_CLAUDE_DIR` pointed to
a unique nonexistent `%TEMP%` path, so no real Claude transcript or Drift
configuration was read or changed.

The unchanged `cmd /d /c tests\run_tests.bat` baseline passes all nine stages.
No permanent test currently drives the session-delete modal through a resize,
which explains why the defect is not detected.

## Severity assessment

**Confirmed severity: Medium**, unchanged from the audit.

The primary interaction can invoke deletion of user session data after its
confirmation has disappeared. The operation is destructive, the invisible
armed state persists across unrelated keys, and the user cannot inspect the
target at the moment consent is accepted. Low would understate that breach of
the destructive-action contract.

High would overstate the likely impact. The operation requests recycle-bin
semantics and a warning when undo is unavailable; the modal retains the
original visible selection rather than silently retargeting; and the sequence
requires the user to press `Y` after resizing. The defect does not itself
bypass filesystem permissions or create an attacker-controlled path.

## Options considered

1. **Leave the behavior unchanged because recycle may be available.** Rejected.
   Recoverability does not restore informed confirmation, and recycle support
   is not universal.
2. **Cancel immediately on every resize.** Safe and small, but inconsistent
   with Drift's other repaired modal handlers and forces the user to reopen a
   prompt after an otherwise harmless resize.
3. **Redraw and recenter after resize; cancel when the prompt cannot be shown.**
   Recommended. This preserves the user's modal action while maintaining the
   rule that `Y` is accepted only after a successful current-geometry paint.
4. **Only cancel when the resized dimensions are too small.** Incomplete. A
   resize that remains large enough can still erase the one-time overlay.
5. **Centralize every modal in a new shared event/render framework.** Potential
   future cleanup, but too broad for an isolated bug fix and likely to couple
   unrelated input paths.

## Approved design

1. Retain the selected session identity for the life of the modal; do not
   reload or retarget it because of resize.
2. Put geometry measurement, popup allocation/content construction, centering,
   and `WriteConsoleOutputW` inside a local `repaint` branch in the modal loop.
   Recompute the current window on every repaint.
3. On `WINDOW_BUFFER_SIZE_EVENT`, call `DrawScreen()` to restore the resized
   session view, set `repaint`, and continue before considering another key.
4. Preserve the existing 44-by-7 target size and width clamp. If the current
   prompt would be narrower than 20 cells or the window shorter than 7 cells,
   cancel rather than remain armed behind nothing.
5. Treat geometry-query, allocation, and popup-write failures as cancellation.
   Also verify that `WriteConsoleOutputW` reports the complete requested
   rectangle rather than a clipped partial paint before marking confirmation
   visible.
6. Process `Y`, `N`, and Escape only after the prompt has been successfully
   painted for the current geometry. Preserve the existing behavior for other
   key-down events and do not expand this item into a modifier-policy change.
7. Leave the `SHFileOperation` flags, session reload/selection clamp, and
   DRIFT-003 saved-name cleanup semantics unchanged.
8. Add a production-linked modal regression test rather than a source-text-only
   assertion. Keep the change local; do not refactor the unrelated popup
   handlers.

This design should either use a small testable popup-paint helper or keep the
existing construction inline if the production-linked harness can intercept
the Win32 boundary cleanly. The behavioral contract matters more than the
helper shape.

## Implementation

- `HandleDeleteSession` now owns a `repaint` state for the lifetime of its
  nested modal loop. Every repaint re-queries the visible window, reconstructs
  the popup at its current clamped width, and recenters it against the current
  window origin and dimensions.
- A resize record redraws the underlying session screen, sets `repaint`, and is
  consumed before another key can be evaluated. The next `Y` is therefore
  reachable only after a current-geometry popup has been painted.
- Geometry-query, allocation, write, clipped-output, and too-small-window paths
  return without entering the destructive branch. The handler compares the
  rectangle returned by `WriteConsoleOutputW` with the complete requested
  rectangle, rather than treating a clipped successful call as confirmation.
- The existing selected-session pointer, `Y`/`N`/Escape and unrelated-key
  policy, shell flags, reload/selection clamp, and DRIFT-003 saved-name cleanup
  block remain unchanged.
- `tests/session_delete_test.c` includes the production translation unit and
  intercepts console geometry, popup/screen output, modal input, input flush,
  popup allocation, and `SHFileOperation`. Scripted resize events model the
  console reflow erasing the prior overlay. The shell boundary always returns
  from the test interceptor, so no real deletion is reachable.
- `tests/run_tests.bat` adds the focused AddressSanitizer `/W4 /WX` suite as
  stage 7 and expands the complete Windows suite from nine to ten stages. Its
  build output is retained in `%TEMP%\drift_tests` and printed only if that
  focused test fails to compile.

## Acceptance criteria and implementer evidence

| Criterion | Result | Evidence |
|---|---|---|
| Resize followed by `Y` cannot delete behind an erased prompt. | Pass | The production handler receives resize then `Y`; the harness requires one underlying-screen redraw, two complete popup paints, and `SHFileOperation` only while the second prompt is marked visible. The pre-fix handler paints once and the investigation probe reached the intercepted delete boundary while hidden. |
| Current geometry governs the repainted popup. | Pass | The scripted window changes from origin `(2,3)`, size `80x25` to origin `(7,4)`, size `100x30`; the second reported popup rectangle must equal `(35,15)-(78,21)`. |
| A too-small resized window cancels. | Pass | Resize to `19x6` leaves only the original paint and no shell call, even with `Y` queued behind the resize. |
| Query, allocation, and failed or clipped paint paths fail closed. | Pass | Separate cases fail the initial geometry query, popup allocation, initial write, resize repaint, and full-width output rectangle. Every case requires zero shell calls. |
| Ordinary confirmation still works. | Pass | With no resize, one complete popup followed by `Y` reaches the intercepted shell function exactly once while visible and flushes queued input. |
| Existing cancellation still works. | Pass | `N` and Escape each return with no shell call. An unrelated `J` leaves the visible prompt and original target intact before a later `Y`. Console input failure also cancels. |
| The target remains the originally selected session. | Pass | After resize and repaint, the intercepted double-NUL shell source begins with exactly `C:\fake\original-session.jsonl`. |
| DRIFT-003 cleanup remains compatible. | Pass | All 13 unchanged name-metadata cases pass. A new handler integration case reports the partial result when an intercepted successful recycle is followed by isolated metadata-cleanup failure; session reload still occurs. The implementation commit repeats `Audit-ID: DRIFT-003`. |
| The complete application remains healthy. | Pass | The ten-stage suite, optimized `/O2` build, `git diff --check`, quality validator, and `/analyze` complete successfully. Static analysis reports only three established diagnostics in unchanged code. |
| Tests never touch real user data. | Pass | `DRIFT_HOME` and `DRIFT_CLAUDE_DIR` point to one unique nonexistent `%TEMP%` root, every transcript path is fake, the real shell function is macro-inaccessible, and the root remains absent after all cases. |

## Validation performed

- Pre-fix production-linked AddressSanitizer probe recorded at investigation
  commit `ba5647f` — a resize was consumed without another popup paint and a
  later `Y` reached the intercepted shell boundary while the prompt was hidden.
  The probe performed no filesystem operation and all disposable artifacts
  were removed.
- `cmd /d /c tests\run_tests.bat` — `ALL CHECKS PASSED`, ten stages. The new
  production-linked session-delete suite passes 16/16 cases under
  AddressSanitizer and `/W4 /WX`; unchanged coverage includes 13/13 DRIFT-003
  name-metadata cases, 19/19 settings cases, 12/12 membership-path cases,
  13/13 membership-concurrency cases, 17/17 Claude launcher cases, 13/13 Vim
  resolver cases, general regression coverage, source lint, and warning-clean
  production compilation.
- `cmd /d /c build.bat` — optimized `/O2` application build succeeded. The
  ignored `drift.exe` produced by validation was removed afterward.
- `cl /analyze /W4 /wd4459 /c drift.c` — exit 0. It reports only the three
  established diagnostics in unchanged code: the `HandleOldHistory` stack
  frame and parameter/global shadowing in `GetSelectedRowPath` and
  `GetFilePath`; no diagnostic points into `HandleDeleteSession`.
- `git diff --check` — passed apart from informational LF-to-CRLF working-tree
  notices.
- `powershell -NoProfile -ExecutionPolicy Bypass -File quality\validate.ps1`
  — passed with DRIFT-007 as the sole active item.
- Post-run artifact check — the fake session/config root was never created;
  the generated application and static-analysis object were removed.

## Compatibility, security, and error paths

- `HandleDeleteSession` also contains DRIFT-003's post-delete saved-name
  cleanup and partial-failure notice. A resize fix must leave that transaction
  unchanged. Because it changes the surrounding deletion gate, the safest
  audit trail is to repeat `Audit-ID: DRIFT-003` on the implementation commit
  and require the independent reviewer to re-confirm compatibility.
- The test must never permit the real `SHFileOperation` to run, even against a
  temporary transcript. An intercepted call is sufficient to prove whether
  the destructive boundary was reached.
- Successful return from `WriteConsoleOutputW` alone may still describe a
  clipped output rectangle through its in/out region argument. Confirmation
  should become armed only after a complete rectangle was reported.
- Event ordering is the relevant safety boundary: a queued resize must be
  consumed and repainted before a later queued `Y`. General protection against
  unrelated applications overwriting console cells is outside Drift's modal
  contract.
- Repainting `DrawScreen` is safe in session view: it redraws from the existing
  `sessions` array and does not reload it. The single-threaded nested handler
  continues to own selection until it returns.
- Preserve shell warning/recycle flags. Changing permanent-delete policy or
  replacing deprecated console APIs is not necessary to close this defect.

## Non-goals and residual risk

- Refactoring every modal or every `ReadConsoleInput` site.
- Changing whether Ctrl/Alt-modified `Y`, key-repeat events, or unrelated keys
  count inside an already visible confirmation; those policies are separable
  from resize visibility.
- Guaranteeing recycle-bin availability or implementing an in-app transcript
  recovery system.
- Solving external console corruption that occurs without an input-buffer
  event Drift can observe.
- Redesigning session discovery, ordering, launch, or name metadata.

After the recommended fix, there remains a narrow platform race between a
successful paint and an external display change for which Drift receives no
ordered event. The concrete queued-resize defect will nevertheless be closed:
every resize event observed before `Y` must restore a complete current prompt
or cancel.

## User disposition

On 2026-07-26, the user retained Medium severity and approved the local
repaint/recenter/fail-closed design, its production-linked regression contract,
and DRIFT-003 compatibility validation, and authorized implementation.

## Independent review handoff

The eligible reviewer must not be Codex. Read this file and
`quality/README.md`, then locate the complete immutable commit set with:

```text
git log --all --reverse --format="%H %s" --grep="Audit-ID: DRIFT-007"
```

Inspect both the investigation and implementation commits plus the surrounding
modal and DRIFT-003 cleanup control flow. Do not accept `popup_writes == 2` as
sufficient by itself: verify event ordering, complete returned-region checks,
current origin/dimension math, cancellation before key dispatch on every
failure, stable target lifetime, and that a second resize cannot re-arm a stale
paint. Confirm the harness cannot reach the real shell function or user paths
and that its visibility model makes the pre-fix implementation fail.

The implementation commit also carries `Audit-ID: DRIFT-003`. Explicitly
re-confirm that successful deletion still attempts saved-name removal, cleanup
failure cannot undo or misreport the transcript result, input is flushed, the
session list reloads and clamps as before, and the existing 13-case metadata
suite remains meaningful. Run the ten-stage suite, inspect all ten acceptance
rows rather than treating green output as approval, and report `Approved`,
`Approved with residual risk`, or `Changes requested` using the required
quality workflow format.

## Review history

### Round 1 — Claude, 2026-07-26

- **Reviewer:** `Claude` (Opus 5). Absent from `Implemented by`, so eligible.
- **Commit set:** `ba5647f` "Investigate DRIFT-007: confirm hidden delete after
  resize" (documentation only) and `a502428` "Fix DRIFT-007: repaint session
  delete prompt on resize". `git log --all --reverse --format="%H %s"
  --grep="Audit-ID: DRIFT-007"` returns these two and no others. `a502428` also
  correctly appears under `--grep="Audit-ID: DRIFT-003"`.
- **Verdict:** `Approved with residual risk`.

**Acceptance criteria:** all ten pass, each re-verified by the reviewer rather
than accepted from the evidence column.

**Tests run by the reviewer:**

- `cmd /d /c tests\run_tests.bat` — `ALL CHECKS PASSED`, ten stages, exit 0,
  including 16/16 session-delete modal cases and 13/13 unchanged DRIFT-003
  name-metadata cases.
- `build.bat` — optimized `/O2` build succeeded; `drift.exe` removed afterward.
- `cl /analyze /W4 /wd4459 /c drift.c` — exit 0. Three diagnostics, all in code
  this fix does not touch: C6262 at `drift.c:4495`, C6244 at `drift.c:4761` and
  `drift.c:4777`. None points into `HandleDeleteSession`.

**The handoff's specific instructions were followed rather than accepting a
green suite:**

- *Event ordering.* `repaint` is cleared only immediately after a complete
  successful write, and a resize record sets it before `continue`, so the loop
  cannot reach the key dispatch without a current-geometry paint. Because
  `ReadConsoleInput` is FIFO, a queued resize is always consumed before a queued
  `Y`.
- *Complete returned-region check.* `written` is seeded from `requested` and
  compared on all four edges after the call, so a clipped-but-successful paint
  fails closed. Verified by mutation, below.
- *Cancellation before key dispatch.* Geometry-query, undersized-window,
  allocation, write-failure, and clipped-output paths all `return`. The function
  body ends immediately after the modal loop, so `return` and `break` are
  equivalent here and no post-loop cleanup is skipped.
- *Stable target lifetime.* `sel` is captured once before the loop and the
  destructive branch uses `sel->path`/`sel->id`, never
  `sessions[session_selected]`. The repaint's `DrawScreen()` cannot invalidate
  it: `DrawScreen` (`drift.c:471`) contains no `LoadSessionsFor` or
  `ApplySessionNames` call, and the only drawing function that does reload —
  `DrawClaudeInfoPane` at `drift.c:3092` — is reached solely from the
  `claude_mode == CM_WORKSPACES` branch at `drift.c:828`, while this modal runs
  in `CM_SESSIONS`. `DrawScreen`'s clamp of `session_selected` is therefore
  harmless to the captured target.
- *A second resize cannot re-arm a stale paint.* Correct in the implementation,
  but **not covered by any permanent case** — see finding 1. Confirmed by a
  reviewer probe, below.
- *Harness cannot reach the real shell or user paths.* `SHFileOperation` is
  macro-redirected before `../drift.c` is included, so the real symbol is
  unreachable from that translation unit; the interceptor always returns a
  failure code. `DRIFT_HOME` and `DRIFT_CLAUDE_DIR` point at one unique
  nonexistent `%TEMP%` root and every transcript path is fake.
- *The harness's visibility model does make the pre-fix implementation fail.*
  `test_popup_visible` becomes true only on a complete popup paint and is
  cleared by a resize, by an underlying-screen write, and by failed or clipped
  paints, so a handler that paints once and then consumes a resize reaches the
  shell boundary with visibility false. Mutant 1 confirms this empirically.

**Reviewer-added mutation testing** (throwaway copies outside the repository; no
repository file was modified). Six mutants of `HandleDeleteSession` compiled
against the permanent session-delete and name-metadata suites:

1. *Remove the `WINDOW_BUFFER_SIZE_EVENT` branch* — the original DRIFT-007
   defect. **Caught**, 4 cases fail.
2. *Set `repaint` but never call `DrawScreen`* — **caught**.
3. *Accept a clipped paint as visible confirmation* — **caught**.
4. *Center against the buffer origin instead of the current window origin* —
   **caught**.
5. *Remove the undersized-window cancellation* — **caught**.
6. *Repaint on only the FIRST resize* — **NOT caught**; the whole suite passes.
   See finding 1.

Every mutant left the 13 DRIFT-003 name-metadata cases green.

**Reviewer probe — repeated resize.** Because mutant 6 survived, the shipped
behavior was checked directly with an intercepted harness driving the production
handler:

```text
two resizes then Y: popup paints=3  screen redraws=2  shell_calls=1  visible=yes
third paint region=(39,17)-(82,23)  expected (39,17)-(82,23)
later undersized resize then Y: shell_calls=0
three resizes then N:           shell_calls=0
```

Each resize forces its own repaint, the final paint is centered in the *latest*
window, a later undersized resize cancels rather than arming, and `N` still
cancels. **The implementation is correct; only the coverage is missing.**

**DRIFT-003 compatibility — explicitly re-confirmed, as the shared
`Audit-ID: DRIFT-003` requires.** The deletion and saved-name-cleanup block was
extracted from both `git show 6387846:drift.c` and the current file and compared:
the 32-line region from `if (vk == 'Y')` through the `N`/Escape line is
**byte-identical** to its reviewed form. Successful deletion still attempts
`SetSessionName(..., "")`, cleanup failure is still recorded only inside the
successful-`SHFileOperation` branch and reported after the reload, input is still
flushed, and the session list still reloads and clamps. All 13 metadata cases
pass unchanged and stayed green under all six mutants, and the new
"DRIFT-003 cleanup failure still reports the partial result" case exercises the
partial-outcome path through the modal. DRIFT-003's accepted behavior did not
change, so it remains `Verified` under rule 6.

**Findings — no defect in the fix. Three items:**

1. *No permanent case queues two resizes, so the handoff's own "a second resize
   cannot re-arm a stale paint" requirement is unverified by the suite.* Mutant 6
   — repaint on the first resize only — passes all 16 session-delete cases while
   leaving a later `Y` armed behind an erased prompt, which is precisely the
   defect this item exists to close. The shipped code is correct, as the probe
   above shows; the gap is in coverage. A case queueing resize, resize, `Y` and
   requiring three complete paints plus visibility at the shell boundary would
   close it, and the harness already supports it — `QueueResize` can be called
   twice and `TEST_SCRIPT_CAPACITY` is 16.
2. *The harness's popup/screen discrimination is heuristic.*
   `tests/session_delete_test.c:70` classifies a write as the popup when
   `buffer_size.Y == 7 && source[0].Char.UnicodeChar == L'\x250C'`. A
   full-screen `DrawScreen` write into a seven-row console whose first cell is
   the frame's top-left corner would be misclassified as a popup paint. No
   current case uses a seven-row window, so nothing is wrong today, but a future
   case at that geometry would silently corrupt the paint accounting rather than
   fail loudly.
3. *`DRIFT-003` was left `Verified` rather than moved to `Awaiting review` while
   its compatibility was pending.* Rule 6 says a previously `Verified` affected
   item returns to `Awaiting review` until an eligible reviewer confirms
   compatibility, then may go directly back to `Verified`. The record instead
   kept it `Verified` and asked the reviewer to re-confirm. The end state is
   correct — compatibility is confirmed above and DRIFT-003 stays `Verified` —
   so this is a process note, not a defect. The audit-trail half of rule 6 was
   handled well: `a502428` does repeat `Audit-ID: DRIFT-003`, so the shared
   change is discoverable, which is the improvement this reviewer asked for in
   DRIFT-006 Round 1 finding 1.

**Checked and dismissed:** `popup_w` only ever shrinks from 44, so the
allocation is bounded at 44 × 7 `CHAR_INFO`; `written` is correctly used as the
in/out region argument; `WriteConsoleOutputW` clips to the screen buffer while
the popup is positioned inside `srWindow`, which is contained in that buffer, so
an unclipped paint is the normal case and clipping is genuinely exceptional;
`const int popup_h` is a harmless tightening; the modal cannot change
`claude_mode`, so the `CM_SESSIONS` reasoning above holds for the whole loop;
unrelated key-down events still fall through without disarming, matching the
declared non-goal; and no `free` is missed on any early return, since `popup` is
released before every visibility check.

**Scope check:** clean. `a502428` restructures only `HandleDeleteSession`, adds
`tests/session_delete_test.c`, inserts one stage in `tests/run_tests.bat`, and
updates this issue file and its own tracker row. It does not touch the shell
flags, the reload/clamp, the DRIFT-003 block, or any other modal handler, and it
correctly does not claim the modifier/key-repeat policy or unobservable external
console corruption.

**Resolution:** approved with residual risk. No finding requires a change to
this commit set: finding 1 asks for one additional regression case rather than a
production change, finding 2 is latent harness fragility, and finding 3 is a
process note whose end state is already correct. Recorded by the reviewer under
section 6 because no implementer was present in the session; no
implementer-authored section of this file was modified.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible invisible armed-delete state after terminal resize. |
| 2026-07-26 | Codex | `Untriaged` | `Investigating` | Began modal input/render tracing and a disposable session-delete resize reproduction; no production fix authorized. |
| 2026-07-26 | Codex | `Investigating` | `Investigating` | Confirmed that resize is consumed without repaint or cancellation and a later `Y` reaches the intercepted shell delete boundary while the popup is hidden; retained Medium severity and recommended current-geometry repaint with fail-closed cancellation; awaiting user disposition. |
| 2026-07-26 | User | `Investigating` | `Fix planned` | Approved current-geometry repaint/recenter, cancellation when the prompt cannot be shown completely, production-linked regression coverage, and DRIFT-003 compatibility validation. |
| 2026-07-26 | Codex | `Fix planned` | `Fixing` | Began the isolated session-delete modal repair and focused regression implementation. |
| 2026-07-26 | Codex | `Fixing` | `Awaiting review` | Repainted and recentered the session-delete prompt after resize, failed closed on incomplete visibility, preserved DRIFT-003 cleanup behavior, and added 16 production-linked modal cases; all ten validation stages pass. |
| 2026-07-26 | Claude | `Awaiting review` | `Verified` | Independent review approved with residual risk: ten-stage suite, optimized build, and `/analyze` re-run; five of six modal mutants including the original defect are detected, but a first-resize-only repaint survives the suite, so a reviewer probe confirmed the shipped handler repaints on every resize and centers in the latest window. DRIFT-003 compatibility re-confirmed byte-for-byte and it remains `Verified` under rule 6. |
| 2026-07-26 | Codex | `Verified` | `Verified` | Rechecked the complete quality folder after Round 1. Preserved the repeated-resize and harness-classification follow-ups for later test hardening, corrected two post-review record details below, and left Claude's verdict and authored review text unchanged. |

## Record maintenance

**2026-07-26 — Codex; post-review follow-up preservation.** Round 1 findings 1
and 2 remain the canonical record of two optional test-only improvements: add
a permanent `resize, resize, Y` sequence that requires three complete popup
paints, and replace or strengthen the harness's heuristic popup/screen write
classification. Claude's production probe established that the shipped handler
already behaves correctly; neither item is a reopened production defect, and
no follow-up implementation has yet been authorized.

Round 1 says at one point that the shell interceptor "always returns a failure
code." The real shell function is indeed unreachable, which is the relevant
safety property, but the interceptor is deliberately configurable:
`test_shell_result` defaults to `ERROR_ACCESS_DENIED`, while the DRIFT-003
partial-result case sets it to zero to simulate successful recycling and reach
the isolated metadata-cleanup path. This clarification preserves Claude's
review text rather than rewriting it and does not affect the verdict, test-data
isolation, or compatibility conclusion.

The metadata above also drops the stale word "proposed" from the now-implemented
test locations. No code, test, status, severity, attribution, or review verdict
changes in this maintenance update.
