#include "SystemAccent.hpp"

#import <AppKit/AppKit.h>
#include <cmath>

namespace SystemAccent
{
    bool GetHueSat(float &hue, float &sat)
    {
        NSColor *accent = [[NSColor controlAccentColor]
                           colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
        if (!accent)
            return false;

        float r = static_cast<float>([accent redComponent]);
        float g = static_cast<float>([accent greenComponent]);
        float b = static_cast<float>([accent blueComponent]);

        float cMax = std::fmax(r, std::fmax(g, b));
        float cMin = std::fmin(r, std::fmin(g, b));
        float delta = cMax - cMin;

        if (delta < 1e-6f)
        {
            // Achromatic — no meaningful hue, leave caller defaults
            return false;
        }

        float h;
        if (cMax == r)
            h = 60.0f * std::fmod((g - b) / delta, 6.0f);
        else if (cMax == g)
            h = 60.0f * ((b - r) / delta + 2.0f);
        else
            h = 60.0f * ((r - g) / delta + 4.0f);

        if (h < 0.0f)
            h += 360.0f;

        float l = (cMax + cMin) * 0.5f;
        float s = delta / (1.0f - std::abs(2.0f * l - 1.0f));

        hue = h;
        sat = s;
        return true;
    }
}
