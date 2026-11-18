// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace VR::SoftSave {
// Guarded by the save IO mutex. A load reads immutable snapshot bytes while normal
// autosaves keep writing the live directory. Never redirect writable opens.
class SnapshotReadLease {
    std::filesystem::path snapshot;
    std::string character;
    bool loading{};

public:
    void Arm(const std::filesystem::path& validated_snapshot, int slot) {
        snapshot = validated_snapshot;
        character = "/savedata0/userdata000" + std::to_string(slot);
    }
    void BeginLoad() {
        loading = true;
    }
    // Cancelling our UI cannot cancel a submitted native load. Keep its read source
    // until the caller observes completion (or a settled return to the title screen).
    bool Cancel() {
        if (loading)
            return false;
        Complete();
        return true;
    }
    void Complete() {
        loading = false;
        snapshot.clear();
        character.clear();
    }
    std::filesystem::path Resolve(std::string_view path, bool read_only) const {
        if (!read_only || snapshot.empty() ||
            (path != character && path != "/savedata0/userdata0010"))
            return {};
        return snapshot / std::filesystem::path(path).filename();
    }
};
} // namespace VR::SoftSave
