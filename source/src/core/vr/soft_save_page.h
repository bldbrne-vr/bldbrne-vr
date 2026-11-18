// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/vr_page_render.h"
namespace VR::SoftSave {
// `when`: the soft save's time (Channel::When), empty when unknown.
inline Page::View DeathPage(int focus, const Page::Glyphs& glyphs, Page::Hover hover = {},
                            const std::string& when = {}) {
    Page::View view;
    view.glyphs = glyphs;
    view.hover = hover;
    view.single_column = true;
    view.tabs = {"Return after death"};
    view.headers[0] = "Choose where to continue";
    view.rows[0] = {true, "Last lantern", "", Page::Widget::Action, 0, 0.f, focus == 0};
    view.rows[1] = {true, "Soft save", "", Page::Widget::Action, 0, 0.f, focus == 1};
    view.help = focus == 0 ? "Return to your last lantern and clear the soft save."
                : when.empty()
                    ? "Return to your soft save. Loading is automatic."
                    : "Return to your soft save from " + when + ". Loading is automatic.";
    view.keys = {{Page::KeyGlyph::DpadUD, "Select"},
                 {Page::KeyGlyph::Confirm, "OK"},
                 {Page::KeyGlyph::Back, "Last lantern"}};
    return view;
}
} // namespace VR::SoftSave
