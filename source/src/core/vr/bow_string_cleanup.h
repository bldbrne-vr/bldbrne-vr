// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "bow_string.h"
namespace VR::Bow {
// Host escrow of the last accepted native edit. Emergency cleanup never asks a failed
// DLL for new policy or retries a legacy recognizer. A fresh animation/rig is untouched.
class StringCleanup {
public:
    void Accept(uint64_t rig, int count, int bone, const StringOutput& result) {
        if (!result.valid)
            return;
        rig_ = rig;
        count_ = count;
        bone_ = bone;
        owned_ = result.owns;
        original_ = result.original;
        written_ = result.written;
    }
    bool Restore(uint64_t rig, int count, int bone, Body::Vec3& current) {
        const bool restore = owned_ && rig == rig_ && count == count_ && bone == bone_ &&
                             Body::IsFinite(current) && Body::LengthSq(current - written_) < 1e-12f;
        owned_ = false;
        if (restore)
            current = original_;
        return restore;
    }

private:
    uint64_t rig_{};
    int count_{}, bone_{};
    bool owned_{};
    Body::Vec3 original_{}, written_{};
};
} // namespace VR::Bow
