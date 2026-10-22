// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/lamp_travel.h"

namespace VR::Lamps {
// Presenter-thread state. Revisions follow selection semantics, not hover animations.
// A finished CPU render is not enough: the matching image must reach an accepted XR frame.
class Presentation {
    struct Selection {
        std::uint64_t offer;
        bool travel;
        int page, focus;
        bool operator==(const Selection&) const = default;
    };
    static Selection Key(const Model& model) {
        return {model.offer.serial, model.travel, model.page, model.focus};
    }
    std::optional<Selection> selection;
    std::uint64_t revision{}, submitted{};

  public:
    void Reset() {
        selection.reset();
        submitted = 0;
        // Keep revisions unique across lamp sessions, including reused native menu serials.
    }
    std::uint64_t Update(const Model& model) {
        const auto next = Key(model);
        if (!selection || *selection != next) {
            selection = next;
            ++revision;
        }
        return revision;
    }
    void Presented(std::uint64_t value) { submitted = value; }
    bool Ready(const Model& model) const {
        return selection && *selection == Key(model) && revision && submitted == revision;
    }
    std::optional<Request> Confirm(Model& model, int row = -1) const {
        if (!Ready(model) || row < -1 || row >= model.Count())
            return {};
        if (row >= 0)
            model.focus = row;
        return model.Confirm();
    }
};
} // namespace VR::Lamps
