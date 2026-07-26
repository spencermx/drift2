# DRIFT-007 — Session-delete confirmation remains armed invisibly after resize

Tracker: [`TRACKER.md`](../TRACKER.md)

**Current status:** `Investigating`
**Reported:** 2026-07-25; comprehensive application review
**Initial severity:** Medium
**Confirmed severity:** Medium
**Primary locations:** `drift.c:HandleDeleteSession`, session-view input and
rendering paths; proposed `tests/session_delete_test.c` and `tests/run_tests.bat`
**Implemented by:** —
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

## Recommended design — awaiting user approval

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

## Proposed acceptance criteria and regression plan

| Criterion | Required evidence |
|---|---|
| Resize followed by `Y` cannot delete behind an erased prompt. | Production-linked test delivers resize then `Y`; it requires a second complete popup paint before the intercepted shell call. The old implementation must fail this test because it paints only once. |
| Current geometry governs the repainted popup. | Test changes the reported window origin/dimensions and requires the second `WriteConsoleOutputW` rectangle to be recentered and bounded by them. |
| A too-small resized window cancels. | Test shrinks below the minimum, then supplies `Y`; no shell call is allowed. |
| Query, allocation, and failed or clipped paint paths fail closed. | Deterministic stubs make geometry/output fail or report a partial rectangle; no shell call is allowed. Exercise allocation failure if the harness can inject it without distorting production code. |
| Ordinary confirmation still works. | With no resize, `Y` reaches the intercepted shell function exactly once while a complete prompt is visible. |
| Existing cancellation still works. | `N` and Escape each return without a shell call; unrelated keys do not silently cancel or delete. |
| The target remains the originally selected session. | Test records the `pFrom` path after resize and requires the originally displayed fake transcript path. |
| DRIFT-003 cleanup remains compatible. | Keep the existing successful-delete/failed-metadata-cleanup contract intact and rerun the session-name coverage. Any material rewrite of its governed control flow must repeat `Audit-ID: DRIFT-003` and receive compatibility review. |
| The complete application remains healthy. | The production-linked test runs under AddressSanitizer and `/W4 /WX`; all existing stages, an optimized build, `git diff --check`, and relevant static analysis pass. |
| Tests never touch real user data. | All paths are fake or under a unique `%TEMP%` fixture, shell deletion is always intercepted, and a post-run artifact check is clean. |

The expected permanent location is `tests/session_delete_test.c`, added as one
new stage in `tests/run_tests.bat`. It should include the production translation
unit, as the existing focused suites do, so modal control-flow regressions are
tested through the real handler rather than through a duplicate implementation.

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

Pending. No production change or permanent regression test is authorized by
this investigation. The recommended disposition is to retain Medium severity
and approve the local repaint/recenter/fail-closed design with the regression
contract above.

## Independent review handoff

After implementation, the eligible reviewer must not be the implementation
identity. Read this file and `quality/README.md`, locate the complete immutable
commit set with:

```text
git log --all --reverse --format="%H %s" --grep="Audit-ID: DRIFT-007"
```

Inspect the handler and the production-linked regression rather than accepting
the implementer's evidence. Confirm that resize cannot leave an invisible
armed `Y`, current geometry is used, incomplete paints and too-small windows
cancel, no real deletion is possible in tests, and normal `Y`/`N`/Escape
behavior is preserved. If the implementation commit also carries
`Audit-ID: DRIFT-003`, explicitly review that saved-name cleanup and its
partial-failure reporting are unchanged. Run the complete suite and report
`Approved`, `Approved with residual risk`, or `Changes requested` using the
required quality workflow format.

## Decision history

| Date | Actor | From | To | Summary |
|---|---|---|---|---|
| 2026-07-25 | Codex | — | `Untriaged` | Recorded by the comprehensive application review as a possible invisible armed-delete state after terminal resize. |
| 2026-07-26 | Codex | `Untriaged` | `Investigating` | Began modal input/render tracing and a disposable session-delete resize reproduction; no production fix authorized. |
| 2026-07-26 | Codex | `Investigating` | `Investigating` | Confirmed that resize is consumed without repaint or cancellation and a later `Y` reaches the intercepted shell delete boundary while the popup is hidden; retained Medium severity and recommended current-geometry repaint with fail-closed cancellation; awaiting user disposition. |
