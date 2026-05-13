#include "ToolUserErrorFeedback.hpp"

#include "rendering/color.hpp"
#include "imgui.h"

float DrawToolUserErrorCopyBlock(float x0, float y, float w, float pad, ImFont *bodyFont,
                                 const std::string &code, const std::string &message,
                                 const std::string &relatedParameterLabel, const char *imguiIdScope)
{
    if (code.empty() && message.empty())
        return 0.f;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = bodyFont ? bodyFont : ImGui::GetFont();
    const float fs = font ? font->FontSize : ImGui::GetFontSize();
    const float wrapW = std::max(1.0f, w - 2.0f * pad);
    const std::string clip = "[" + code + "] " + message;
    const float lh = font ? font->CalcTextSizeA(fs, FLT_MAX, 0.0f, "Mg").y : ImGui::GetTextLineHeight();

    const float titleBlock = relatedParameterLabel.empty() ? 0.f : lh + 3.f;
    const ImVec2 clipSz =
        font ? font->CalcTextSizeA(fs, wrapW, 0.0f, clip.c_str()) : ImGui::CalcTextSize(clip.c_str());
    const float blockH = titleBlock + clipSz.y + pad;

    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    ImGui::PushID(imguiIdScope ? imguiIdScope : "toolUserErr");
    ImGui::InvisibleButton("copyErr", ImVec2(w, blockH));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    ImGui::PopID();
    if (clicked && !clip.empty())
        ImGui::SetClipboardText(clip.c_str());
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (hovered)
    {
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        glm::vec4 hac = Color::GetAccent(1, 0.12f, 1.0f);
        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImVec4(hac.r, hac.g, hac.b, hac.a)), 4.0f);
    }

    float yc = y;
    if (!relatedParameterLabel.empty())
    {
        glm::vec4 ac = Color::GetAccent(2, 1.0f, 1.15f);
        ImU32 pc = ImGui::GetColorU32(ImVec4(ac.r, ac.g, ac.b, ac.a));
        dl->AddText(font, fs, ImVec2(x0 + pad, yc), pc, relatedParameterLabel.c_str());
        yc += lh + 3.f;
    }
    glm::vec4 tc = Color::GetUIText(1);
    ImU32 mc = ImGui::GetColorU32(ImVec4(tc.r, tc.g, tc.b, tc.a));
    dl->AddText(font, fs, ImVec2(x0 + pad, yc), mc, clip.c_str(), nullptr, wrapW);
    return blockH;
}
