// SPDX-License-Identifier: GPL-2.0-or-later
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
