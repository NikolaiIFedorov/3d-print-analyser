#pragma once

/// When `CAD_SHUTDOWN_STACK_TRACE` is set to a non-empty value other than `0`, writes the **current
/// thread's** stack to stderr with a short tag (uses `backtrace` / `backtrace_symbols_fd` where
/// available). Intended for narrowing quit freezes: compare main-thread stacks across shutdown
/// phases vs structure-worker stacks around CGAL failures. Off by default.
void ShutdownStackTraceLogIfEnabled(const char *tag);
