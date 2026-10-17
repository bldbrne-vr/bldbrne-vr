// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/vr/lever_update_queue.h"
#include "core/vr/physical_lever.h"
#include <cstring>

namespace VR::Lever {
// Views are acquired and checked by the game-thread adapter immediately before use.
// Never retain these across a map/object lifetime change or write from the XR thread.
struct PoseBuffers {
    std::span<Body::HkQsTransform> locals;
    std::span<std::uint32_t> flags;
    std::uint16_t &sync; // low byte: model valid; high byte: locals valid
};
struct UpdateQueue {
    std::uint16_t index;
    std::uint32_t capacity;
    std::span<std::uint16_t> indices;
    std::uint32_t &count;
    std::uint16_t &pending;
    std::uint16_t &active;
};

// Own exactly one existing bone during a physical hold. An external animation write
// revokes ownership; cancellation must never put an obsolete rest pose over that write.
// The game adapter must synchronize the invalidated model pose before publishing it.
struct LegacyPoseGeometry {
    bool Valid(const BoneBinding& binding) const { return BindRig(binding).has_value(); }
    std::optional<Body::QsTransform> Target(const BoneBinding& binding,float angle) const {
        return BoneTarget(binding,angle);
    }
};
template <class GeometryPolicy> class BasicPoseEdit {
public:
    GeometryPolicy& Policy() { return geometry_; }
    bool Begin(const BoneBinding &binding, PoseBuffers pose, std::size_t bone) {
        if (owned_ || !geometry_.Valid(binding) || !ValidBuffers(pose) || bone >= pose.locals.size()) {
            return false;
        }
        auto expected = pose.locals[bone];
        Body::ToHavok(binding.rest_local, expected);
        if (!Same(expected, pose.locals[bone])) {
            return false;
        }
        binding_ = binding;
        original_ = last_ = pose.locals[bone];
        locals_ = pose.locals.data();
        flags_ = pose.flags.data();
        bone_ = bone;
        count_ = pose.locals.size();
        owned_ = true;
        return true;
    }

    // Own a bone an earlier edit left turned and then forgot (a parked capstan, lever_models.h
    // Parked): only while the bone is still exactly, byte for byte, as that edit left it (`last`),
    // and only with `binding` at the bone's true rest (`original`, which Restore puts back). The
    // edit then carries on from there as if it had never let go.
    bool Resume(const BoneBinding &binding, PoseBuffers pose, std::size_t bone,
                const Body::HkQsTransform &original, const Body::HkQsTransform &last) {
        if (owned_ || !geometry_.Valid(binding) || !ValidBuffers(pose) || bone >= pose.locals.size() ||
            !Same(pose.locals[bone], last)) {
            return false;
        }
        auto expected = original;
        Body::ToHavok(binding.rest_local, expected);
        if (!Same(expected, original)) {
            return false;
        }
        binding_ = binding;
        original_ = original;
        last_ = last;
        locals_ = pose.locals.data();
        flags_ = pose.flags.data();
        bone_ = bone;
        count_ = pose.locals.size();
        owned_ = true;
        return true;
    }

    bool Apply(float angle, PoseBuffers pose, UpdateQueue queue) {
        const auto target = geometry_.Target(binding_, angle);
        if (!target || !Matches(pose)) {
            return false;
        }
        auto next = original_;
        Body::ToHavok(*target, next);
        return Commit(next, pose, queue);
    }

    bool Restore(PoseBuffers pose, UpdateQueue queue) {
        if (!Matches(pose) || !Commit(original_, pose, queue)) {
            return false;
        }
        owned_ = false;
        return true;
    }

    void Forget() { owned_ = false; } // lifetime change: never touch an old pointer
    bool Owned() const { return owned_; }
    // The owned bone at its true rest, and as last written (a parked capstan's record).
    const Body::HkQsTransform &Original() const { return original_; }
    const Body::HkQsTransform &Last() const { return last_; }

private:
    static bool Same(const Body::HkQsTransform &a, const Body::HkQsTransform &b) {
        return std::memcmp(&a, &b, sizeof(a)) == 0;
    }
    static bool ValidBuffers(PoseBuffers pose) {
        if (pose.locals.empty() || pose.locals.size() != pose.flags.size() ||
            pose.locals.size() > 256 || (pose.sync != 0x0100 && pose.sync != 0x0101)) {
            return false;
        }
        for (const auto &local : pose.locals) {
            if (!Body::IsFinite(Body::FromHavok(local))) {
                return false;
            }
        }
        return true;
    }
    bool Matches(PoseBuffers pose) {
        if (!owned_ || !ValidBuffers(pose) || pose.locals.data() != locals_ ||
            pose.flags.data() != flags_ || pose.locals.size() != count_ ||
            !Same(pose.locals[bone_], last_)) {
            owned_ = false;
            return false;
        }
        return true;
    }
    bool Commit(const Body::HkQsTransform &next, PoseBuffers pose, UpdateQueue queue) {
        // All fallible validation precedes the first mutation. A full/inconsistent queue
        // leaves the pose unchanged, allowing the caller to retry or cancel cleanly.
        if (RequestModelRefresh(queue.index, queue.capacity, queue.indices, queue.count,
                                queue.pending, queue.active) == RefreshResult::Invalid) {
            return false;
        }
        pose.locals[bone_] = next;
        last_ = next;
        // Same invalidation contract as setPoseLocalSpace and the existing VR body writer.
        for (auto &flag : pose.flags) {
            flag = 2;
        }
        pose.sync = 0x0100;
        return true;
    }
    GeometryPolicy geometry_{};
    BoneBinding binding_{};
    Body::HkQsTransform original_{}, last_{};
    Body::HkQsTransform *locals_{};
    std::uint32_t *flags_{};
    std::size_t bone_{}, count_{};
    bool owned_{};
};
using PoseEdit = BasicPoseEdit<LegacyPoseGeometry>;
} // namespace VR::Lever
