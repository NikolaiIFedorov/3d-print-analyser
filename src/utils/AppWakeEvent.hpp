#pragma once

// Lets a background thread wake a blocked SDL_WaitEventTimeout immediately when work
// completes, instead of the main loop only noticing on the next real input event.
namespace AppWakeEvent
{
/// Call once after SDL_Init, before any background task can complete.
void RegisterEventType();

/// Thread-safe. No-op if RegisterEventType() has not run yet.
void Push();
}
