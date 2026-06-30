#pragma once

/// When `CAD_CRASH_BACKTRACE=1`, print a stack trace to stderr on SIGSEGV/SIGABRT before exit.
void InstallCrashBacktraceIfEnabled();
