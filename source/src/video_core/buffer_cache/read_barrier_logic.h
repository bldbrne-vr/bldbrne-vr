// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

// FAST_GPU_READ_BARRIERS (Buffer::GetBarrierReads, buffer.cpp; the switch in vk_pass_breaks.h):
// the pure logic, no Vulkan, unit-tested in tests/test_read_barriers.cpp. Flags are the
// VkAccessFlagBits2 / VkPipelineStageFlagBits2 values (fixed by the Vulkan spec; buffer.cpp
// static_asserts them against vulkan.hpp).
//
// Buffer::GetBarrier keeps one (access, stage) state per buffer: the last access asked for. With the
// switch off a request equal to the state needs no barrier and any other gets one from the state to
// the request, so a buffer read in turns as a vertex buffer, a storage buffer and an index buffer
// gets a read -> read barrier at every change (and a barrier ends the open render pass,
// Pipeline::BindResources). With it on (ReadStep):
//  - Broadened: the first read after anything else gets one barrier from every earlier command and
//    write (src ALL_COMMANDS, the state's access plus MEMORY_WRITE) to every read at every stage
//    (dst ALL_COMMANDS, AnyRead), and the state becomes (AnyRead, ALL_COMMANDS).
//  - Elided: a read in that state needs no barrier: every write recorded before the broadened
//    barrier is available and visible to every read access at every stage, and no write was asked
//    for since (a write request leaves the state). The writes GetBarrier does not see (uploads,
//    copies, fills, the copy-shader HLE, DMA data) end with their own barrier into MEMORY_READ at
//    ALL_COMMANDS: the invariant FastRender bit 44 (Buffer::shader_reads_visible) already rests on.
//    The exception, a resync from an image, marks the buffer image_data: excluded below.
//  - Old: writes, partial-range barriers (offset != 0) and the excluded reads (Request::excluded,
//    set by GetBarrierReads): a buffer holding image data; a copy's source (TRANSFER stage: always
//    recorded outside a pass, and inside one copy-shader HLE call a JoinOverlap copy can read what
//    an earlier batch copied before that call's closing barrier); a read asked for by the draw
//    whose bindings also asked for a write of this buffer (the write lands after this barrier,
//    inside that draw). The switch-off step, unchanged.
// Every later write still gets a barrier whose source is ALL_COMMANDS (the state's stage), so it
// waits for the elided reads (WAR). While the state is not (AnyRead, ALL_COMMANDS) it equals the
// old step's state, so a broadened barrier replaces one the old step records too (the state differs
// from the request) and covers it (same source state, ALL_COMMANDS to AnyRead). From (AnyRead,
// ALL_COMMANDS) the old path records one for a write, as the old step does, and for an excluded or
// partial read, where the old step may record none (rare: a resync, a copy's source after the same
// kind of read; no caller asks for a partial read); its source (ALL_COMMANDS, AnyRead) still waits
// for every earlier command. So without exclusions the step never records more barriers than the
// old one on the same requests (tests/test_read_barriers.cpp RandomSequences).
#pragma once

#include <cstdint>

namespace VideoCore::ReadBarrierLogic {

// VkAccessFlagBits2.
inline constexpr uint64_t IndirectCommandRead = 0x1;
inline constexpr uint64_t IndexRead = 0x2;
inline constexpr uint64_t VertexAttributeRead = 0x4;
inline constexpr uint64_t UniformRead = 0x8;
inline constexpr uint64_t ShaderRead = 0x20;
inline constexpr uint64_t ShaderWrite = 0x40;
inline constexpr uint64_t TransferRead = 0x800;
inline constexpr uint64_t TransferWrite = 0x1000;
inline constexpr uint64_t HostRead = 0x2000;
inline constexpr uint64_t MemoryRead = 0x8000;
inline constexpr uint64_t MemoryWrite = 0x10000;
// Every read access bit (vr_perf_logic.h ReadOnlyAccess): MEMORY_READ stands for all of them.
inline constexpr uint64_t AllReadBits = 0x1ull | 0x2ull | 0x4ull | 0x8ull | 0x10ull | 0x20ull |
                                        0x80ull | 0x200ull | 0x800ull | 0x2000ull | 0x8000ull |
                                        0x100000000ull | 0x200000000ull;
// VkPipelineStageFlagBits2.
inline constexpr uint64_t AllCommands = 0x10000;

/// Every read a buffer is bound for (vk_pass_breaks.h PassBreaks::AnyRead): the state a broadened
/// barrier leaves.
inline constexpr uint64_t AnyRead = MemoryRead | ShaderRead | UniformRead | VertexAttributeRead |
                                    IndexRead | IndirectCommandRead | TransferRead;

struct State {
    uint64_t access{};
    uint64_t stage{};
    friend constexpr bool operator==(const State&, const State&) = default;
};

struct Masks {
    bool valid{};
    uint64_t src_stage{}, src_access{}, dst_stage{}, dst_access{};
};

struct Request {
    uint64_t access{}, stage{};
    bool whole{};    // the barrier would cover the whole buffer (offset 0)
    bool excluded{}; // image data, or a write of this buffer asked for by the same draw
};

enum class Kind : uint8_t { Same, Elided, Broadened, Old };

/// A non-empty access mask of reads a buffer is bound for (no HOST_READ: that takes the old step).
constexpr bool IsPureRead(uint64_t access) {
    return access != 0 && (access & ~AnyRead) == 0;
}

/// The state in which a read needs no barrier.
constexpr bool Elidable(const State& s) {
    return s.access == AnyRead && s.stage == AllCommands;
}

/// Buffer::GetBarrier with the switch off: the barrier (none when the request equals the state)
/// and the state it leaves.
constexpr Masks OldStep(State& s, uint64_t dst_access, uint64_t dst_stage) {
    if (dst_access == s.access && dst_stage == s.stage) {
        return {};
    }
    const Masks m{true, s.stage, s.access, dst_stage, dst_access};
    s = {dst_access, dst_stage};
    return m;
}

/// FAST_GPU_READ_BARRIERS' step (see the top of this file).
constexpr Kind ReadStep(State& s, const Request& r, Masks& out) {
    out = {};
    if (r.access == s.access && r.stage == s.stage) {
        return Kind::Same;
    }
    if (IsPureRead(r.access) && r.whole && !r.excluded) {
        if (Elidable(s)) {
            return Kind::Elided;
        }
        out = {true, AllCommands, s.access | MemoryWrite, AllCommands, AnyRead};
        s = {AnyRead, AllCommands};
        return Kind::Broadened;
    }
    out = OldStep(s, r.access, r.stage);
    return Kind::Old;
}

/// Stage mask `a` waits for (or blocks) everything `b` does.
constexpr bool StageCovers(uint64_t a, uint64_t b) {
    return (a & AllCommands) != 0 || (b & ~a) == 0;
}

/// Access mask `a` includes every access of `b` (MEMORY_READ every read, MEMORY_WRITE every write).
constexpr bool AccessCovers(uint64_t a, uint64_t b) {
    if (a & MemoryRead) {
        b &= ~AllReadBits;
    }
    if (a & MemoryWrite) {
        b &= AllReadBits;
    }
    return (b & ~a) == 0;
}

/// Barrier `a` orders and makes visible at least what `b` does.
constexpr bool Covers(const Masks& a, const Masks& b) {
    if (!b.valid) {
        return true;
    }
    return a.valid && StageCovers(a.src_stage, b.src_stage) &&
           AccessCovers(a.src_access, b.src_access) && StageCovers(a.dst_stage, b.dst_stage) &&
           AccessCovers(a.dst_access, b.dst_access);
}

/// One barrier that does what both do (FAST_GPU_READ_BARRIERS_VERIFY records it).
constexpr Masks Merge(const Masks& a, const Masks& b) {
    if (!a.valid) {
        return b;
    }
    if (!b.valid) {
        return a;
    }
    return {true, a.src_stage | b.src_stage, a.src_access | b.src_access,
            a.dst_stage | b.dst_stage, a.dst_access | b.dst_access};
}

/// FAST_GPU_READ_BARRIERS_VERIFY's comparison of one request, `now` the step's barrier and state
/// after it, `old` the switch-off step's (run on its own copy of the state).
enum class Mismatch : uint8_t {
    None,
    ElidedWrite, // no barrier where the old step makes a write visible
    Weaker,      // a barrier that does not cover the old one
    Diverged,    // a state that is neither (AnyRead, ALL_COMMANDS) nor the old step's
    Count,
};
constexpr Mismatch Check(const Masks& now, const State& now_state, const Masks& old,
                         const State& old_state) {
    if (old.valid && !now.valid && (old.src_access & ~AllReadBits) != 0) {
        return Mismatch::ElidedWrite;
    }
    if (now.valid && !Covers(now, old)) {
        return Mismatch::Weaker;
    }
    if (!Elidable(now_state) && now_state != old_state) {
        return Mismatch::Diverged;
    }
    return Mismatch::None;
}

/// The old step's barrier only orders a read after a read (what Elided saves).
constexpr bool ReadAfterRead(const Masks& m) {
    return m.valid && (m.src_access & ~AllReadBits) == 0 && (m.dst_access & ~AllReadBits) == 0;
}

} // namespace VideoCore::ReadBarrierLogic
