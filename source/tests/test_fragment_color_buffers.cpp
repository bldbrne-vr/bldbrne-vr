// SPDX-License-Identifier: GPL-2.0-or-later

// The colour targets in a fragment permutation (shader_recompiler/runtime_info.h,
// FragmentRuntimeInfo::KeepWrittenColorBuffers): the recompiler reads color_buffers[i] only for
// the targets the program exports (Info::mrt_mask; with dual source blending MRT1's export reads
// target 0), so the others are cleared; of a written target it reads the export format, the
// number conversion, the swizzle through CompMapping::Map and the number format's class, and the
// data format and exact number format only for the unorm fix-up. Two draws that bind other
// targets, or other formats for them, then select the same permutation: equal for the
// StageSpecialization's comparison, equal bytes for the spec memo, and equal again after the meta.

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "common/serdes.h"
#include "shader_recompiler/runtime_info.h"

namespace {

using AmdGpu::CompMapping;
using AmdGpu::CompSwizzle;
using AmdGpu::DataFormat;
using AmdGpu::NumberConversion;
using AmdGpu::NumberFormat;
using AmdGpu::ShaderExportFormat;
using Shader::MaxColorBuffers;
using Shader::PsColorBuffer;
using Shader::RuntimeInfo;
using Shader::Stage;

constexpr bool NoFixup = false;   // every desktop driver (Profile::needs_unorm_fixup)
constexpr bool UnormFixup = true; // Mesa KosmicKrisp

constexpr CompMapping Identity{.r = CompSwizzle::Red,
                               .g = CompSwizzle::Green,
                               .b = CompSwizzle::Blue,
                               .a = CompSwizzle::Alpha};
// A three-channel format's swizzle (Format10_11_11): no alpha.
constexpr CompMapping NoAlpha{.r = CompSwizzle::Red,
                              .g = CompSwizzle::Green,
                              .b = CompSwizzle::Blue,
                              .a = CompSwizzle::Zero};

PsColorBuffer Target(DataFormat data, NumberFormat num, ShaderExportFormat exported,
                     CompMapping swizzle = Identity) {
    PsColorBuffer cb;
    std::memset(&cb, 0, sizeof(cb)); // as RefreshGraphicsKey's key starts
    cb.data_format = data;
    cb.num_format = num;
    cb.num_conversion = NumberConversion::None;
    cb.export_format = exported;
    cb.swizzle = swizzle;
    return cb;
}

// A fragment stage as BuildRuntimeInfo gives it: every bound, enabled target copied from the key.
RuntimeInfo Fragment(DataFormat lighting, DataFormat others) {
    RuntimeInfo info{};
    info.Initialize(Stage::Fragment);
    info.native_stereo = true;
    info.native_multiview = true;
    info.fs_info.num_inputs = 2;
    const auto swizzle = lighting == DataFormat::Format10_11_11 ? NoAlpha : Identity;
    for (std::uint32_t i = 0; i < MaxColorBuffers; ++i) {
        info.fs_info.color_buffers[i] =
            i < 2 ? Target(lighting, NumberFormat::Float, ShaderExportFormat::ABGR_FP16, swizzle)
                  : Target(others, NumberFormat::Unorm, ShaderExportFormat::ABGR_UNORM16);
    }
    return info;
}

bool SameBytes(const RuntimeInfo& a, const RuntimeInfo& b) {
    return std::memcmp(&a, &b, sizeof(RuntimeInfo)) == 0;
}

TEST(FragmentColorBuffers, UnwrittenTargetsNoLongerMakePermutations) {
    // cb0/cb1 go from Format10_11_11 to Format16_16_16_16 and cb5/cb6 change too. A program
    // writing only MRT2 and MRT3 never reads them: one permutation.
    auto a = Fragment(DataFormat::Format10_11_11, DataFormat::Format8_8_8_8);
    auto b = Fragment(DataFormat::Format16_16_16_16, DataFormat::Format8_8_8_8);
    b.fs_info.color_buffers[5] = {};
    b.fs_info.color_buffers[6] =
        Target(DataFormat::Format32_32_32_32, NumberFormat::Float, ShaderExportFormat::ABGR_32);
    EXPECT_FALSE(a == b);
    constexpr std::uint32_t written = 0b1100;
    for (const bool fixup : {NoFixup, UnormFixup}) {
        auto x = a;
        auto y = b;
        x.fs_info.KeepWrittenColorBuffers(written, fixup);
        y.fs_info.KeepWrittenColorBuffers(written, fixup);
        EXPECT_TRUE(x == y) << fixup;
        EXPECT_TRUE(SameBytes(x, y)) << fixup; // the spec memo compares the bytes
    }
}

TEST(FragmentColorBuffers, TheLoggedTargetFormatChangeIsOnePermutation) {
    // The user's headset cache: the light and G-buffer programs write MRT0 and MRT1 (one also
    // MRT4), and those targets went from Format10_11_11 (alpha Zero) to Format16_16_16_16 (alpha
    // Alpha), both float with the same FP16 export. The SPIR-V reads neither the data format nor
    // a Zero alpha (CompMapping::Map keeps the component): one permutation.
    const auto a = Fragment(DataFormat::Format10_11_11, DataFormat::Format8_8_8_8);
    const auto b = Fragment(DataFormat::Format16_16_16_16, DataFormat::Format8_8_8_8);
    constexpr std::uint32_t written = 0b1'0011;
    auto x = a;
    auto y = b;
    x.fs_info.KeepWrittenColorBuffers(written, NoFixup);
    y.fs_info.KeepWrittenColorBuffers(written, NoFixup);
    EXPECT_TRUE(x == y);
    EXPECT_TRUE(SameBytes(x, y));
    // The unorm fix-up reads the data format: there it stays a permutation of its own.
    x = a;
    y = b;
    x.fs_info.KeepWrittenColorBuffers(written, UnormFixup);
    y.fs_info.KeepWrittenColorBuffers(written, UnormFixup);
    EXPECT_FALSE(x == y);
}

TEST(FragmentColorBuffers, WrittenTargetsKeepWhatTheRecompilerReads) {
    const auto bound = Fragment(DataFormat::Format16_16_16_16, DataFormat::Format8_8_8_8);
    constexpr std::uint32_t written = 0b1000'0101; // MRT0, MRT2, MRT7
    const PsColorBuffer cleared{};
    for (const bool fixup : {NoFixup, UnormFixup}) {
        auto masked = bound;
        masked.fs_info.KeepWrittenColorBuffers(written, fixup);
        for (std::uint32_t i = 0; i < MaxColorBuffers; ++i) {
            const auto& cb = masked.fs_info.color_buffers[i];
            const auto& in = bound.fs_info.color_buffers[i];
            if ((written & (1u << i)) == 0) {
                EXPECT_TRUE(cb == cleared) << i;
                EXPECT_TRUE(cb.export_format == ShaderExportFormat::Zero) << i;
                continue;
            }
            EXPECT_TRUE(cb.export_format == in.export_format) << i;
            EXPECT_TRUE(cb.num_conversion == in.num_conversion) << i;
            EXPECT_TRUE(cb.swizzle == in.swizzle) << i; // already the identity
            EXPECT_TRUE(AmdGpu::GetNumberClass(cb.num_format) ==
                        AmdGpu::GetNumberClass(in.num_format))
                << i;
            if (fixup) {
                // Mesa KosmicKrisp: the translator reads both formats, kept as they were.
                EXPECT_EQ(std::memcmp(&cb, &in, sizeof(cb)), 0) << i;
            } else {
                EXPECT_TRUE(cb.data_format == DataFormat::FormatInvalid) << i;
            }
        }
        // Nothing else in the runtime info changes.
        auto expected = bound;
        expected.fs_info.color_buffers = masked.fs_info.color_buffers;
        EXPECT_TRUE(SameBytes(masked, expected));
    }
}

TEST(FragmentColorBuffers, ANumberFormatCountsByItsClass) {
    // The output's type: float (unorm, snorm, srgb, float...), unsigned or signed integer.
    const auto masked = [](NumberFormat num) {
        auto info = Fragment(DataFormat::Format8_8_8_8, DataFormat::Format8_8_8_8);
        info.fs_info.color_buffers[0].num_format = num;
        info.fs_info.KeepWrittenColorBuffers(0b0001, NoFixup);
        return info;
    };
    const auto unorm = masked(NumberFormat::Unorm);
    EXPECT_TRUE(SameBytes(unorm, masked(NumberFormat::Srgb)));
    EXPECT_TRUE(SameBytes(unorm, masked(NumberFormat::Snorm)));
    EXPECT_TRUE(SameBytes(unorm, masked(NumberFormat::Float)));
    EXPECT_FALSE(unorm == masked(NumberFormat::Uint));
    EXPECT_FALSE(unorm == masked(NumberFormat::Sint));
    EXPECT_FALSE(masked(NumberFormat::Uint) == masked(NumberFormat::Sint));
}

TEST(FragmentColorBuffers, ZeroAndOneSwizzlesKeepTheirComponent) {
    // CompMapping::Map: a component swizzled to Zero or One exports its own component.
    const auto masked = [](CompMapping swizzle) {
        auto info = Fragment(DataFormat::Format16_16_16_16, DataFormat::Format8_8_8_8);
        info.fs_info.color_buffers[1].swizzle = swizzle;
        info.fs_info.KeepWrittenColorBuffers(0b0010, NoFixup);
        return info;
    };
    const auto identity = masked(Identity);
    EXPECT_TRUE(SameBytes(identity, masked(NoAlpha)));
    EXPECT_TRUE(SameBytes(identity, masked({.r = CompSwizzle::One,
                                            .g = CompSwizzle::Green,
                                            .b = CompSwizzle::Zero,
                                            .a = CompSwizzle::Alpha})));
    // A real component swap is read.
    EXPECT_FALSE(identity == masked({.r = CompSwizzle::Blue,
                                     .g = CompSwizzle::Green,
                                     .b = CompSwizzle::Red,
                                     .a = CompSwizzle::Alpha}));
}

TEST(FragmentColorBuffers, AWrittenTargetsChangeIsStillANewPermutation) {
    const auto masked = [](auto&& change) {
        auto info = Fragment(DataFormat::Format16_16_16_16, DataFormat::Format8_8_8_8);
        change(info.fs_info.color_buffers[0]);
        info.fs_info.KeepWrittenColorBuffers(0b0001, NoFixup);
        return info;
    };
    const auto base = masked([](PsColorBuffer&) {});
    // The number class, the export format, the swizzle and the number conversion count.
    EXPECT_FALSE(base == masked([](PsColorBuffer& cb) { cb.num_format = NumberFormat::Uint; }));
    EXPECT_FALSE(
        base == masked([](PsColorBuffer& cb) { cb.export_format = ShaderExportFormat::Zero; }));
    EXPECT_FALSE(
        base == masked([](PsColorBuffer& cb) { cb.export_format = ShaderExportFormat::ABGR_32; }));
    EXPECT_FALSE(base == masked([](PsColorBuffer& cb) { cb.swizzle.r = CompSwizzle::Blue; }));
    EXPECT_FALSE(base == masked([](PsColorBuffer& cb) {
                     cb.num_conversion = NumberConversion::UintToUscaled;
                 }));
}

TEST(FragmentColorBuffers, DualSourceBlendingKeepsTargetZeroForMrt1) {
    // MRT1's export is the second blend source: it converts with target 0's entry.
    auto dual = Fragment(DataFormat::Format8_8_8_8, DataFormat::Format8_8_8_8);
    dual.fs_info.dual_source_blending = true;
    const auto target0 = dual.fs_info.color_buffers[0];
    dual.fs_info.KeepWrittenColorBuffers(0b0010, UnormFixup);
    EXPECT_TRUE(dual.fs_info.color_buffers[0] == target0);
    EXPECT_FALSE(dual.fs_info.color_buffers[1] == PsColorBuffer{});
    EXPECT_TRUE(dual.fs_info.color_buffers[2] == PsColorBuffer{});
    auto read = Fragment(DataFormat::Format8_8_8_8, DataFormat::Format8_8_8_8);
    read.fs_info.dual_source_blending = true;
    read.fs_info.KeepWrittenColorBuffers(0b0010, NoFixup);
    EXPECT_TRUE(read.fs_info.color_buffers[0].export_format == target0.export_format);
    // Without it, an MRT1-only program never reads target 0.
    auto single = Fragment(DataFormat::Format8_8_8_8, DataFormat::Format8_8_8_8);
    single.fs_info.KeepWrittenColorBuffers(0b0010, NoFixup);
    EXPECT_TRUE(single.fs_info.color_buffers[0] == PsColorBuffer{});
    EXPECT_FALSE(single.fs_info.color_buffers[1] == PsColorBuffer{});
}

TEST(FragmentColorBuffers, MaskingTwiceIsMaskingOnce) {
    // The owner masks its selection, a preparation worker masks the copy it is handed again.
    for (const bool fixup : {NoFixup, UnormFixup}) {
        auto once = Fragment(DataFormat::Format10_11_11, DataFormat::Format8_8_8_8);
        once.fs_info.KeepWrittenColorBuffers(0b0110, fixup);
        auto twice = once;
        twice.fs_info.KeepWrittenColorBuffers(0b0110, fixup);
        EXPECT_TRUE(SameBytes(once, twice)) << fixup;
    }
}

TEST(FragmentColorBuffers, NoExportsKeepNoTargets) {
    // A depth-only or discard-only program: every bound target is left out.
    auto info = Fragment(DataFormat::Format16_16_16_16, DataFormat::Format8_8_8_8);
    info.fs_info.KeepWrittenColorBuffers(0, NoFixup);
    for (const auto& cb : info.fs_info.color_buffers) {
        EXPECT_TRUE(cb == PsColorBuffer{});
    }
}

TEST(FragmentColorBuffers, TheMaskedSpecLoadsEqualFromTheMeta) {
    // StageSpecialization::Serialize writes the RuntimeInfo as one value: a cached permutation
    // matches a later selection that binds other unwritten targets, or another data format for a
    // written one.
    auto cached = Fragment(DataFormat::Format10_11_11, DataFormat::Format8_8_8_8);
    cached.fs_info.KeepWrittenColorBuffers(0b0101, NoFixup);
    Serialization::Archive out;
    Serialization::Writer writer{out};
    writer.Write(cached);
    Serialization::Archive in{out.TakeOff()};
    Serialization::Reader reader{in};
    RuntimeInfo loaded{};
    reader.Read(loaded);
    EXPECT_FALSE(in.Failed());
    auto later = Fragment(DataFormat::Format16_16_16_16, DataFormat::Format8_8_8_8);
    later.fs_info.KeepWrittenColorBuffers(0b0101, NoFixup);
    EXPECT_TRUE(loaded == later);
    EXPECT_TRUE(SameBytes(loaded, later));
}

} // namespace
