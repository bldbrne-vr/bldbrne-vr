// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include "core/vr/arm_ik.h"
#include "core/vr/trick_weapons.h"

namespace VR::Melee::Kos {

// Attachment sanity bound, not a damage radius or a new lash reach. Covers the
// 3 m right bundle (+6% fit), the 2.75 m left bundle and the 2.7 m form-A lash.
inline constexpr float MarkerReachM = 4.0f;
inline constexpr int LeftKey = Tricks::KosRuneVariation * 10 + 1;
inline constexpr int LeftJudge = 400;
inline constexpr int LeftBid = 103800400;
inline constexpr int LeftTip = 13100;
inline constexpr int LeftMid = 13110;
inline constexpr std::int64_t CheckTimeNs = 100'000'000;

constexpr float MarkerLimit(int variation, float otherwise) {
    return variation == Tricks::KosRuneVariation ? MarkerReachM : otherwise;
}

// Old learned rows may have been selected using fist proximity. This native row
// belongs unambiguously to the left bundle; all other weapons retain their choice.
constexpr int PreferredLeftJudge(int key, int learned, int known) {
    return key == LeftKey ? LeftJudge : learned >= 0 ? learned : known;
}

// Same signature-checked instance layout as kos_punch.h; caller checks the
// handle and bid via HitEntry first. Read only; never patch shared attack data.
inline bool LeftAttachment(std::span<const std::uint8_t> entry) {
    if (entry.size() < 0x190) return false;
    std::int32_t bid{}, tip{}, mid{};
    std::memcpy(&bid, entry.data() + 0x8C, sizeof(bid));
    std::memcpy(&tip, entry.data() + 0x188, sizeof(tip));
    std::memcpy(&mid, entry.data() + 0x18C, sizeof(mid));
    return bid == LeftBid && tip == LeftTip && mid == LeftMid;
}

enum class Check { Wait, Accept, Reject };

// Per swing, fixed storage. Collision can report the preceding pose's endpoint;
// accept that one recent, validated marker too. No dependency on the other hand.
class LeftCheck {
public:
    void Begin(std::int64_t now) { *this = {}; start_ = now; state_ = Check::Wait; }
    bool Accepted() const { return state_ == Check::Accept; }

    Check Step(std::int64_t now, bool attachment_ok, Body::Vec3 hit,
               bool marker_available, Body::Vec3 marker, Body::Vec3 grip) {
        if (state_ != Check::Wait) return state_;
        if (start_ < 0 || now < start_)
            return state_ = Check::Reject;
        const bool marker_ok = marker_available && Body::IsFinite(marker) &&
            Body::IsFinite(grip) && Body::LengthSq(marker - grip) <= MarkerReachM * MarkerReachM;
        const bool previous_ok = previous_valid_ && now >= previous_ns_ &&
                                 now - previous_ns_ <= CheckTimeNs;
        constexpr float error_sq = 0.20f * 0.20f;
        const bool match = attachment_ok && marker_ok && Body::IsFinite(hit) &&
            Body::LengthSq(hit) > 0.0f &&
            (Body::LengthSq(hit - marker) <= error_sq ||
             (previous_ok && Body::LengthSq(hit - previous_) <= error_sq));
        previous_valid_ = marker_ok;
        previous_ = marker;
        previous_ns_ = now;
        // A hitch can delay the next observation beyond the retry interval. A
        // matching current endpoint is still valid evidence; time out only an
        // unresolved mismatch. Reject remains terminal until the next Begin.
        return state_ = match ? Check::Accept
                             : now - start_ >= CheckTimeNs ? Check::Reject : Check::Wait;
    }

private:
    Body::Vec3 previous_{};
    std::int64_t start_{}, previous_ns_{};
    bool previous_valid_{false};
    Check state_{Check::Reject};
};

} // namespace VR::Melee::Kos
