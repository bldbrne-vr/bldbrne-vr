#pragma once

#include "controls_guide.h"
#include "controller_art.h"
#include "hunter_theme.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace ControlsGuide {

struct DiagramSelection {
    int button{-1};
    Platform platform{Platform::Quest};
};

// Controller diagrams use embedded artwork and the game mapping tables.
inline void DrawDiagram(Platform platform, const std::vector<DiagramButton>& buttons,
                        float scale, ImVec4 accent,
                        const std::array<ControllerArt, 2>& artwork, DiagramSelection& selection,
                        const HunterTheme::Art& theme) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * scale, 4 * scale});
    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = available >= 550 * scale ? 2 : 1;
    const ImU32 ink = ImGui::GetColorU32(accent);
    const ImU32 text = ImGui::GetColorU32(ImVec4{.84f, .83f, .79f, 1});
    int hovered = -1;
    int& selected = selection.button;
    if (selection.platform != platform || selected < 0 || selected >= static_cast<int>(buttons.size()))
        selected = buttons.empty() ? -1 : 0;
    selection.platform = platform;
    if (ImGui::BeginTable("controller-art", columns, ImGuiTableFlags_SizingStretchSame)) {
        for (int hand = 0; hand < 2; ++hand) {
            ImGui::TableNextColumn();
            ImGui::PushID(hand);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = 278 * scale;
            auto* draw = ImGui::GetWindowDrawList();
            HunterTheme::InsetPanel(draw,origin,{origin.x+width,origin.y+height},theme,scale);
            draw->AddLine({origin.x+16*scale,origin.y+34*scale},
                          {origin.x+width-16*scale,origin.y+34*scale}, IM_COL32(62,63,59,255));
            draw->AddText({origin.x + 16 * scale, origin.y + 12 * scale}, ink,
                          hand ? "RIGHT HAND" : "LEFT HAND");
            const auto& image = artwork[hand];
            const float ratio = image.height ? float(image.width) / image.height : 1.0f;
            const float art_width = std::clamp(width*.46f,118*scale,160*scale);
            const float image_height = std::min(226 * scale, art_width / ratio);
            const float image_width = image_height * ratio;
            const ImVec2 top{origin.x + 10 * scale + (art_width - image_width) / 2,
                             origin.y + 40 * scale + (226 * scale - image_height) / 2};
            const auto point = [&](float x, float y) {
                return ImVec2{top.x + x / 240 * image_width, top.y + y / 440 * image_height};
            };
            if (image.view)
                draw->AddImage((ImTextureID)(intptr_t)image.view.Get(), top,
                               {top.x + image_width, top.y + image_height},
                               {0,0},{1,1},IM_COL32(222,213,192,255));
            int row = 0;
            const float label_x = origin.x + art_width + 24 * scale;
            const float label_width = std::max(30.0f, origin.x + width - label_x - 12 * scale);
            for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
                const auto& button = buttons[i];
                if (button.right != (hand == 1)) continue;
                ImGui::PushID(i);
                const auto pos = point(button.x, button.y);
                const ImVec2 label{label_x, origin.y + (44 + row * 37) * scale};
                ImGui::SetCursorScreenPos({pos.x - 4 * scale, pos.y - 4 * scale});
                if (ImGui::InvisibleButton("button", {8 * scale, 8 * scale})) selected = i;
                bool over = ImGui::IsItemHovered();
                const bool tip_pin = over && HunterTheme::TooltipHovered();
                ImGui::SetCursorScreenPos(label);
                if (ImGui::InvisibleButton("mapping", {label_width, 36 * scale},
                                           ImGuiButtonFlags_EnableNav)) selected = i;
                const bool over_label = ImGui::IsItemHovered();
                const bool tip_label = over_label && HunterTheme::TooltipHovered();
                over |= over_label;
                const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
                if (over) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (over || focused) hovered = i;
                const bool active = over || focused || selected == i;
                if (active) {
                    draw->AddRectFilled({label.x - 5 * scale, label.y - 2 * scale},
                                        {label.x + label_width, label.y + 35 * scale},
                                        over ? IM_COL32(64,31,32,255) : IM_COL32(47,22,24,255), 2 * scale);
                    draw->AddLine(pos, {label.x - 6 * scale, label.y + 13 * scale}, ink, scale);
                }
                if (active) {
                    draw->AddCircle(pos, 8 * scale, ink, 24, 1.5f * scale);
                    draw->AddCircleFilled(pos, 2 * scale, ink);
                }
                draw->PushClipRect(label, {label.x + label_width, label.y + 36 * scale}, true);
                draw->AddText(label, ink, button.line.control.c_str());
                std::string title = button.line.title;
                while (title.size() > 3 && ImGui::CalcTextSize((title + "...").c_str()).x > label_width) {
                    size_t last = title.size() - 1;
                    while (last > 0 && (static_cast<unsigned char>(title[last]) & 0xc0) == 0x80) --last;
                    title.erase(last);
                }
                if (title != button.line.title) title += "...";
                draw->AddText({label.x, label.y + 17 * scale}, text, title.c_str());
                draw->PopClipRect();
                if (focused) draw->AddRect({label.x-5*scale,label.y-2*scale},
                    {label.x+label_width,label.y+35*scale}, ink, 2*scale);
                if (tip_pin || tip_label)
                    HunterTheme::Tooltip(scale,button.line.help.c_str(),button.line.title.c_str(),
                                         button.line.sub.c_str());
                ImGui::PopID();
                ++row;
            }
            ImGui::SetCursorScreenPos(origin);
            ImGui::Dummy({width, height});
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    const int detail = hovered >= 0 ? hovered : selected;
    ImGui::PushTextWrapPos(0);
    if (detail >= 0 && detail < static_cast<int>(buttons.size())) {
        const auto& line = buttons[detail].line;
        const std::string title = line.control + "  /  " + line.title +
                                  (line.sub.empty() ? "" : "  (" + line.sub + ")");
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {.078f,.073f,.066f,1});
        ImGui::PushStyleColor(ImGuiCol_Border, {.29f,.25f,.19f,1});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10*scale,8*scale});
        if (ImGui::BeginChild("selected-control", {0,0},
                ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                ImGuiChildFlags_AlwaysUseWindowPadding)) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(accent, "%s", title.c_str());
            ImGui::TextUnformatted(line.help.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
}
} // namespace ControlsGuide
