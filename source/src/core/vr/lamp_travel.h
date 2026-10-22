// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VR::Lamps {
enum class Group { Yharnam, Frontier, Village, Nightmare, HuntersNightmare };
inline constexpr std::array<const char*, 5> GroupNames{"Yharnam", "Frontier", "Unseen Village",
                                                       "Nightmare", "Hunter's Nightmare"};
struct Lamp {
    Group group;
    int area, block, index;
    const char* name;
    constexpr int ReturnPoint() const { return area * 100000 + block * 10000 + 2950 + index; }
    constexpr int Talk() const { return (area * 10 + block) * 1000 + index; }
    constexpr int Lit() const { return 10000000 + (area * 10 + block) * 10000 + 7810 + 20 * index; }
    constexpr int Blocked() const { return 70000000 + (area * 10 + block) * 10 + index; }
    constexpr int NameText() const { return Blocked(); }
};
inline constexpr std::array<Lamp, 44> Table{{
    {Group::Yharnam, 24, 1, 0, "1st Floor Sickroom"},
    {Group::Yharnam, 24, 1, 1, "Central Yharnam"},
    {Group::Yharnam, 24, 1, 2, "Great Bridge"},
    {Group::Yharnam, 24, 1, 3, "Tomb of Oedon"},
    {Group::Yharnam, 24, 0, 0, "Cathedral Ward"},
    {Group::Yharnam, 24, 0, 1, "Grand Cathedral"},
    {Group::Yharnam, 24, 2, 0, "Upper Cathedral Ward"},
    {Group::Yharnam, 24, 2, 2, "Lumenflower Gardens"},
    {Group::Yharnam, 24, 2, 1, "Altar of Despair"},
    {Group::Yharnam, 23, 0, 0, "Old Yharnam"},
    {Group::Yharnam, 23, 0, 1, "Church of the Good Chalice"},
    {Group::Yharnam, 23, 0, 2, "Graveyard of the Darkbeast"},
    {Group::Frontier, 22, 0, 0, "Hemwick Charnel Lane"},
    {Group::Frontier, 22, 0, 1, "Witch's Abode"},
    {Group::Frontier, 27, 0, 0, "Forbidden Woods"},
    {Group::Frontier, 27, 0, 1, "Forbidden Grave"},
    {Group::Frontier, 32, 0, 0, "Byrgenwerth"},
    {Group::Frontier, 32, 0, 2, "Moonside Lake"},
    {Group::Village, 28, 0, 0, "Yahar'gul, Unseen Village"},
    {Group::Village, 28, 0, 3, "Yahar'gul Chapel"},
    {Group::Village, 28, 0, 1, "Advent Plaza"},
    {Group::Village, 28, 0, 2, "Hypogean Gaol"},
    {Group::Village, 25, 0, 0, "Forsaken Castle Cainhurst"},
    {Group::Village, 25, 0, 2, "Logarius' Seat"},
    {Group::Village, 25, 0, 1, "Vileblood Queen's Chamber"},
    {Group::Village, 21, 1, 0, "Abandoned Old Workshop"},
    {Group::Nightmare, 32, 0, 1, "Lecture Building"},
    {Group::Nightmare, 32, 0, 3, "Lecture Building 2nd Floor"},
    {Group::Nightmare, 33, 0, 0, "Nightmare Frontier"},
    {Group::Nightmare, 33, 0, 1, "Amygdala's Chamber"},
    {Group::Nightmare, 26, 0, 0, "Nightmare of Mensis"},
    {Group::Nightmare, 26, 0, 3, "Mergo's Loft: Base"},
    {Group::Nightmare, 26, 0, 2, "Mergo's Loft: Middle"},
    {Group::Nightmare, 26, 0, 1, "Wet Nurse's Lunarium"},
    {Group::HuntersNightmare, 34, 0, 0, "Hunter's Nightmare"},
    {Group::HuntersNightmare, 34, 0, 1, "Nightmare Church"},
    {Group::HuntersNightmare, 34, 0, 3, "Nightmare Grand Cathedral"},
    {Group::HuntersNightmare, 34, 0, 2, "Underground Corpse Pile"},
    {Group::HuntersNightmare, 35, 0, 0, "Research Hall"},
    {Group::HuntersNightmare, 35, 0, 1, "Lumenwood Garden"},
    {Group::HuntersNightmare, 35, 0, 2, "Astral Clocktower"},
    {Group::HuntersNightmare, 36, 0, 0, "Fishing Hamlet"},
    {Group::HuntersNightmare, 36, 0, 1, "Lighthouse Hut"},
    {Group::HuntersNightmare, 36, 0, 2, "Coast"},
}};
inline const Lamp* ByTalk(int id) {
    for (const auto& lamp : Table)
        if (lamp.Talk() == id)
            return &lamp;
    return nullptr;
}
inline const Lamp* ByReturnPoint(int id) {
    for (const auto& lamp : Table)
        if (lamp.ReturnPoint() == id)
            return &lamp;
    return nullptr;
}
// Unknown reads must never unlock a destination.
template <class ReadFlag> bool Available(const Lamp& lamp, ReadFlag&& flag) {
    const std::optional<bool> lit = flag(lamp.Lit()), blocked = flag(lamp.Blocked());
    return lit && *lit && blocked && !*blocked;
}
inline constexpr int RowsPerPage = 5;
struct TravelPage {
    Group group;
    int number{1}, count{1};
    std::vector<int> destinations;
    std::string Title() const {
        return std::string(GroupNames[static_cast<int>(group)]) +
               (count > 1 ? " " + std::to_string(number) + "/" + std::to_string(count) : "");
    }
};
template <class ReadFlag> std::vector<TravelPage> TravelPages(ReadFlag&& flag) {
    std::vector<TravelPage> pages;
    for (int g = 0; g < 5; ++g) {
        std::vector<int> ids;
        for (const auto& lamp : Table)
            if (static_cast<int>(lamp.group) == g && Available(lamp, flag))
                ids.push_back(lamp.ReturnPoint());
        const int count = (static_cast<int>(ids.size()) + RowsPerPage - 1) / RowsPerPage;
        for (int n = 0; n < count; ++n) {
            TravelPage page{static_cast<Group>(g), n + 1, count, {}};
            for (int i = n * RowsPerPage;
                 i < static_cast<int>(ids.size()) && i < (n + 1) * RowsPerPage; ++i)
                page.destinations.push_back(ids[i]);
            pages.push_back(std::move(page));
        }
    }
    return pages;
}
// The lantern's Yes/No box is MessageBox; IngameMessageBox is an object-description box.
inline bool IsLampDialogJob(std::string_view name, bool over_menu) {
    return !over_menu && name == "MessageBox";
}
enum class Pad { Neutral, Back, Yes, Left };
struct PadReceipt {
    Pad pad;
    std::int64_t applied_ns;
};
enum class DialogState { Original, Closed, Changed };
// The menu serial counts opens: a close leaves it (the 2026-10-13 run: open 4, closed 4). Another
// menu opened in between is not our response.
inline DialogState DialogFor(std::uint64_t offered, std::uint64_t current, bool modal,
                             bool same_dialog) {
    if (!same_dialog)
        return DialogState::Changed;
    if (modal && current == offered)
        return DialogState::Original;
    if (!modal && current == offered)
        return DialogState::Closed;
    return DialogState::Changed;
}
enum class Action { None, Rest, Travel, Dream, Leave };

// Compare map identity, not lamp/spawn identity. A respawn-point fallback is not evidence of
// the current map: the hunter may have walked somewhere else since resting there.
inline bool ChangesMapForCleanup(Action action, const Lamp* source, bool source_verified,
                                 const Lamp* destination = nullptr) {
    if (!source || !source_verified)
        return false;
    if (action == Action::Dream)
        return source->area != 21 || source->block != 0; // Hunter's Dream: m21_00
    return action == Action::Travel && destination &&
           (source->area != destination->area || source->block != destination->block);
}

// Arm only after the native warp accepted (or Dream's native Yes response finished). A new
// player alone never proves a different map. Expire so an unrelated later reload cannot match.
class CleanupArrival {
public:
    void Begin(std::uintptr_t player, bool map_changed, std::int64_t now) {
        departing = map_changed ? player : 0;
        expires = now + 30'000'000'000;
    }
    bool Update(std::uintptr_t player, std::int64_t now) {
        if (now > expires)
            departing = 0;
        if (!departing || !player || player == departing)
            return false;
        departing = 0;
        return true;
    }
private:
    std::uintptr_t departing{};
    std::int64_t expires{};
};

struct Request {
    std::uint64_t serial{};
    Action action{Action::None};
    int destination{};
};
struct Offer {
    std::uint64_t serial{};
    int current{};
    std::vector<TravelPage> pages;
};
// No guest pointers cross threads.
class Model {
  public:
    Offer offer;
    bool travel{false};
    int page{0}, focus{0};
    explicit Model(Offer value = {}) : offer(std::move(value)) {}
    int Count() const {
        return travel
                   ? (offer.pages.empty() ? 0
                                          : static_cast<int>(offer.pages[page].destinations.size()))
                   : 4;
    }
    void Move(int delta) {
        const int n = Count();
        if (n)
            focus = (focus + delta % n + n) % n;
    }
    void Tab(int delta) {
        if (!travel || offer.pages.empty())
            return;
        const int n = static_cast<int>(offer.pages.size());
        page = (page + delta % n + n) % n;
        focus = 0;
    }
    std::optional<Request> Back() {
        if (travel) {
            travel = false;
            focus = 1;
            return {};
        }
        return Request{offer.serial, Action::Leave, 0};
    }
    std::optional<Request> Confirm() {
        if (travel) {
            if (!Count())
                return {};
            return Request{offer.serial, Action::Travel, offer.pages[page].destinations[focus]};
        }
        if (focus == 1) {
            travel = true;
            page = focus = 0;
            return {};
        }
        return Request{offer.serial,
                       focus == 0   ? Action::Rest
                       : focus == 2 ? Action::Dream
                                    : Action::Leave,
                       focus == 0 ? offer.current : 0};
    }
};
inline bool ValidRequest(const Offer& offer, const Request& request) {
    if (!offer.serial || request.serial != offer.serial)
        return false;
    if (request.action == Action::Dream || request.action == Action::Leave)
        return true;
    if (request.action == Action::Rest)
        return request.destination == offer.current && ByReturnPoint(request.destination);
    if (request.action != Action::Travel)
        return false;
    for (const auto& page : offer.pages)
        for (int id : page.destinations)
            if (id == request.destination)
                return true;
    return false;
}
} // namespace VR::Lamps
