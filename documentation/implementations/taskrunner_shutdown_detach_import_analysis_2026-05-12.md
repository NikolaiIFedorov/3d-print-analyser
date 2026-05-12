# TaskRunner: non-blocking shutdown for import/analysis (quit freeze)

## Problem

`TaskRunner::~TaskRunner` **join()**s worker threads. If a worker is stuck inside **CGAL** (or any long call), **teardown blocks indefinitely** — user sees freeze on quit. Structure carve already uses a **leaked** dedicated `TaskRunner`; import/analysis used a member `TaskRunner` destroyed with `Display`, so **join** ran at static teardown.

## Approach

1. **`TaskRunner::RequestStopClearQueueAndDetachWorkers()`** — set `stopRequested`, **drain** the pending job queue (discarded work acceptable at process exit), `notify_all`, **`detach()`** each worker, clear `workers` so `~TaskRunner` has nothing to join.
2. **Never run `~TaskRunner` on that instance after detach** — detached threads may still return to `WorkerLoop` and touch the instance mutex. So **`Display::Shutdown()`** calls detach, then **`taskRunner.release()`** — **intentional one-time leak** at normal app exit (same family as `StructureCarveTaskRunner`).
3. **`Display` holds `std::unique_ptr<TaskRunner>`** — after `release()`, pointer is null; remaining `~Display` does not destroy a second runner.

## Files

- `src/utils/TaskRunner.hpp` — new API + `workersDetachedForExit` guard in destructor.
- `src/display/display.hpp`, `src/display/display.cpp` — `unique_ptr`, ctor init, `Shutdown()` order with `mainThreadPipeline.Clear()`, cancel handles, detach+release.
- `documentation/Architecture_UIThreadAndWorkers.md` — shutdown / leak note.

## Outcome

Quit path no longer **blocks** on `join()` for the default import/analysis runner when a worker is stuck. **Build:** `CAD_OpenGL` OK (2026-05-12).

## Risks / follow-up

- **Intentional leak** of one `TaskRunner` per process lifetime (only if `Shutdown()` ran). Not suitable if tests construct/destroy `Display` in a loop without process exit — would leak threads; mitigate later with a test-only join policy.
- **Structure** carve runner unchanged (already leaked).

## Mini retro

- **Worked:** Small surface (`taskRunner` only in `display.cpp`).
- **Tradeoff:** Leak vs freeze — chosen for product quit path.

## Shutdown breadcrumb logging (2026-05-12)

`LOG_SESSION` breadcrumbs were replaced by **`SessionLogger::LogShutdownPhase`**: each step appends a `shutdown_phase` event and **rewrites `session_log.json` immediately** (quiet flush — no per-step console line) so the last phase survives a hang even when the terminal is unusable. Inspect the JSON: events where `"type": "shutdown_phase"` and `data.phase`.

## Structure staging carve: one WARN instead of terminal “blank line flood” (2026-05-12)

**Symptom:** After CGAL precondition text, some terminals filled with **apparent empty lines**.

**Cause:** Per-solid `LOG_WARN` with **identical** CGAL `what()` text → logger repeat suppression used **ANSI cursor-up**; newline-heavy messages made repeats look like blank spam.

**Fix:** `display.cpp` — one aggregated `LOG_WARN` after the carve loop, with `\n`/`\r`/`\t` replaced by spaces (`SanitizeMessageForSingleLineLog`).
