# Shutdown breadcrumbs → `session_log.json` (not terminal)

## Problem

Console is a poor signal when CGAL / repeat logging floods or corrupts the view. Quit hangs need a **durable** “last completed step” without relying on stdout.

## Approach

- Add **`SessionLogger::LogShutdownPhase(phase)`** — `PushEvent("shutdown_phase", { "phase": … })` then **`Flush("session_log.json", false)`** so each step rewrites the full JSON **quietly** (no per-flush `Log::Session` spam).
- **`main::Shutdown`** and **`Display::Shutdown`** replace prior `LOG_SESSION("[shutdown] …")` calls with `LogShutdownPhase`.
- **Reorder teardown:** `LogSessionEndSnapshot()` still runs before `Display::Shutdown()`; the mid-quit flush that used to happen *before* display teardown is replaced by incremental flushes inside `LogShutdownPhase`, so the file on disk always includes the latest `shutdown_phase` even if the process stops inside `Display::Shutdown`.

## Files

- `src/utils/SessionLogger.hpp`, `SessionLogger.cpp` — `LogShutdownPhase`, `Flush(path, logToConsole)`.
- `src/main.cpp`, `src/display/display.cpp` — call sites.
- `documentation/implementations/taskrunner_shutdown_detach_import_analysis_2026-05-12.md` — pointer to `shutdown_phase` events.
- `documentation/debug/Architecture_SessionLogger.md` — shutdown flow diagram.

## How to read

Open `session_log.json` (working directory is usually the app cwd). In `events`, find entries with `"type": "shutdown_phase"`; **`data.phase`** is the label. The **last** such entry in the array is the last step that finished before a hang or kill. Each row includes **`dt_ms`**: milliseconds since the **previous** event’s `t_ms` (same as `t_ms` deltas; easier to spot stalls than reading adjacent `t_ms` when many share one millisecond). Root fields **`last_event_t_ms`** and **`event_count`** summarize the tail.

## Outcome

`CAD_OpenGL` builds cleanly.

## Mini retro

- **Tradeoff:** one full JSON rewrite per phase at quit — acceptable for debugging; events list stays small.

## Follow-up — freeze with log stopping at `main: after FillSessionReproState`

Repro: `session_log.json` had **no** `session_end` and **no** later `shutdown_phase` — stall was inside **`LogSessionEndSnapshot()`**, almost certainly **`Log::Session("Session end snapshot recorded")`** blocking on a flooded/slow terminal or contending on the log mutex.

**Change:** `LogSessionEndSnapshot(bool echoToConsole = true)`; **`main::Shutdown`** uses **`LogSessionEndSnapshot(false)`** so quit still records `session_end` in the buffer without writing that line to the console.
