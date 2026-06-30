#include "AppWakeEvent.hpp"
#include "utils/log.hpp"

#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <limits>

namespace
{
std::atomic<uint32_t> g_wakeEventType{0};
}

namespace AppWakeEvent
{
void RegisterEventType()
{
    const Uint32 type = SDL_RegisterEvents(1);
    if (type == std::numeric_limits<Uint32>::max())
    {
        LOG_SESSION("AppWakeEvent", "RegisterEventType failed");
        return; // registration failed; Push() stays a no-op
    }
    g_wakeEventType.store(type, std::memory_order_relaxed);
    LOG_SESSION("AppWakeEvent", "registered type", type);
}

void Push()
{
    const uint32_t type = g_wakeEventType.load(std::memory_order_relaxed);
    if (type == 0)
    {
        LOG_SESSION("AppWakeEvent", "Push skipped: not registered");
        return;
    }
    SDL_Event event{};
    event.type = type;
    const bool ok = SDL_PushEvent(&event);
    LOG_SESSION("AppWakeEvent", "Push", "ok", ok);
}
}
