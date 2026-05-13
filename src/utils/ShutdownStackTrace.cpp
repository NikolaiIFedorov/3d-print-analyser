#include "utils/ShutdownStackTrace.hpp"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)

void ShutdownStackTraceLogIfEnabled(const char * /*tag*/)
{
}

#else

#include <execinfo.h>
#include <unistd.h>

void ShutdownStackTraceLogIfEnabled(const char *tag)
{
    const char *e = std::getenv("CAD_SHUTDOWN_STACK_TRACE");
    if (e == nullptr || e[0] == '\0' || e[0] == '0')
        return;

    static const char kHdr[] = "\n[CAD_SHUTDOWN_STACK_TRACE] ";
    (void)write(STDERR_FILENO, kHdr, sizeof(kHdr) - 1u);
    if (tag != nullptr)
        (void)write(STDERR_FILENO, tag, std::strlen(tag));
    (void)write(STDERR_FILENO, "\n", 1);

    void *buf[48];
    const int n = backtrace(buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    if (n > 0)
        backtrace_symbols_fd(buf, n, STDERR_FILENO);
    (void)write(STDERR_FILENO, "\n", 1);
}

#endif
