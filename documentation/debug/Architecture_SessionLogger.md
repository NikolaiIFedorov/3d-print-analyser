# Architecture: SessionLogger — Structured Session Event Log

## Executive Summary

`SessionLogger` is a lightweight, in-process structured event log. It buffers meaningful application events in memory during a session, then writes them as a single JSON file (`session_log.json`) to the build directory on shutdown. Each event is also echoed to the terminal via `Log::Session`. A companion `SessionState` struct tracks a live snapshot of scene geometry, analysis parameters, flaw counts, and camera state so that the `bug_marker` event can capture a complete application snapshot at any point. The system is ~200 lines across 2 files.

**Verdict: Approve with changes**

**Top 3 strengths:**
1. Zero-overhead when not flushing — events are stored as plain structs in a `std::vector`; no I/O on the hot path.
2. `bug_marker` event captures a full-state snapshot (geometry, params, flaws, camera) in one keypress, making bug reproduction straightforward.
3. Dual output — every event is immediately echoed to the terminal via `Log::Session` for live monitoring, while the JSON file provides the full post-mortem log.

**Top 3 concerns:**
1. `SessionLogger` is a Meyer's singleton — global mutable state that cannot be tested in isolation or run with different configurations.
2. `Flush` writes to a hardcoded relative path (`"session_log.json"` in the build directory) — the path is decided at the call site in `main.cpp` with no mechanism to redirect it.
3. `SessionState` must be manually updated by callers before every log call — there is no automatic sync, so stale state is silently written if a caller forgets to update it.

---

## 1. Requirements & Motivation

### Functional Requirements
- Record the sequence of significant user actions during a session: app start, file import, analysis run, parameter change, bug marker.
- Capture a full snapshot of application state on demand (bug marker).
- Persist the log to a JSON file at session end for post-mortem debugging.
- Echo every event to the terminal in real time for live monitoring.

### Non-Functional Requirements
- No I/O on the per-frame hot path — all writes are buffered in memory.
- Single flush at shutdown — one file write per session.
- Human-readable JSON output that can be inspected without tooling.

### Constraints
- C++23, macOS, single-threaded — no concurrent logging from multiple threads.
- No third-party JSON library — serialization is hand-written.
- Output path is relative to the working directory at `Flush` time (the build directory during development).

---

## 2. Solution Description

### Components

| Component | Files | Responsibility |
|-----------|-------|----------------|
| **SessionLogger** | `SessionLogger.hpp/cpp` | Meyer's singleton. Owns the `events` buffer and `startTime`. Provides event logger methods, `Start()`, and `Flush()`. |
| **SessionState** | `SessionLogger.hpp` | Plain mutable struct. Holds the live snapshot: scene geometry counts, last filename/format, analysis parameters, flaw counts, camera target + ortho size. Callers write to it before calling a log method. |
| **Event** | `SessionLogger.hpp` (private) | Internal record: `t_ms` (elapsed milliseconds from `startTime`), `type` string, and a `vector<pair<key, json_value>>` field list. Fields store pre-serialized JSON value strings. |

### Event Types

| Event `type` | Trigger | Key `data` fields |
|---|---|---|
| `app_start` | `SessionLogger::Start()` — called from `Init()` in `main.cpp` | *(none)* |
| `file_import` | `Display::DoFileImport()` after a file is loaded | `filename`, `format`, `points`, `edges`, `faces`, `solids` |
| `analysis_run` | `Display::Frame()` after `Analysis::AnalyzeScene()` completes | `overhangs`, `sharp_edges`, `thin_sections`, `small_features`, all 5 analysis params |
| `param_change` | `Display::Frame()` when a DragFloat analysis parameter is changed | `param` (name), `value` |
| `bug_marker` | `Display::MarkBug()` — triggered by pressing `/` in `Input.cpp` | Full snapshot: geometry counts, last file, all analysis params, all flaw counts, `camera_target`, `camera_ortho_size` |

### State Management

`SessionState` is a public member of `SessionLogger`. Callers are responsible for writing to it before calling a log method:

```
// File import (Display::DoFileImport)
sl.state.lastFilename = filename;
sl.state.lastFormat   = lower;
sl.state.points       = scene->points.size();
// ... etc
sl.LogFileImport(filename, lower);   // reads state, builds event

// Analysis run (Display::Frame)
sl.state.overhangs    = overhangs;
// ... etc
sl.LogAnalysisRun();                 // reads state, builds event

// Param change (Display::Frame DragFloat onChange)
sl.state.overhangAngle = this->overhangAngle;
// ... etc
sl.LogParamChange(paramName, value); // param + value are passed directly

// Bug marker (Display::MarkBug)
sl.state.cameraTarget    = camera.target;
sl.state.cameraOrthoSize = camera.orthoSize;
sl.LogBugMarker();                   // reads full state
```

### Data Flow

```
main.cpp Init()
└── SessionLogger::Instance().Start()
    ├── startTime = steady_clock::now()
    └── PushEvent("app_start", {})

[user imports file] → Display::DoFileImport()
└── sl.state.* = ...
    └── LogFileImport() → PushEvent("file_import", {...})
                        → Log::Session("File imported: ...")

[scene dirty] → Display::Frame() → Analysis::AnalyzeScene()
└── sl.state.* = flaw counts
    └── LogAnalysisRun() → PushEvent("analysis_run", {...})
                         → Log::Session("Analysis: ...")

[user drags param] → Display::Frame() DragFloat onChange
└── sl.state.* = updated params
    └── LogParamChange(name, v) → PushEvent("param_change", {...})
                                 → Log::Session("Param changed: ...")

[user presses /] → Input.cpp → Display::MarkBug()
└── sl.state.cameraTarget = ...
    └── LogBugMarker() → PushEvent("bug_marker", {...})
                       → Log::Session("BUG MARKER — ...")

main.cpp Shutdown()
└── SessionLogger::Instance().Flush("session_log.json")
    ├── SerializeJson() → build JSON string
    └── std::ofstream("session_log.json") << json
```

### JSON Output Format

```json
{
  "session_start": "2026-04-21T14:32:10Z",
  "events": [
    { "t_ms": 0, "type": "app_start" },
    { "t_ms": 3201, "type": "file_import", "data": {
        "filename": "part.stl", "format": "stl",
        "points": "1024", "edges": "3072", "faces": "1024", "solids": "1"
    }},
    { "t_ms": 3250, "type": "analysis_run", "data": {
        "overhangs": "3", "sharp_edges": "12",
        "thin_sections": "0", "small_features": "1",
        "overhang_angle": "45.00", "sharp_corner_angle": "100.00",
        "thin_min_width": "2.00", "min_feature_size": "0.40", "layer_height": "0.20"
    }},
    { "t_ms": 4800, "type": "param_change", "data": { "param": "overhang_angle", "value": "50.00" }},
    { "t_ms": 9100, "type": "bug_marker", "data": {
        "points": "1024", "edges": "3072", "faces": "1024", "solids": "1",
        "last_file": "part.stl", "format": "stl",
        "overhang_angle": "50.00", "sharp_corner_angle": "100.00",
        "thin_min_width": "2.00", "min_feature_size": "0.40", "layer_height": "0.20",
        "overhangs": "2", "sharp_edges": "12", "thin_sections": "0", "small_features": "1",
        "camera_target": "[0.00, 0.00, 10.00]", "camera_ortho_size": "25.00"
    }}
  ]
}
```

### Key Implementation Details

- **`ElapsedMs()`**: Computes milliseconds since `startTime` using `steady_clock`. Monotonic — not affected by wall-clock adjustments.
- **`SerializeJson()`**: Hand-written serializer. Produces an indented, human-readable JSON object. Field values are pre-serialized strings stored in `Event::fields` — strings are stored with quotes already escaped, numbers without.
- **`EscapeStr()`**: Escapes `"`, `\`, `\n`, `\r`, `\t` in user-provided strings (filenames) before embedding in JSON.
- **`Fmt(float)`**: Serializes floats to 2 decimal places via `std::fixed << std::setprecision(2)`.
- **`Log::Session`**: Echoes each event summary to the terminal with `Level::SESSION` formatting. Independent of the JSON buffer.

---

## 3. Design Principles

### Conforming

| Principle | Evidence |
|-----------|----------|
| **SRP** | `SessionLogger` handles buffering and serialization only; `SessionState` holds state; callers own the logic that decides when to log. |
| **Zero-overhead on hot path** | `PushEvent` appends to a `std::vector` — O(1) amortized, no I/O. `Flush` is called once at shutdown. |
| **String safety** | `EscapeStr` is applied to all user-supplied string fields. Numeric fields are formatted via `Fmt` or `std::to_string` — no injection risk. |

### Deliberately Relaxed

| Principle | Deviation | Justification |
|-----------|-----------|---------------|
| **DIP** | Singleton accessed via `Instance()` everywhere | Convenient for a utility that must be reachable from `Display`, `Input`, and `main` without threading a reference through every call chain. |
| **Encapsulation** | `SessionState state` is a public member | Callers must write directly to individual fields before logging. Making it private would require either individual setters or a large parameter list on each log method. Public struct is the pragmatic tradeoff. |

---

## 4. Alternatives Considered

| Decision | Current Approach | Alternative | Verdict |
|----------|-----------------|-------------|---------|
| Output format | Hand-written JSON | `nlohmann/json` or `miniz_zip` | No dependency needed for a simple flat log. Hand-written is sufficient and transparent. |
| Flush strategy | Single flush at shutdown | Flush-per-event to file | Per-event I/O would add latency on every action. Single flush is safe for normal shutdown; data is lost on crash. |
| State management | Manual `sl.state.* = ...` before each log call | Auto-sync via observer/callback | Auto-sync would couple `SessionLogger` to `Display`, `Scene`, and `Camera`. Manual sync keeps the logger dependency-free. |
| Output location | Relative path `"session_log.json"` | Absolute path in user data directory | Relative to the working directory (build dir during development) is simple and always accessible. Can be changed at the `Flush` call site. |
| Bug trigger | `/` key (SDL_SCANCODE_SLASH) | Menu button or hotkey combination | Slash is easy to reach one-handed while the other controls the mouse. Single key is faster than a combination for in-session bug marking. |

---

## 5. Technology & Dependencies

| Library | Role |
|---------|------|
| `<chrono>` | `steady_clock` for elapsed milliseconds; `system_clock` for the ISO 8601 wall-clock timestamp |
| `<fstream>` | Writing the JSON file |
| `<sstream>` / `<iomanip>` | Float formatting (`Fmt`) and `fmtVec3` in `LogBugMarker` |
| `<ctime>` / `gmtime_r` | UTC wall-clock formatting for `session_start` |
| `Log::Session` | Terminal echo for each event |
| GLM | `glm::vec3` for camera state in `SessionState` |

No third-party JSON, logging, or serialization libraries.

---

## 6. Tradeoffs

| Decision | Chosen Approach | What's Sacrificed | Rationale |
|----------|----------------|-------------------|-----------|
| In-memory buffer | All events in `std::vector<Event>` | Data lost on crash | Crash-safe logging would require file-per-event or a memory-mapped ring buffer. For a debug aid, in-memory is sufficient. |
| Manual state sync | Callers update `sl.state.*` before logging | Guaranteed freshness | Any field not updated before a log call will reflect the value from the last update. Accepted tradeoff for zero coupling. |
| Single output file | `session_log.json` overwritten each run | Session history | Only the most recent session is preserved. Sufficient for debugging the last run; rotate files if history is needed. |
| Relative path | Resolved at `Flush` time (build dir) | User-facing log location | Works for development. Would need an absolute path for a shipped application. |
| Hand-rolled JSON | `EscapeStr` + `Fmt` + `std::to_string` | Schema validation, nesting | The log is flat (one level of `data`), making hand-rolling correct and trivial. |

---

## 7. Best Practices Compliance

### Conforming
- **No hot-path I/O**: `PushEvent` is pure memory; `Flush` is called once.
- **JSON string safety**: `EscapeStr` handles the five JSON-unsafe characters in user-provided filenames.
- **Monotonic timing**: `ElapsedMs()` uses `steady_clock` — correct even across daylight saving changes.
- **UTC timestamp**: `session_start` uses `gmtime_r` for a portable UTC ISO 8601 string.
- **RAII-compatible**: `SessionLogger` default-constructs cleanly; `Flush` can be called from any scope.

### Non-Conforming

| Issue | Severity | Details | Remediation |
|-------|----------|---------|-------------|
| Singleton | Minor | `Instance()` is globally accessible mutable state. Cannot run two loggers simultaneously or unit-test without side effects. | Accept `SessionLogger&` as a parameter at call sites, or use a thin factory function. |
| Manual state sync | Minor | Callers must update `sl.state.*` before every log call. A forgotten update silently logs stale values. | Wrap `LogAnalysisRun` and `LogFileImport` to accept their data as parameters, removing the intermediate mutable state. |
| Crash safety | Minor | In-memory buffer is lost on abnormal termination. | Acceptable for a debug tool. If crash safety is needed, write a line-delimited JSON file per event. |
| Hardcoded decimal precision | Minor | `Fmt` always uses 2 decimal places for all float fields, including angles (e.g., `100.00°`) and sizes (e.g., `0.40mm`). | Acceptable for the current scale; could accept a precision parameter if sub-millimetre resolution is needed. |

---

## 8. Risks & Future-Proofing

### Risks

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| Stale `SessionState` | Low — a log event records outdated values silently | Medium — easy to forget an update when adding a new log call | Wrap log methods to accept state as parameters; deprecate `SessionState` as intermediate. |
| Crash before `Flush` | Low — entire session log is lost | Low (app is stable) | If crash safety becomes important, switch to append-mode line-delimited JSON. |
| Large session | Negligible — `events` grows at human interaction speed | Negligible | Even 10,000 events is under 1 MB. |

### Future Considerations
- **Ship-mode path**: Replace the relative `"session_log.json"` with a path inside the OS user data directory for non-development builds.
- **Session rotation**: Keep the last N session logs (rename to `session_log_1.json`, etc.) to preserve history across runs.
- **Crash-safe variant**: Switch to `std::ofstream` append mode with line-delimited JSON events, flushed per event, for reliability during development of unstable features.
- **Remove `SessionState` intermediary**: Pass event data directly to log methods to eliminate the manual sync step and the possibility of stale values.

---

## 9. Recommendations

### Should-Have
1. **Remove `SessionState` intermediary** — pass scene counts, flaw counts, and analysis params as direct parameters to `LogFileImport` and `LogAnalysisRun`. This eliminates the stale-value footgun and makes each call self-contained.
2. **Absolute output path** — use an OS-appropriate user data directory instead of a relative path so the log is findable in non-development contexts.

### Nice-to-Have
3. **Session rotation** — on `Start()`, rename any existing `session_log.json` to `session_log_prev.json` so the previous session is not overwritten.
4. **Decouple from singleton** — thread `SessionLogger&` through `Display` and `Input` constructors for testability.
