#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

// Length units for display and user input. Canonical scene / analysis values stay in millimeters.
enum class LengthUnit : int8_t
{
    Millimeter = 0,
    Centimeter = 1,
    Inch = 2,
    Foot = 3,
};

inline constexpr float kLengthMillimetersPerInch = 25.4f;
inline constexpr float kLengthMillimetersPerFoot = 12.0f * kLengthMillimetersPerInch;

inline constexpr LengthUnit LengthUnitFromIndex(int i)
{
    switch (i)
    {
    case 0:
        return LengthUnit::Millimeter;
    case 1:
        return LengthUnit::Centimeter;
    case 2:
        return LengthUnit::Inch;
    case 3:
        return LengthUnit::Foot;
    default:
        return LengthUnit::Millimeter;
    }
}

inline constexpr int LengthUnitToIndex(LengthUnit u)
{
    return static_cast<int>(u);
}

inline constexpr float MillimetersPerUnit(LengthUnit u)
{
    switch (u)
    {
    case LengthUnit::Millimeter:
        return 1.0f;
    case LengthUnit::Centimeter:
        return 10.0f;
    case LengthUnit::Inch:
        return kLengthMillimetersPerInch;
    case LengthUnit::Foot:
        return kLengthMillimetersPerFoot;
    }
    return 1.0f;
}

inline constexpr float ToMillimeters(float valueInUnit, LengthUnit u)
{
    return valueInUnit * MillimetersPerUnit(u);
}

inline constexpr float FromMillimeters(float mm, LengthUnit u)
{
    const float d = MillimetersPerUnit(u);
    return d > 0.0f ? mm / d : mm;
}

inline constexpr const char *LengthUnitAbbreviation(LengthUnit u)
{
    switch (u)
    {
    case LengthUnit::Millimeter:
        return "mm";
    case LengthUnit::Centimeter:
        return "cm";
    case LengthUnit::Inch:
        return "in";
    case LengthUnit::Foot:
        return "ft";
    default:
        return "mm";
    }
}

// e.g. "25 mm", "1 in"
inline void FormatLengthMmForDisplay(char *buf, size_t cap, float mm, LengthUnit displayAs)
{
    if (!buf || cap == 0)
        return;
    const float v = FromMillimeters(mm, displayAs);
    const char *abbr = LengthUnitAbbreviation(displayAs);
    if (displayAs == LengthUnit::Millimeter)
        std::snprintf(buf, cap, "%.2f %s", static_cast<double>(v), abbr);
    else if (displayAs == LengthUnit::Centimeter)
        std::snprintf(buf, cap, "%.3f %s", static_cast<double>(v), abbr);
    else
        std::snprintf(buf, cap, "%.4f %s", static_cast<double>(v), abbr);
}

inline bool IsAsciiSpace(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

inline void TrimAsciiInPlace(std::string_view &s)
{
    while (!s.empty() && IsAsciiSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && IsAsciiSpace(s.back()))
        s.remove_suffix(1);
}

inline bool EndsWithIgnoreCase(std::string_view s, std::string_view suffix)
{
    if (suffix.size() > s.size())
        return false;
    const size_t off = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        const char a = s[off + i];
        const char b = suffix[i];
        if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b)))
            return false;
    }
    return true;
}

inline bool TryParseLengthToMm(std::string_view s, LengthUnit defaultUnit, float &outMm)
{
    TrimAsciiInPlace(s);
    if (s.empty())
        return false;
    if (s.find(',') != std::string_view::npos)
        return false;

    LengthUnit unit = defaultUnit;
    if (EndsWithIgnoreCase(s, "mm"))
    {
        unit = LengthUnit::Millimeter;
        s.remove_suffix(2);
    }
    else if (EndsWithIgnoreCase(s, "cm"))
    {
        unit = LengthUnit::Centimeter;
        s.remove_suffix(2);
    }
    else if (EndsWithIgnoreCase(s, "ft"))
    {
        unit = LengthUnit::Foot;
        s.remove_suffix(2);
    }
    else if (EndsWithIgnoreCase(s, "in"))
    {
        unit = LengthUnit::Inch;
        s.remove_suffix(2);
    }

    TrimAsciiInPlace(s);
    if (s.empty())
        return false;

    char numBuf[128];
    if (s.size() + 1 > sizeof(numBuf))
        return false;
    std::memcpy(numBuf, s.data(), s.size());
    numBuf[s.size()] = '\0';

    char *endPtr = nullptr;
    double v = std::strtod(numBuf, &endPtr);
    if (!endPtr || endPtr == numBuf)
        return false;
    while (*endPtr && IsAsciiSpace(*endPtr))
        ++endPtr;
    if (*endPtr != '\0')
        return false;

    if (!std::isfinite(v))
        return false;
    outMm = ToMillimeters(static_cast<float>(v), unit);
    return std::isfinite(outMm);
}
