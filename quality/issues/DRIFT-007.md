# DRIFT-007 — Session-delete confirmation remains armed invisibly after resize

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Awaiting review`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Confirmed severity:** Medium
**Primary locations:** `drift.c:HandleDeleteSession`, session-view input and
rendering paths; proposed `tests/session_delete_test.c` and `tests/run_tests.bat`
**Implemented by:** Codex
**Reviewed by:** —
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

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible invisible armed-delete state after terminal resize. |
| 2026-07-26 | Codex | `Untriaged` | `Investigating` | Began modal input/render tracing and a disposable session-delete resize reproduction; no production fix authorized. |
| 2026-07-26 | Codex | `Investigating` | `Investigating` | Confirmed that resize is consumed without repaint or cancellation and a later `Y` reaches the intercepted shell delete boundary while the popup is hidden; retained Medium severity and recommended current-geometry repaint with fail-closed cancellation; awaiting user disposition. |
| 2026-07-26 | User | `Investigating` | `Fix planned` | Approved current-geometry repaint/recenter, cancellation when the prompt cannot be shown completely, production-linked regression coverage, and DRIFT-003 compatibility validation. |
| 2026-07-26 | Codex | `Fix planned` | `Fixing` | Began the isolated session-delete modal repair and focused regression implementation. |
| 2026-07-26 | Codex | `Fixing` | `Awaiting review` | Repainted and recentered the session-delete prompt after resize, failed closed on incomplete visibility, preserved DRIFT-003 cleanup behavior, and added 16 production-linked modal cases; all ten validation stages pass. |
