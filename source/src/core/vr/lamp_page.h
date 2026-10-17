// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/lamp_travel.h"
#include "core/vr/vr_page_render.h"

namespace VR::Lamps {
template <class LampModel>
inline Page::View MakePage(const LampModel& m, const Page::Glyphs& glyphs = {},
                           Page::Hover hover = {}) {
    using namespace Page;
    View v;
    v.glyphs = glyphs;
    v.hover = hover;
    v.single_column = true; // centred rows: no column divider through them
    v.tabs = {m.travel ? "Travel" : "Lamp"};
    if (const auto* current = ByReturnPoint(m.offer.current))
        v.headers[0] = current->name;
    if (m.travel) {
        v.tabs = {m.offer.pages.empty() ? "Travel" : m.offer.pages[m.page].Title()};
        for (int i = 0; i < m.Count(); ++i) {
            const auto* dest = ByReturnPoint(m.offer.pages[m.page].destinations[i]);
            v.rows[i] = {true,        dest ? dest->name : "Unavailable", "", Widget::Action, 0, 0.f,
                         m.focus == i};
        }
        v.help = m.Count() ? "Travel and refill. Grips change destination pages."
                           : "No available destinations.";
    } else {
        constexpr std::array<const char*, 4> labels{"Rest", "Travel", "Hunter's Dream", "Leave"};
        constexpr std::array<const char*, 4> help{
            "Refill vials and bullets, restore health and respawn enemies.",
            "Travel to a lit lamp. Blocked destinations are hidden.",
            "Return to the Hunter's Dream.", "Leave the lamp."};
        for (int i = 0; i < 4; ++i)
            v.rows[i] = {true, labels[i], "", Widget::Action, 0, 0.f, m.focus == i};
        v.help = help[m.focus];
    }
    v.keys = {{KeyGlyph::DpadUD, "Select"}, {KeyGlyph::Confirm, "OK"}, {KeyGlyph::Back, "Return"}};
    return v;
}
} // namespace VR::Lamps
