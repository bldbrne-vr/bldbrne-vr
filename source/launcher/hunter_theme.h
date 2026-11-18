#pragma once

#include "embedded_image.h"
#include "resource.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// Static artwork and ordinary ImGui drawing only: no timers, offscreen passes or shaders.
// App owns the COM references; missing artwork falls back to plain, functional controls.
namespace HunterTheme {
inline constexpr ImVec4 Crimson{.25f,.055f,.06f,1};
inline constexpr float ScrollbarGutter = 4.f;
inline void PushAccent() {
    ImGui::PushStyleColor(ImGuiCol_Button, Crimson);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.38f,.10f,.10f,1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {.17f,.035f,.04f,1});
    ImGui::PushStyleColor(ImGuiCol_Text, {.91f,.87f,.79f,1});
}
inline void PushUpdateAccent() {
    ImGui::PushStyleColor(ImGuiCol_Button, {.80f,.63f,.36f,1});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.92f,.76f,.48f,1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {.67f,.51f,.29f,1});
    ImGui::PushStyleColor(ImGuiCol_Text, {.045f,.035f,.024f,1});
}

// The launcher sleeps between events. Only request another frame while a tooltip is waiting.
inline bool tooltip_pending = false;
inline bool tooltip_shown = false;

inline void BeginTooltipFrame() {
    tooltip_pending = false;
    tooltip_shown = false;
}

inline bool TooltipHovered() {
    constexpr auto flags = ImGuiHoveredFlags_AllowWhenDisabled;
    if (tooltip_shown || !ImGui::IsItemHovered(flags))
        return false;
    const bool ready = ImGui::IsItemHovered(flags | ImGuiHoveredFlags_DelayShort |
                                           ImGuiHoveredFlags_NoSharedDelay);
    tooltip_pending |= !ready;
    return ready;
}

inline void Tooltip(float scale, const char* text, const char* title = nullptr,
                    const char* subtitle = nullptr) {
    if (tooltip_shown)
        return;
    tooltip_shown = true;
    tooltip_pending = false;

    ImGui::PushFont(nullptr);
    const ImVec2 padding{10.f*scale,8.f*scale};
    const auto* viewport = ImGui::GetMainViewport();
    const ImVec2 safe_min{viewport->WorkPos.x+20.f*scale,viewport->WorkPos.y+26.f*scale};
    const ImVec2 safe_max{viewport->WorkPos.x+viewport->WorkSize.x-20.f*scale,
                          viewport->WorkPos.y+viewport->WorkSize.y-38.f*scale};
    const float available = safe_max.x-safe_min.x-2.f*padding.x;
    const float width_limit = std::max(1.f, std::min(350.f*scale,available));
    const auto measure = [](const char* line) { return line ? ImGui::CalcTextSize(line).x : 0.f; };
    const float width = std::min(width_limit,std::max({measure(text),measure(title),measure(subtitle)}));
    const float window_width = width+2.f*padding.x;
    const float line_gap = 4.f*scale;
    float height = 2.f*padding.y;
    int blocks = 0;
    for (const char* line : {title,subtitle,text}) {
        if (!line || !*line)
            continue;
        if (blocks++) height += line_gap;
        height += ImGui::CalcTextSize(line,nullptr,false,width_limit).y;
    }
    // ImGui's own viewport padding does not account for the engraved cabinet.
    const auto anchor = ImGui::IsMousePosValid() ? ImGui::GetMousePos()
                                                  : ImGui::GetItemRectMax();
    const float x = anchor.x+15.f*scale+window_width <= safe_max.x
        ? anchor.x+15.f*scale : anchor.x-12.f*scale-window_width;
    const float y = anchor.y+14.f*scale+height <= safe_max.y
        ? anchor.y+14.f*scale : anchor.y-10.f*scale-height;
    ImGui::SetNextWindowPos({std::clamp(x,safe_min.x,std::max(safe_min.x,safe_max.x-window_width)),
                             std::clamp(y,safe_min.y,std::max(safe_min.y,safe_max.y-height))});
    ImGui::SetNextWindowSize({window_width,0});
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,4.f*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize,scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{8.f*scale,line_gap});
    ImGui::PushStyleColor(ImGuiCol_PopupBg,{.068f,.063f,.055f,1});
    ImGui::PushStyleColor(ImGuiCol_Border,{.43f,.36f,.27f,1});
    if (ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(padding.x+width_limit);
        if (title && *title)
            ImGui::TextColored({.89f,.78f,.59f,1},"%s",title);
        if (subtitle && *subtitle)
            ImGui::TextColored({.72f,.68f,.61f,1},"%s",subtitle);
        if (text && *text)
            ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(5);
    ImGui::PopFont();
}

struct Art {
    Launcher::EmbeddedImage hero, leather, frame, button, launch, cabinet, hardware, scrollbar;
    bool attempted{};
    void Load(ID3D11Device* device) {
        if (attempted) return;
        attempted = true;
        frame = Launcher::LoadEmbeddedImage(device, IDR_THEME_FRAME);
        cabinet = Launcher::LoadEmbeddedImage(device, IDR_THEME_CABINET_FRAME);
        hardware = Launcher::LoadEmbeddedImage(device, IDR_THEME_HARDWARE);
        button = Launcher::LoadEmbeddedImage(device, IDR_THEME_BUTTON_FRAME);
        launch = Launcher::LoadEmbeddedImage(device, IDR_THEME_LAUNCH_PLAQUE);
        hero = Launcher::LoadEmbeddedImage(device, IDR_THEME_YHARNAM);
        leather = Launcher::LoadEmbeddedImage(device, IDR_THEME_LEATHER);
        scrollbar = Launcher::LoadEmbeddedImage(device, IDR_THEME_SCROLLBAR);
    }
};

inline void Leather(ImDrawList* d, ImVec2 a, ImVec2 b,
                    const Art& art, float scale, ImU32 tint = IM_COL32(180,185,190,255),
                    ImU32 wash = 0) {
    d->AddRectFilled(a, b, IM_COL32(12,14,16,255));
    if (art.leather.view) {
        const float tile = 256 * scale;
        for (float y = a.y; y < b.y; y += tile)
            for (float x = a.x; x < b.x; x += tile) {
                ImVec2 end{std::min(x + tile, b.x), std::min(y + tile, b.y)};
                d->AddImage(reinterpret_cast<ImTextureID>(art.leather.view.Get()), {x,y}, end,
                            {0,0}, {(end.x-x)/tile,(end.y-y)/tile}, tint);
            }
    }
    if (wash)
        d->AddRectFilled(a,b,wash);
}

inline void Frame(ImDrawList* d, ImVec2 a, ImVec2 b, float s, bool heavy = false) {
    const float w = (heavy ? 5.f : 2.f) * s;
    d->AddRect(a, b, IM_COL32(4,5,6,255), 3*s, 0, w + 3*s);
    d->AddRect(a, b, IM_COL32(62,63,61,255), 3*s, 0, w);
    d->AddRect({a.x+s,a.y+s}, {b.x-s,b.y-s}, IM_COL32(109,103,88,255), 2*s);
    d->AddRect({a.x+3*s,a.y+3*s}, {b.x-3*s,b.y-3*s}, IM_COL32(8,10,12,255), 2*s);
    for (int y : {0,1}) for (int x : {0,1}) {
        const float sx = x ? -1.f : 1.f, sy = y ? -1.f : 1.f;
        ImVec2 p{x ? b.x : a.x, y ? b.y : a.y};
        d->AddLine({p.x+sx*5*s,p.y+sy*18*s}, {p.x+sx*5*s,p.y+sy*5*s}, IM_COL32(125,116,96,255));
        d->AddLine({p.x+sx*5*s,p.y+sy*5*s}, {p.x+sx*18*s,p.y+sy*5*s}, IM_COL32(125,116,96,255));
        if (heavy) {
            ImVec2 c{p.x+sx*10*s,p.y+sy*10*s};
            d->AddCircleFilled(c, 3*s, IM_COL32(7,9,10,255), 8);
            d->AddCircle(c, 2*s, IM_COL32(111,108,96,255), 8);
        }
    }
}

inline void Divider(ImDrawList* d, ImVec2 a, float width, float scale) {
    const float mid = a.x + width*.5f, r = 3*scale;
    const auto ink = IM_COL32(107,100,85,160);
    d->AddLine(a, {mid-9*scale,a.y}, ink);
    d->AddLine({mid+9*scale,a.y}, {a.x+width,a.y}, ink);
    const ImVec2 diamond[]{{mid,a.y-r},{mid+r,a.y},{mid,a.y+r},{mid-r,a.y}};
    d->AddPolyline(diamond, 4, ink, ImDrawFlags_Closed, scale);
}

inline void WeatheredTitle(const char* title, float width, float scale) {
    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddText({origin.x+scale,origin.y+3.f*scale},IM_COL32(0,0,0,220),title);
    const int first_vertex = draw->VtxBuffer.Size;
    ImGui::TextUnformatted(title);
    if (!ImGui::IsItemVisible())
        return;
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw,first_vertex,draw->VtxBuffer.Size,
        {origin.x,origin.y+13.f*scale},{origin.x,origin.y+50.f*scale},
        IM_COL32(253,235,202,255),IM_COL32(178,147,106,255));

    // A fixed grain pattern stays still between frames. Fine clipping keeps it inside the ink.
    std::uint32_t grain = 0x5fa26u;
    const auto sample = [&] {
        grain = grain*1664525u+1013904223u;
        return float((grain >> 12)&1023u)/1024.f;
    };
    auto* const font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    for (int i=0; i<153; ++i) {
        const float x = origin.x+sample()*width;
        const float y = origin.y+(12.f+sample()*40.f)*scale;
        const float length = (1.4f+sample()*8.f)*scale;
        const float depth = (.85f+sample()*.6f)*scale;
        const ImU32 ink = i%6==0 ? IM_COL32(254,242,215,157) : IM_COL32(83,62,43,124);
        const ImVec4 clip{x,y,x+length,y+depth};
        draw->AddText(font,font_size,origin,ink,title,nullptr,0.f,&clip);
    }
}

// Nine-slice keeps the metal rails narrow and the corner fittings undistorted at any DPI.
inline void MetalFrame(ImDrawList* d, ImVec2 a, ImVec2 b, const Launcher::EmbeddedImage& art,
                       float scale, ImVec2 corners, ImVec2 slice, float top_rail_height = 0.f) {
    if (!art.view) { Frame(d,a,b,scale,true); return; }
    const float cx = std::min(corners.x*scale,(b.x-a.x)*.5f);
    const float cy = std::min(corners.y*scale,(b.y-a.y)*.5f);
    const float x[]{a.x,a.x+cx,b.x-cx,b.x};
    const float y[]{a.y,a.y+cy,b.y-cy,b.y};
    const float u[]{0,slice.x,1-slice.x,1}, v[]{0,slice.y,1-slice.y,1};
    for (int row=0;row<3;++row) for (int col=0;col<3;++col) {
        if (row==1 && col==1) continue;
        const bool crop_top_rail = row==0 && col==1 && top_rail_height>0.f;
        if (crop_top_rail)
            d->PushClipRect({x[col],y[row]},{x[col+1],a.y+top_rail_height},true);
        d->AddImage(reinterpret_cast<ImTextureID>(art.view.Get()),
            {x[col],y[row]},{x[col+1],y[row+1]},
            {u[col],v[row]},{u[col+1],v[row+1]});
        if (crop_top_rail)
            d->PopClipRect();
    }
}

inline void Mullion(ImDrawList* draw, ImVec2 pos, float height, const Art& art, float scale) {
    const ImVec2 left{pos.x-5*scale,pos.y}, right{pos.x+5*scale,pos.y+height};
    draw->AddRectFilled(left,right,IM_COL32(8,9,10,255));
    if (art.cabinet.view) {
        // Sample a straight section of the outer rail to use the same iron on the join.
        draw->AddImage(reinterpret_cast<ImTextureID>(art.cabinet.view.Get()),left,right,
                       {.011f,.18f},{.037f,.82f},IM_COL32(205,205,205,255));
    }
}

inline void PaintScrollbarThumb(ImDrawList* draw, const ImRect& thumb, ImGuiAxis axis,
                               const Art& art, bool hovered, bool held, float scale) {
    if (!art.scrollbar.view) {
        const auto fill = held ? IM_COL32(168,129,85,255) : hovered ? IM_COL32(138,111,78,255)
                                                                      : IM_COL32(104,87,67,255);
        draw->AddRectFilled(thumb.Min, thumb.Max, fill, 2.f * scale);
        return;
    }

    const bool vertical = axis == ImGuiAxis_Y;
    const ImGuiAxis cross_axis = vertical ? ImGuiAxis_X : ImGuiAxis_Y;
    const float width = thumb.GetSize()[cross_axis], length = thumb.GetSize()[axis];
    const float start = thumb.Min[axis], end = thumb.Max[axis], center = (start + end) * .5f;
    const float cap = std::min(width * .72f, length * .22f);
    const float grip = std::min(width * 1.55f, length * .48f);
    // Stretch the plain metal between the end caps and the eye. The ornament
    // stays the same size as the thumb changes length with the page.
    const float pos[]{start, start+cap, center-grip*.5f, center+grip*.5f, end-cap, end};
    const float uv[]{0.f, .065f, .425f, .565f, .935f, 1.f};
    const auto tint = held ? IM_COL32(206,235,255,255) : hovered ? IM_COL32_WHITE
                                                                 : IM_COL32(222,218,211,255);
    const auto texture = reinterpret_cast<ImTextureID>(art.scrollbar.view.Get());
    for (int i = 0; i < 5; ++i) {
        if (vertical) {
            draw->AddImage(texture, {thumb.Min.x,pos[i]}, {thumb.Max.x,pos[i+1]},
                           {0,uv[i]}, {1,uv[i+1]}, tint);
        } else {
            draw->AddImageQuad(texture, {pos[i],thumb.Min.y}, {pos[i+1],thumb.Min.y},
                {pos[i+1],thumb.Max.y}, {pos[i],thumb.Max.y},
                {1,uv[i]}, {1,uv[i+1]}, {0,uv[i+1]}, {0,uv[i]}, tint);
        }
    }
}

// ImGui still lays out and handles the scrollbar. Paint only its thumb, at the
// native coordinates, after the window contents have been drawn.
inline void PaintScrollbar(ImGuiWindow* window, ImGuiAxis axis, const Art& art, float scale) {
    const ImRect frame = ImGui::GetWindowScrollbarRect(window, axis);
    if (frame.GetWidth() <= 0 || frame.GetHeight() <= 0 || !frame.Overlaps(window->OuterRectClipped))
        return;
    const auto inset = [](float size) { return std::clamp(std::trunc((size - 2.f) * .5f), 0.f, 3.f); };
    ImRect thumb = frame;
    thumb.Expand({-inset(frame.GetWidth()), -inset(frame.GetHeight())});
    // Leave the extra layout space on the content side, keeping the art the same width.
    const ImGuiAxis cross_axis = axis == ImGuiAxis_Y ? ImGuiAxis_X : ImGuiAxis_Y;
    thumb.Min[cross_axis] += std::min(std::round(ScrollbarGutter*scale),
                                      std::max(0.f,thumb.GetSize()[cross_axis]-1.f));
    const float length = thumb.GetSize()[axis];
    if (length <= 0)
        return;
    const float visible = std::trunc(window->InnerRect.GetSize()[axis]);
    const float contents = std::trunc(window->ContentSize[axis] + 2.f * window->WindowPadding[axis]);
    const float size = std::clamp(length * visible / std::max({1.f, visible, contents}),
                                  std::min(length, ImGui::GetStyle().GrabMinSize), length);
    const float ratio = std::clamp(std::trunc(window->Scroll[axis]) /
                                   std::max(1.f, contents - visible), 0.f, 1.f);
    thumb.Min[axis] += (length - size) * ratio;
    thumb.Max[axis] = thumb.Min[axis] + size;

    const auto& gui = *ImGui::GetCurrentContext();
    const ImGuiID id = ImGui::GetWindowScrollbarID(window, axis);
    const bool held = gui.ActiveId == id;
    const bool hovered = gui.HoveredId == id;
    auto* draw = window->DrawList;
    draw->PushClipRect(window->OuterRectClipped.Min, window->OuterRectClipped.Max, false);
    PaintScrollbarThumb(draw, thumb, axis, art, hovered, held, scale);
    draw->PopClipRect();
}

inline void PaintScrollbars(const Art& art, float scale) {
    for (auto* window : ImGui::GetCurrentContext()->Windows) {
        if (!window->Active || window->Hidden || window->Collapsed)
            continue;
        if (window->ScrollbarX) PaintScrollbar(window, ImGuiAxis_X, art, scale);
        if (window->ScrollbarY) PaintScrollbar(window, ImGuiAxis_Y, art, scale);
    }
}

inline void HardwarePiece(ImDrawList* draw, const Art& art, ImVec2 a, ImVec2 b,
                          ImVec2 first, ImVec2 last, ImU32 tint = IM_COL32_WHITE) {
    draw->AddImage(reinterpret_cast<ImTextureID>(art.hardware.view.Get()),a,b,
                   {first.x/512.f,first.y/192.f},{last.x/512.f,last.y/192.f},tint);
}

inline void PlayScrollEdge(ImGuiWindow* window, const Art& art, float scale) {
    const auto& outer = window->OuterRectClipped;
    const float left = outer.Min.x, right = outer.Max.x;
    const float edge = window->InnerRect.Max.y;
    auto* draw = window->DrawList;
    // Paint on the scroll window, after its rows, and let the trim project into the footer.
    draw->PushClipRect({left,outer.Min.y},
                      {right,std::min(edge+19.f*scale,window->ParentWindow->InnerClipRect.Max.y)}, false);
    constexpr ImU32 shadow = IM_COL32(0,0,0,116);
    const float shadow_depth = 8.f*scale;
    draw->AddRectFilledMultiColor({left,edge-shadow_depth}, {right,edge}, 0,0,shadow,shadow);
    draw->AddRectFilledMultiColor({left,edge}, {right,edge+shadow_depth}, shadow,shadow,0,0);

    if (art.frame.view) {
        // Use the lower frame rail and its scrollwork corners. Only the straight iron stretches.
        const float cap = std::min(30.f*scale, (right-left)*.2f);
        const float x[]{left,left+cap,right-cap,right};
        const float u[]{4.f/512.f,66.f/512.f,446.f/512.f,508.f/512.f};
        const auto rail = reinterpret_cast<ImTextureID>(art.frame.view.Get());
        for (int i = 0; i < 3; ++i)
            draw->AddImage(rail, {x[i],edge-9.f*scale}, {x[i+1],edge+8.f*scale},
                           {u[i],449.f/512.f}, {u[i+1],507.f/512.f}, IM_COL32(178,182,189,255));
    } else {
        Divider(draw, {left,edge+2.f*scale}, right-left, scale);
    }
    if (art.hardware.view) {
        const float center = (left+right)*.5f;
        HardwarePiece(draw, art, {center-40.f*scale,edge-6.f*scale},
                      {center+40.f*scale,edge+17.f*scale}, {20,100}, {212,154},
                      IM_COL32(202,205,213,255));
    }
    draw->PopClipRect();
}

inline void CabinetFrame(ImDrawList* draw, ImVec2 size, const Art& art, float scale,
                         bool hero_at_top) {
    if (art.cabinet.view) {
        // Keep the top rail's own shading over the hero instead of a flat dark strip.
        if (hero_at_top)
            draw->PushClipRect({0,22*scale},size,true);
        draw->AddRect({16*scale,16*scale},{size.x-16*scale,size.y-16*scale},
                      IM_COL32(0,0,0,120),29*scale,0,11*scale);
        if (hero_at_top)
            draw->PopClipRect();
    }
    // The straight rail texture includes a dark inner lip that covers the top of the hero.
    MetalFrame(draw,{2*scale,2*scale},{size.x-2*scale,size.y-2*scale},
               art.cabinet,scale,{60,59},{.13f,.17f},hero_at_top ? 15.f*scale : 0.f);
    if (!art.hardware.view)
        return;
    const float center = size.x*.5f;
    HardwarePiece(draw,art,{center-32*scale,4*scale},{center+32*scale,22*scale},
                  {20,100},{212,154});
    const float height = (size.y < 700*scale ? 24.f : 30.f)*scale;
    const float half_width = height*92.f/168.f;
    const float bottom = size.y-4*scale;
    // Seat the lock plate over scrollwork shoulders along the lower rail.
    HardwarePiece(draw,art,{center-height*1.1f,bottom-height*.65f},
                  {center+height*1.1f,bottom-height*.08f}, {20,100},{212,154},
                  IM_COL32(180,180,178,255));
    HardwarePiece(draw,art,{center-half_width,bottom-height},{center+half_width,bottom},
                  {270,100},{362,184});
}

inline void ScanCabinet(ImGuiWindow* window, const Art& art, float scale) {
    const auto a = window->Pos;
    const ImVec2 b{a.x+window->Size.x,a.y+window->Size.y};
    auto* draw = window->DrawList;
    draw->PushClipRect(window->OuterRectClipped.Min, window->OuterRectClipped.Max, false);
    draw->AddRectFilled({a.x+4.f*scale,a.y+4.f*scale}, {b.x-4.f*scale,b.y-4.f*scale},
                        IM_COL32(10,11,12,255), 18.f*scale);
    Leather(draw,{a.x+14.f*scale,a.y+12.f*scale},{b.x-14.f*scale,b.y-12.f*scale},
            art,scale,IM_COL32(141,145,151,255),IM_COL32(16,20,24,30));
    if (art.cabinet.view)
        MetalFrame(draw,a,b,art.cabinet,scale,{43,43},{.13f,.17f});
    else
        MetalFrame(draw,{a.x+2.f*scale,a.y+2.f*scale},{b.x-2.f*scale,b.y-2.f*scale},
                   art.frame,scale,{24,24},{.14f,.14f});
    if (art.hardware.view) {
        const float center = (a.x+b.x)*.5f;
        HardwarePiece(draw,art,{center-31.f*scale,a.y+2.f*scale},{center+31.f*scale,a.y+20.f*scale},
                      {20,100},{212,154});
    }
    draw->PopClipRect();
}

inline void InsetPanel(ImDrawList* draw, ImVec2 a, ImVec2 b, const Art& art, float scale) {
    Leather(draw,a,b,art,scale,IM_COL32(140,142,147,255));
    const ImVec2 inside{a.x+3*scale,a.y+3*scale};
    const ImVec2 end{b.x-3*scale,b.y-3*scale};
    draw->AddRectFilledMultiColor(inside,end,IM_COL32(0,0,0,105),IM_COL32(0,0,0,105),
                                 IM_COL32(0,0,0,20),IM_COL32(0,0,0,20));
    MetalFrame(draw,a,b,art.frame,scale,{17,17},{.14f,.14f});
}

inline void PlaqueFrame(ImDrawList* d, ImVec2 a, ImVec2 b, const Art& art, float scale) {
    if (!art.button.view) { Frame(d,a,b,scale); return; }
    const float c = std::min({28.f*scale,(b.y-a.y)*.38f,(b.x-a.x)*.25f});
    const float x[]{a.x,a.x+c,b.x-c,b.x}, y[]{a.y,a.y+c,b.y-c,b.y};
    const float u[]{0,.055f,.945f,1}, v[]{0,.26f,.74f,1};
    const auto tint = ImGui::GetColorU32(ImVec4{1,1,1,1});
    for (int row=0;row<3;++row) for (int col=0;col<3;++col) {
        if (row==1 && col==1) continue;
        d->AddImage(reinterpret_cast<ImTextureID>(art.button.view.Get()),
            {x[col],y[row]},{x[col+1],y[row+1]}, {u[col],v[row]},{u[col+1],v[row+1]},tint);
    }
}

// Short, fixed English brand labels use optical tracking; controls keep their native IDs.
inline float TrackedWidth(const char* text, float spacing) {
    const size_t n = strlen(text);
    return ImGui::CalcTextSize(text).x + (n ? float(n-1)*spacing : 0);
}
inline void TrackedText(ImDrawList* d, ImVec2 p, const char* text, float spacing, ImU32 ink) {
    for (const char* c=text; *c; ++c) {
        const char glyph[]{*c,0};
        d->AddText(p,ink,glyph);
        p.x += ImGui::CalcTextSize(glyph).x+spacing;
    }
}

inline bool Link(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_TextLink, {.76f,.72f,.63f,1});
    const bool clicked = ImGui::TextLink(label);
    ImGui::PopStyleColor();
    return clicked;
}

enum class ButtonFace { Standard, Launch, Utility, Gilt };

inline void JoinGlowRings(ImDrawList* draw, unsigned base, int ring_size, int rings) {
    for (int ring = 0; ring < rings-1; ++ring) {
        for (int i = 0; i < ring_size; ++i) {
            const auto inner = base+ring*ring_size+i;
            const auto next = base+ring*ring_size+(i+1)%ring_size;
            for (auto index : {inner,next,next+ring_size,inner,next+ring_size,inner+ring_size})
                draw->PrimWriteIdx(static_cast<ImDrawIdx>(index));
        }
    }
}

inline void LaunchFaceGlow(ImDrawList* draw, ImVec2 a, ImVec2 b) {
    const ImVec2 center{(a.x+b.x)*.5f,(a.y+b.y)*.5f};
    const ImVec2 radius{(b.x-a.x)*.46f,(b.y-a.y)*.33f};
    constexpr int ring_size = 32;
    constexpr float radii[]{.23f,.46f,.72f,1.f};
    constexpr int opacity[]{61,39,16,0};
    constexpr int rings = IM_ARRAYSIZE(radii);
    const auto uv = ImGui::GetFontTexUvWhitePixel();

    draw->PrimReserve((3+6*(rings-1))*ring_size,1+rings*ring_size);
    const auto base = draw->_VtxCurrentIdx;
    draw->PrimWriteVtx(center,uv,IM_COL32(217,70,42,70));
    for (int ring = 0; ring < rings; ++ring) {
        for (int i = 0; i < ring_size; ++i) {
            const float angle = i*(6.28318531f/ring_size);
            draw->PrimWriteVtx({center.x+std::cos(angle)*radius.x*radii[ring],
                                center.y+std::sin(angle)*radius.y*radii[ring]},
                               uv,IM_COL32(217,70,42,opacity[ring]));
        }
    }
    for (int i = 0; i < ring_size; ++i)
        for (auto index : {base,base+1+i,base+1+(i+1)%ring_size})
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(index));
    JoinGlowRings(draw,base+1,ring_size,rings);
}

inline void PaintPlaque(ImDrawList* draw, ImVec2 a, ImVec2 b, const Art& art,
                        float scale, ImU32 fill, bool held, bool gilt = false) {
    const float notch = std::min({28.f*scale,(b.y-a.y)*.38f,(b.x-a.x)*.25f})*.65f;
    const ImVec2 face[]{{a.x+notch,a.y},{b.x-notch,a.y},{b.x,a.y+notch},
        {b.x,b.y-notch},{b.x-notch,b.y},{a.x+notch,b.y},{a.x,b.y-notch},{a.x,a.y+notch}};
    draw->AddRectFilled({a.x+scale,a.y+2*scale},{b.x-scale,b.y+scale},
                        ImGui::GetColorU32(ImVec4{0,0,0,.55f}),3*scale);
    const int first_vertex = draw->VtxBuffer.Size;
    draw->AddConvexPolyFilled(face,IM_ARRAYSIZE(face),fill);
    if (gilt) {
        const auto base = ImGui::ColorConvertU32ToFloat4(fill);
        const auto top = ImGui::ColorConvertFloat4ToU32({base.x+.09f,base.y+.09f,base.z+.07f,1});
        const auto bottom = ImGui::ColorConvertFloat4ToU32({base.x*.88f,base.y*.85f,base.z*.81f,1});
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw,first_vertex,draw->VtxBuffer.Size,
                                                      a,{a.x,b.y},top,bottom);
        // Wash the entire face with patina so the texture follows the bevel.
        if (art.leather.view) {
            const int patina_start = draw->VtxBuffer.Size;
            draw->PushTextureID(reinterpret_cast<ImTextureID>(art.leather.view.Get()));
            draw->AddConvexPolyFilled(face,IM_ARRAYSIZE(face),ImGui::GetColorU32(ImVec4{1,1,1,.12f}));
            ImGui::ShadeVertsLinearUV(draw,patina_start,draw->VtxBuffer.Size,a,b,
                                      {0,0},{1.f,std::min(1.f,(b.y-a.y)/(96.f*scale))},true);
            draw->PopTextureID();
        }
        PlaqueFrame(draw,a,b,art,scale);
        return;
    }
    const ImU32 sheen = ImGui::GetColorU32(ImVec4{.78f,.70f,.58f,held ? .025f : .11f});
    draw->AddRectFilledMultiColor({a.x+notch,a.y},{b.x-notch,b.y},sheen,sheen,0,0);
    const ImVec2 inner{a.x+3*scale,a.y+notch}, end{b.x-3*scale,b.y-notch};
    if (art.leather.view && end.x>inner.x && end.y>inner.y) {
        draw->AddImage(reinterpret_cast<ImTextureID>(art.leather.view.Get()),inner,end,
            {0,0},{(end.x-inner.x)/(256*scale),(end.y-inner.y)/(256*scale)},
            ImGui::GetColorU32(ImVec4{1,1,1,.35f}));
    }
    PlaqueFrame(draw,a,b,art,scale);
}

inline void UtilityFace(ImDrawList* draw, ImVec2 a, ImVec2 b, const Art& art, float scale,
                        bool hovered, bool held, bool selected = false) {
    if (!art.hardware.view) {
        const ImGuiCol state = held ? ImGuiCol_ButtonActive
                             : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button;
        const ImU32 fill = selected ? ImGui::GetColorU32(Crimson) : ImGui::GetColorU32(state);
        PaintPlaque(draw,a,b,art,scale,fill,held);
        return;
    }
    const float brightness = held ? .67f : hovered ? 1.f : .86f;
    const auto tint = ImGui::GetColorU32(ImVec4{brightness,brightness,brightness,1});
    draw->AddRectFilled({a.x+2*scale,a.y+3*scale},{b.x-2*scale,b.y+2*scale},
                       ImGui::GetColorU32(ImVec4{0,0,0,.49f}),3*scale);
    // Stretch the middle and keep the rivets away from the label on compact tabs.
    const float cap = std::min({12.f*scale,(b.y-a.y)*.34f,(b.x-a.x)*.25f});
    const float x[]{a.x,a.x+cap,b.x-cap,b.x};
    const float source[]{16,44,468,496};
    for (int i=0;i<3;++i)
        HardwarePiece(draw,art,{x[i],a.y},{x[i+1],b.y},{source[i],8},{source[i+1],78},tint);
    if (selected) {
        const float enamel = held ? .38f : hovered ? .65f : .55f;
        const auto red = ImGui::GetColorU32(ImVec4{.39f,.065f,.075f,enamel});
        // Trace the inside edge of the bevel in the same 28 x 70 atlas coordinates
        // as the end caps. The enamel reaches both ends as the center stretches.
        const float pixel_x = cap / 28.f;
        const float pixel_y = (b.y-a.y) / 70.f;
        const float edge_x = 9.f * pixel_x;
        const float shoulder_x = 19.f * pixel_x;
        const float top = a.y + 9.f * pixel_y;
        const float upper_shoulder = a.y + 18.f * pixel_y;
        const float lower_shoulder = a.y + 50.f * pixel_y;
        const float bottom = a.y + 59.f * pixel_y;
        const ImVec2 face[]{{a.x+shoulder_x,top}, {b.x-shoulder_x,top},
                            {b.x-edge_x,upper_shoulder}, {b.x-edge_x,lower_shoulder},
                            {b.x-shoulder_x,bottom}, {a.x+shoulder_x,bottom},
                            {a.x+edge_x,lower_shoulder}, {a.x+edge_x,upper_shoulder}};
        draw->AddConvexPolyFilled(face, IM_ARRAYSIZE(face), red);
    }
}

// Draw behind the native widget so text, focus, navigation and input remain native.
template<class Widget, class Paint>
inline bool Decorated(ImGuiID id, Widget widget, Paint paint) {
    auto* draw = ImGui::GetWindowDrawList();
    ImDrawListSplitter layers;
    layers.Split(draw,2);
    layers.SetCurrentChannel(draw,1);
    const bool result = widget();
    if (ImGui::GetItemID() == id && ImGui::IsItemVisible()) {
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        layers.SetCurrentChannel(draw,0);
        paint(draw,ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),hovered,ImGui::IsItemActive(),result);
    }
    layers.Merge(draw);
    return result;
}

// The font's line box sits slightly low against the plaque's bevel. Lift the ink
// by one logical pixel while keeping the widget and its focus outline in place.
inline float PlaqueLabelLift(float scale) {
    return std::round(scale);
}

// Keep ImGui's real button behavior, navigation, disabled handling and focus outline.
inline bool Button(const char* label, ImVec2 size, const Art& art, float scale,
                   ButtonFace face = ButtonFace::Standard, bool selected = false) {
    const ImU32 normal = ImGui::GetColorU32(ImGuiCol_Button);
    const ImU32 hover = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
    const ImU32 active = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    const ImGuiID id = ImGui::GetID(label);
    const bool lift_label = face == ButtonFace::Utility && art.hardware.view;
    return Decorated(id, [&] {
        ImGui::PushStyleColor(ImGuiCol_Button, {0,0,0,0});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0,0,0,0});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0,0,0,0});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0.f);
        if (lift_label) ImGui::PushStyleColor(ImGuiCol_Text,{0,0,0,0});
        const bool clicked = ImGui::Button(label,size);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(lift_label ? 4 : 3);
        if (lift_label && ImGui::GetItemID() == id && ImGui::IsItemVisible()) {
            const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            const auto& style = ImGui::GetStyle();
            const float lift = PlaqueLabelLift(scale);
            // The fixed-height sidebar plaques can be shorter than the font plus the
            // default padding. Leave enough room to center the text on those too.
            const float pad_y = std::clamp((rect.GetHeight()-ImGui::GetFontSize())*.5f,
                                           0.f, style.FramePadding.y);
            ImGui::RenderTextClippedEx(ImGui::GetWindowDrawList(),
                {rect.Min.x+style.FramePadding.x,rect.Min.y+pad_y-lift},
                {rect.Max.x-style.FramePadding.x,rect.Max.y-pad_y-lift},
                label,ImGui::FindRenderedTextEnd(label),nullptr,style.ButtonTextAlign,&rect);
        }
        return clicked;
    }, [&](ImDrawList* draw, ImVec2 a, ImVec2 b, bool hovered, bool held, bool) {
        if (face == ButtonFace::Utility) {
            UtilityFace(draw,a,b,art,scale,hovered,held,selected);
        } else if (face == ButtonFace::Launch && art.launch.view) {
            const float cap = std::min((b.y-a.y)*.55f,(b.x-a.x)*.2f);
            const float x[]{a.x,a.x+cap,b.x-cap,b.x};
            const float u[]{0,.14f,.86f,1};
            const float brightness = held ? .67f : hovered ? 1.f : .78f;
            const auto tint = ImGui::GetColorU32(ImVec4{brightness,brightness,brightness,1});
            const int first_vertex = draw->VtxBuffer.Size;
            for (int i=0;i<3;++i)
                draw->AddImage(reinterpret_cast<ImTextureID>(art.launch.view.Get()),
                    {x[i],a.y},{x[i+1],b.y},{u[i],0},{u[i+1],1},tint);
            // Shade the artwork itself so the leather, trim, and transparent edges stay intact.
            if (hovered && !held) {
                ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw,first_vertex,draw->VtxBuffer.Size,
                    a,{a.x,b.y},IM_COL32_WHITE,IM_COL32(241,230,220,255));
                LaunchFaceGlow(draw,a,b);
            }
        } else {
            PaintPlaque(draw,a,b,art,scale,held ? active : hovered ? hover : normal,held,
                        face == ButtonFace::Gilt);
        }
    });
}

inline bool Header(const char* label, const Art& art, float scale) {
    return Decorated(ImGui::GetID(label), [&] {
        ImGui::PushStyleColor(ImGuiCol_Header,{0,0,0,0});
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,{0,0,0,0});
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,{0,0,0,0});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0.f);
        const bool open = ImGui::CollapsingHeader(label);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        return open;
    }, [&](ImDrawList* draw, ImVec2 a, ImVec2 b, bool hovered, bool held, bool open) {
        const ImVec4 fill = held ? ImVec4{.12f,.09f,.075f,1}
                        : hovered ? ImVec4{.19f,.16f,.13f,1}
                        : open ? ImVec4{.13f,.105f,.085f,1} : ImVec4{.085f,.082f,.075f,1};
        PaintPlaque(draw,a,b,art,scale,ImGui::GetColorU32(fill),held);
    });
}

inline bool Tab(const char* label, ImGuiTabItemFlags flags, const Art& art, float scale) {
    const ImGuiID id = ImGui::GetID(label);
    const bool lift_label = bool(art.hardware.view);
    return Decorated(id, [&] {
        for (const ImGuiCol color : {ImGuiCol_Tab, ImGuiCol_TabHovered, ImGuiCol_TabSelected,
                                     ImGuiCol_TabDimmed, ImGuiCol_TabDimmedSelected})
            ImGui::PushStyleColor(color,{0,0,0,0});
        if (lift_label) ImGui::PushStyleColor(ImGuiCol_Text,{0,0,0,0});
        const bool selected = ImGui::BeginTabItem(label,nullptr,flags);
        ImGui::PopStyleColor(lift_label ? 6 : 5);
        if (lift_label && ImGui::GetItemID() == id && ImGui::IsItemVisible()) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const auto& tab_bar = *ImGui::GetCurrentContext()->CurrentTabBar;
            auto* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect({tab_bar.ScrollingRectMinX,a.y-1},{tab_bar.ScrollingRectMaxX,b.y},true);
            const ImVec2 start{a.x+tab_bar.FramePadding.x,a.y+tab_bar.FramePadding.y-PlaqueLabelLift(scale)};
            const ImVec2 end{b.x-tab_bar.FramePadding.x,b.y};
            ImGui::RenderTextEllipsis(draw,start,end,end.x,b.x-1,label,nullptr,nullptr);
            draw->PopClipRect();
        }
        return selected;
    }, [&](ImDrawList* draw, ImVec2 a, ImVec2 b, bool hovered, bool held, bool selected) {
        UtilityFace(draw,a,b,art,scale,hovered,held,selected);
    });
}

inline bool Checkbox(const char* label, bool* checked, const Art& art, float scale) {
    const float side = ImGui::GetFrameHeight();
    return Decorated(ImGui::GetID(label), [&] {
        ImGui::PushStyleColor(ImGuiCol_FrameBg,{0,0,0,0});
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,{0,0,0,0});
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,{0,0,0,0});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0.f);
        const bool changed = ImGui::Checkbox(label,checked);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        return changed;
    }, [&](ImDrawList* draw, ImVec2 a, ImVec2, bool hovered, bool held, bool) {
        const ImVec4 fill = hovered ? ImVec4{.20f,.16f,.13f,1}
                          : *checked ? ImVec4{.17f,.07f,.06f,1} : ImVec4{.075f,.073f,.068f,1};
        PaintPlaque(draw,a,{a.x+side,a.y+side},art,scale,ImGui::GetColorU32(fill),held);
    });
}

inline bool LetteredButton(const char* label, ImVec2 size, const Art& art, float scale, float spacing) {
    ImGui::PushStyleColor(ImGuiCol_Text,{0,0,0,0});
    const bool clicked = Button(label,size,art,scale,ButtonFace::Launch);
    ImGui::PopStyleColor();
    if (ImGui::GetItemID()==ImGui::GetID(label) && ImGui::IsItemVisible()) {
        const auto ink = ImGui::IsItemHovered() && !ImGui::IsItemActive()
            ? ImGui::GetColorU32(ImVec4{.99f,.91f,.76f,1.f}) : ImGui::GetColorU32(ImGuiCol_Text);
        const auto a=ImGui::GetItemRectMin(), b=ImGui::GetItemRectMax();
        TrackedText(ImGui::GetWindowDrawList(),
            {a.x+(b.x-a.x-TrackedWidth(label,spacing))*.5f,
             a.y+(b.y-a.y-ImGui::GetFontSize())*.5f}, label,spacing,ink);
    }
    return clicked;
}
} // namespace HunterTheme
