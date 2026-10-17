// SPDX-License-Identifier: GPL-2.0-or-later

// Object-motion permutations in the disk cache (shader_recompiler/vr_motion.h, common/serdes.h):
// with Shader::VrPushData a motion variant's runtime info keeps vr_motion's flag alone
// (VrMotion::Variant). The meta holds it as StageSpecialization::Serialize writes it (the
// RuntimeInfo's bytes); loaded, it compares equal to the runtime info any later allocation of the
// motion buffers selects, so GetProgram finds the preloaded permutation and the motion pipeline's
// key is the cached one. A baked one (addresses inside) never matches it.

#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "common/serdes.h"
#include "shader_recompiler/runtime_info.h"

namespace {

using Shader::RuntimeInfo;
using Shader::Stage;
using Shader::VrMotion;

VrMotion Buffers(std::uint64_t params, std::uint64_t positions) {
    VrMotion motion{};
    motion.params_address = params;
    motion.positions_address = positions;
    motion.enabled = true;
    return motion;
}

// A motion variant's stage as BuildRuntimeInfo gives it: the selection's vr_motion copied in,
// the live buffers stripped for VrPushData (PipelineCache::VrMotionVariant).
RuntimeInfo MotionStage(Stage stage, const VrMotion& live) {
    RuntimeInfo info{};
    info.Initialize(stage);
    info.native_stereo = true;
    info.native_asymmetric = true;
    info.vr_motion = live.Variant(true);
    return info;
}

// Through the meta: StageSpecialization::Serialize / Deserialize write and read the RuntimeInfo
// as one value.
RuntimeInfo ThroughTheMeta(const RuntimeInfo& info) {
    Serialization::Archive out;
    Serialization::Writer writer{out};
    writer.Write(info);
    Serialization::Archive in{out.TakeOff()};
    Serialization::Reader reader{in};
    RuntimeInfo loaded{};
    reader.Read(loaded);
    EXPECT_FALSE(in.Failed());
    EXPECT_TRUE(in.IsEoS());
    return loaded;
}

TEST(VrMotionCache, TheRuntimeInfoIsWrittenAsItsBytes) {
    static_assert(std::is_trivially_copyable_v<VrMotion>);
    static_assert(std::is_trivially_copyable_v<RuntimeInfo>);
    EXPECT_EQ(sizeof(VrMotion), 24u); // two addresses and the flag: the meta layout (meta 12)
}

TEST(VrMotionCache, AFlagOnlyPermutationLoadsEqualForEveryAllocation) {
    const VrMotion live = Buffers(0x7F00'0001'0000ull, 0x7F00'0002'0000ull);
    // The next session's buffers, or this one's after VrObjectMotion grows its history.
    const VrMotion later = Buffers(0x7F00'0040'0000ull, 0x7F00'0080'0000ull);
    for (const Stage stage : {Stage::Vertex, Stage::Fragment}) {
        const RuntimeInfo written = MotionStage(stage, live);
        const RuntimeInfo loaded = ThroughTheMeta(written);
        EXPECT_TRUE(loaded == written);
        // LoadPipelineStage keeps it (only a baked one is rejected).
        EXPECT_TRUE(loaded.vr_motion.enabled);
        EXPECT_FALSE(loaded.vr_motion.Baked());
        // The same permutation for other buffers: no translation, the same key.
        EXPECT_TRUE(loaded == MotionStage(stage, later));
        // Never the plain permutation of the same program, nor a baked one.
        RuntimeInfo plain = written;
        plain.vr_motion = {};
        EXPECT_FALSE(loaded == plain);
        RuntimeInfo baked = written;
        baked.vr_motion = live;
        EXPECT_FALSE(loaded == baked);
    }
}

} // namespace
