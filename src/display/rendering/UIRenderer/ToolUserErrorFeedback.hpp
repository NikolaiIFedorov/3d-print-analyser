#pragma once

#include <string>

struct ImFont;

/// User-facing tool error for panels (Calibrate, Structure, …): stable `code`, readable `message`,
/// optional `relatedParameterLabel` shown as a header line. Used with `DrawToolUserErrorCopyBlock`.
struct ToolUserErrorPayload
{
    std::string code;
    std::string message;
    std::string relatedParameterLabel;
    friend bool operator==(const ToolUserErrorPayload &, const ToolUserErrorPayload &) = default;
};

/// Clickable block: copies `[code] message` to the clipboard. Returns total height used (px).
[[nodiscard]] float DrawToolUserErrorCopyBlock(float x0, float y, float w, float pad, ImFont *bodyFont,
                                               const std::string &code, const std::string &message,
                                               const std::string &relatedParameterLabel,
                                               const char *imguiIdScope = "toolUserErr");
