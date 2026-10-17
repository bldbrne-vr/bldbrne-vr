// SPDX-License-Identifier: GPL-2.0-or-later

// core/patch_fault.h: a privileged-instruction fault exactly at a site the CPU patcher rewrote (a
// fetch torn across the rewrite) resumes on the finished jump in every mode; any other privileged
// instruction is handled as before (static red-zone patching: the patch handler; else reported).

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "core/patch_fault.h"

namespace {

using Core::PatchFault::OnPrivilegedInstruction;
using Core::PatchFault::PrivAction;

TEST(PatchFault, ATornFetchAtAPatchedSiteResumesInEveryMode) {
    // The user's usertest_1113_crash.txt ran without static patching: reported, the emulator shut
    // down. Now it resumes.
    EXPECT_EQ(OnPrivilegedInstruction(true, false), PrivAction::ResumeAtSite);
    EXPECT_EQ(OnPrivilegedInstruction(true, true), PrivAction::ResumeAtSite);
}

TEST(PatchFault, AnyOtherPrivilegedInstructionIsHandledAsBefore) {
    EXPECT_EQ(OnPrivilegedInstruction(false, false), PrivAction::Report);
    EXPECT_EQ(OnPrivilegedInstruction(false, true), PrivAction::PatchHandler);
}

// x86 port I/O (in / out / ins / outs): privileged at user mode's IOPL 0.
constexpr bool IsPortIo(std::uint8_t op) {
    return (op >= 0xE4 && op <= 0xE7) || (op >= 0xEC && op <= 0xEF) || (op >= 0x6C && op <= 0x6F);
}

TEST(PatchFault, TheCrashesWereTornFetchesOfFinishedJumps) {
    // The bytes at the fault address in every 0xc0000096 report: a finished near jump (E9 rel32,
    // then NOP padding or the next instruction). A fetch with the old first two bytes (the FS
    // access's prefixes, e.g. 64 48) and the new tail decodes byte 2 - the rel32's second byte -
    // as the opcode: port I/O in all five.
    struct Crash {
        const char* where;
        std::array<std::uint8_t, 8> bytes;
    };
    const std::array<Crash, 5> crashes{{
        {"usertest_1113_crash (the user's)", {0xe9, 0xdf, 0xec, 0x42, 0x04, 0x90, 0x90, 0x90}},
        {"kit/shake5c", {0xe9, 0x3c, 0xee, 0x42, 0x04, 0x90, 0x90, 0x90}},
        {"duck3/after_crash_flake", {0xe9, 0x90, 0xed, 0x42, 0x04, 0x90, 0x90, 0x90}},
        {"perf/me/new_1", {0xe9, 0x5e, 0xef, 0x42, 0x04, 0x90, 0x90, 0x90}},
        {"review/r6b_face_final", {0xe9, 0x49, 0x6d, 0xf6, 0x04, 0x0f, 0x1f, 0x40}},
    }};
    for (const auto& crash : crashes) {
        EXPECT_EQ(crash.bytes[0], 0xe9) << crash.where; // the finished jump: runs on resume
        EXPECT_TRUE(IsPortIo(crash.bytes[2])) << crash.where;
        EXPECT_FALSE(IsPortIo(crash.bytes[0])) << crash.where;
    }
}

} // namespace
