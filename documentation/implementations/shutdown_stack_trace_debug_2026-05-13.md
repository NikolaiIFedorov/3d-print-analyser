# Shutdown stack trace debug hook (2026-05-13)

## Usage

```bash
export CAD_SHUTDOWN_STACK_TRACE=1
./build/CAD_OpenGL
```

When set (non-empty, not `0`), the app writes **current-thread** stacks to **stderr** at:

- `main::Shutdown` entry, before/after `Display::Shutdown`, before final session log flush  
- `Display::Shutdown` entry, after structure cancel+abandon, before `renderer.Shutdown`, after GL context destroy, before return  
- Structure worker: job entered, cancel between solids, `TryApplyStructureCarve` returned false, cancel after failed TryApply, job complete  

**Compare:** last main-thread tag printed before freeze vs worker tags (did worker log “returned false” and “job complete”?). Symbols need a debug build / `-rdynamic` on Linux for best names.

## Files

- `src/utils/ShutdownStackTrace.{hpp,cpp}` — `execinfo` / `backtrace_symbols_fd` (non-Windows stub).  
- `src/main.cpp`, `src/display/display.cpp` — call sites.  
- `CMakeLists.txt` — added `ShutdownStackTrace.cpp`.
