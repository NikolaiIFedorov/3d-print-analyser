# Log: do not hold mutex across stdout/stderr

## Problem

With Structure + CGAL failures, the worker emits **`LOG_WARN`** while the terminal is often **saturated** (CGAL stderr, ANSI). **`Log::Write`** held **`gLogWriteMutex`** for the entire **`std::cout << output`**. If `cout` blocks on TTY backpressure, the **worker keeps the mutex**. The **main** thread then blocks on the **next** `Log::*` (e.g. `LOG_DESC` after `PollStructureStagingTaskIfReady`, or other UI/session logging) → app appears frozen; force quit.

## Approach

Build the formatted line **under the lock**, **release**, then write to `cout`/`cerr`, then **re-lock** only for **`UpdateGlobals`**.

## Tradeoff

Repeat / cursor-up suppression (`lastMsg` / `lastLevel`) can race slightly between threads; acceptable vs deadlock.

## Files

- `src/utils/log.cpp`

## Outcome

`CAD_OpenGL` builds cleanly.
