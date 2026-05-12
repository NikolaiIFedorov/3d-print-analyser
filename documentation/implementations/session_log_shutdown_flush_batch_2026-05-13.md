# Session log: batch shutdown flush (2026-05-13)

## Problem

`LogShutdownPhase` flushed `session_log.json` after **every** breadcrumb (full JSON rewrite). Quitting triggered many rapid writes; IDE file watchers often stuttered when switching back to the editor.

## Change

- **`LogShutdownPhase`**: `PushEvent` only by default; optional **`CAD_SESSION_LOG_SHUTDOWN_PER_PHASE_FLUSH=1`** restores per-phase `Flush` for hang forensics mid-teardown.
- **`main::Shutdown`**: single quiet **`Flush("session_log.json", false)`** after `main: end` so a normal quit still persists all shutdown phases.

## Tradeoff

If the process dies **before** the final flush, recent `shutdown_phase` lines may be missing from disk unless per-phase env is set.
