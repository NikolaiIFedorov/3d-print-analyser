#pragma once

#include <functional>

namespace SystemAppearance
{
    // Returns true if the current system appearance is dark.
    bool IsDark();

    // Register a callback to be invoked on the main thread whenever the system
    // appearance changes (e.g. user switches Light ↔ Dark in System Settings).
    void SetChangeCallback(std::function<void()> cb);

    // Remove the registered callback (call from Display::Shutdown).
    void ClearChangeCallback();
}
