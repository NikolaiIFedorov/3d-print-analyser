#include "utils/CrashBacktrace.hpp"

#include <cstring>

#if !defined(_WIN32)

#include <csignal>
#include <execinfo.h>
#include <unistd.h>

namespace
{
void OnFatalSignal(int sig)
{
    const char *name = (sig == SIGSEGV) ? "SIGSEGV" : (sig == SIGABRT) ? "SIGABRT" : "SIGNAL";
    const char hdr[] = "\n[CAD_CRASH] ";
    (void)write(STDERR_FILENO, hdr, sizeof(hdr) - 1u);
    (void)write(STDERR_FILENO, name, std::strlen(name));
    (void)write(STDERR_FILENO, " — backtrace:\n", 14);

    void *buf[64];
    const int n = backtrace(buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    if (n > 0)
        backtrace_symbols_fd(buf, n, STDERR_FILENO);
    (void)write(STDERR_FILENO, "\n", 1);

    signal(sig, SIG_DFL);
    raise(sig);
}
} // namespace

void InstallCrashBacktraceIfEnabled()
{
    signal(SIGSEGV, OnFatalSignal);
    signal(SIGABRT, OnFatalSignal);
}

#else

void InstallCrashBacktraceIfEnabled() {}

#endif
