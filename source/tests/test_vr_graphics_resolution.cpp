// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "core/vr/graphics_resolution.h"
using namespace VR::Graphics;
TEST(VrGraphicsResolution, HeadsetIsPerEyeAndScaleChangesBothDimensions) {
    auto target = OutputResolution({1632, 1760}, {1632, 1760}, 0);
    EXPECT_EQ(target.width, 1632u);
    EXPECT_EQ(target.height, 1760u);
    auto half = OutputResolution({1632, 1760}, {1632, 1760}, -50);
    EXPECT_EQ(half.width, 816u);
    EXPECT_EQ(half.height, 880u);
    auto larger = OutputResolution({1632, 1760}, {1632, 1760}, 50);
    EXPECT_EQ(larger.width, 2448u);
    EXPECT_EQ(larger.height, 2640u);
    auto quality = InputResolution(target, Upscaler::Dlss, Profile::Quality);
    EXPECT_EQ(quality.width, 1088u);
    EXPECT_EQ(quality.height, 1174u);
    auto perf = InputResolution(target, Upscaler::Fsr4, Profile::Performance);
    EXPECT_EQ(perf.width, 816u);
    EXPECT_EQ(perf.height, 880u);
    auto off = InputResolution(target, Upscaler::Off, Profile::Performance);
    EXPECT_EQ(off.width, target.width);
    EXPECT_EQ(off.height, target.height);
}
TEST(VrGraphicsResolution, LimitsAlignmentAndMissingHeadset) {
    EXPECT_EQ(OutputResolution({}, {1632, 1760}, 0).width, 0u);
    auto limit = OutputResolution({1632, 1760}, {1700, 1800}, 50, {2000, 2100});
    EXPECT_EQ(limit.width, 2000u);
    EXPECT_EQ(limit.height, 2096u);
    for (int scale = -50; scale <= 50; scale += 10) {
        auto target = OutputResolution({1632, 1760}, {1632, 1760}, scale);
        EXPECT_EQ(target.width % 8, 0u);
        EXPECT_EQ(target.height % 8, 0u);
        for (auto provider : {Upscaler::Dlss, Upscaler::Fsr4})
            for (auto preset : {Profile::Performance, Profile::Balanced, Profile::Quality}) {
                auto input = InputResolution(target, provider, preset);
                EXPECT_LE(input.width, target.width);
                EXPECT_LE(input.height, target.height);
                EXPECT_EQ(input.width % 2, 0u);
                EXPECT_EQ(input.height % 2, 0u);
            }
    }
}
TEST(VrGraphicsResolution, TemplateIncludesLightGridUiAndBothDisplayDimensions) {
    const Extent target{1632, 1760};
    EXPECT_EQ(*ResolutionPatchValue(0x02196A6B, "", target), "0x000660B8");
    EXPECT_EQ(*ResolutionPatchValue(0x0235855D, "", target), "0x0006E0B9");
    EXPECT_EQ(*ResolutionPatchValue(0x02695CB6, "", target), "C783A878000030030000");
    EXPECT_EQ(*ResolutionPatchValue(0x02695CC0, "", target), "C783AC78000070030000");
    const std::string stub = "418D4DFF83F97F7718C78424A000000018070000C78424A4000000B0070000EB1E";
    auto value = ResolutionPatchValue(0x02594AA6, stub, target);
    ASSERT_TRUE(value);
    EXPECT_EQ(value->size(), stub.size());
    EXPECT_EQ(value->substr(32, 8), "60060000");
    EXPECT_EQ(value->substr(54, 8), "E0060000");
    EXPECT_EQ(value->substr(40, 14), stub.substr(40, 14));
    EXPECT_FALSE(ResolutionPatchValue(0x02594AA6, "unrecognized stub", target));
    EXPECT_FALSE(ResolutionPatchValue(0x1234, "", target));
}
TEST(VrGraphicsResolution, AcceptedScaleDoesNotMisreportAppliedAllocation) {
    const auto saved = Accepted();
    Config config;
    config.scale_percent = 0;
    accepted.store(Pack(config));
    SetAppliedResolution({1632, 1760}, 0);
    EXPECT_FALSE(ResolutionRestartRequired());
    config.scale_percent = 20;
    accepted.store(Pack(config));
    EXPECT_TRUE(ResolutionRestartRequired());
    EXPECT_EQ(AppliedResolution().width, 1632u);
    accepted.store(Pack(saved));
    applied_scale.store(1000);
    applied_resolution.store(0);
}

#include "core/vr/graphics_restart.h"
TEST(VrGraphicsResolution, RestartPreservesPrivatePathsAndGuestArguments) {
    const std::vector<std::string> args{"--game","G:/game/eboot.bin","--user-dir","C:/private save",
        "--openxr","--patch","private.xml","--wait-for-pid","123","--wait-for-pid=456",
        "--","--wait-for-pid","guest argument"};
    const std::vector<std::string> expected{"--game","G:/game/eboot.bin","--user-dir","C:/private save",
        "--openxr","--patch","private.xml","--","--wait-for-pid","guest argument"};
    EXPECT_EQ(RestartArguments(args),expected);
}

#include "core/vr/graphics_status.h"
TEST(VrGraphicsStatus, SnapshotAndProviderFailuresAreReportedHonestly) {
    Config c;
    c.upscaler = Upscaler::Dlss;
    c.dlss5 = true;
    c.dlss = c.fsr4 = Profile::Balanced;
    RunStatus status{{1088,1174},{1632,1760},RunState::Active,Upscaler::Dlss,Profile::Balanced,true};
    auto decoded = UnpackStatus(PackStatus(status));
    EXPECT_EQ(decoded.input.width, 1088u);
    EXPECT_EQ(decoded.input.height, 1174u);
    EXPECT_EQ(decoded.output.width, 1632u);
    EXPECT_EQ(decoded.output.height, 1760u);
    EXPECT_EQ(decoded.state, RunState::Active);
    EXPECT_EQ(decoded.profile, Profile::Balanced);
    EXPECT_TRUE(decoded.neural);
    EXPECT_EQ(StatusText(c, decoded, {}, false), "DLSS 5 active | 1088x1174 -> 1632x1760 / eye | Neural 1093x1179 / eye");
    c.upscaler = Upscaler::Fsr4;
    EXPECT_NE(StatusText(c, decoded, {}, false).find("Waiting"), std::string::npos);
    decoded.provider = Upscaler::Fsr4;
    decoded.state = RunState::Unavailable;
    EXPECT_NE(StatusText(c, decoded, {}, false).find("unavailable"), std::string::npos);
    decoded.state = RunState::Retrying;
    EXPECT_NE(StatusText(c, decoded, {}, false).find("Reconstruction skipped"), std::string::npos);
    EXPECT_EQ(StatusText(c, decoded, {1632,1760}, true), "Restart required | Current output 1632x1760 / eye");
    c.upscaler = Upscaler::Off;
    EXPECT_EQ(StatusText(c, decoded, {1632,1760}, false), "Native rendering | 1632x1760 / eye");
    for (auto profile : {Profile::Performance, Profile::Balanced, Profile::Quality}) {
        status.profile = profile;
        status.input = status.output = {8192,8192};
        decoded = UnpackStatus(PackStatus(status));
        EXPECT_EQ(decoded.profile, profile);
        EXPECT_EQ(decoded.output.height, 8192u);
        EXPECT_EQ(decoded.input.width, 8192u);
    }
}

TEST(VrGraphicsResolution, NeuralPresetControlsRenderInputWithoutChangingRememberedDlss) {
    Config c; c.upscaler = Upscaler::Dlss; c.dlss5 = true; c.dlss = Profile::Quality;
    EXPECT_EQ(c.neural, Profile::Balanced);
    auto size = InputResolution({1872,2016},c.upscaler,EffectiveProfile(c));
    EXPECT_EQ(size.width,1086u); EXPECT_EQ(size.height,1170u);
    SetNeuralProfile(c,Profile::Performance);
    size = InputResolution({1872,2016},c.upscaler,EffectiveProfile(c));
    EXPECT_EQ(size.width,936u); EXPECT_EQ(size.height,1008u);
    EXPECT_EQ(c.dlss,Profile::Quality);
    c.dlss5=false; EXPECT_EQ(EffectiveProfile(c),Profile::Quality);
}
TEST(VrGraphicsResolution, LegacyNeuralQualityMigratesAndCannotBeRestored) {
    const auto old=Parse("1,0,1,2,1,1,2,75,85,70");
    ASSERT_TRUE(old); EXPECT_EQ(old->neural,Profile::Balanced);
    EXPECT_EQ(old->dlss,Profile::Quality); EXPECT_EQ(EffectiveProfile(*old),Profile::Balanced);
    Config c; SetNeuralProfile(c,Profile::Quality);
    EXPECT_EQ(c.neural,Profile::Balanced); EXPECT_EQ(c.intensity,100); EXPECT_EQ(c.lighting,85); EXPECT_EQ(c.detail,80);
    c.neural=Profile::Quality;
    EXPECT_EQ(Unpack(Pack(c)).neural,Profile::Balanced);
    EXPECT_EQ(Parse(Serialize(c))->neural,Profile::Balanced);
}

TEST(VrGraphicsStatus, PreparationIsDistinctFromActiveAndReportsNativeFallback) {
    Config c; c.upscaler=Upscaler::Dlss; c.dlss5=true;
    PublishStatus(c,RunState::Preparing);
    auto status=UnpackStatus(run_status.load());
    EXPECT_EQ(status.state,RunState::Preparing);
    EXPECT_EQ(StatusText(c,status,{},false),"DLSS 5 preparing | Native rendering until ready");
}
