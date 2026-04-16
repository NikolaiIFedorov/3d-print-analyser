#pragma once

namespace SystemAccent
{
    // Fills hue (0–360) and sat (0–1) with the OS accent color if available.
    // Returns true on success; on failure the output values are left unchanged
    // so callers can safely fall back to their own defaults.
    bool GetHueSat(float &hue, float &sat);
}
