// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "common/hash.h"
#include "common/decoder.h"
#include "common/thread.h"
#include "core/memory.h"
#include "core/vr/vr_session.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_prepared_bindings.h"
#include "../../../scratchpad/dual-eye-perf/shader_header_read.h"
#include "../../../scratchpad/dual-eye-perf/draw_registers.h"
#include "../../../scratchpad/dual-eye-perf/graphics_packet_replay.h"
#include "../../../scratchpad/dual-eye-perf/graphics_submission_scan.h"
#include "../../../scratchpad/dual-eye-perf/native_stereo.h"
#include "../../../scratchpad/dual-eye-perf/register_delta.h"
#include <magic_enum/magic_enum.hpp>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include "../../../scratchpad/dual-eye-perf/speculative_pool.h"
#include "../../../scratchpad/dual-eye-perf/speculative_read.h"

namespace Vulkan {
namespace {
// No owner, allocation or C++ unwinding is allowed inside these callbacks.
bool ReadRaw(void (*read)(void*), void* context) {
#ifdef _WIN32
    return ScratchDraw::TrySpeculativeRead(read, context);
#else
    // The Windows scratchpad does not install a POSIX speculative fault boundary.
    return false;
#endif
}
bool CopyRaw(void* target, const void* source, size_t bytes) {
    struct Copy { void* target; const void* source; size_t bytes; } copy{target, source, bytes};
    return ReadRaw([](void* p) {
        const auto& c = *static_cast<Copy*>(p);
        std::memcpy(c.target, c.source, c.bytes);
    }, &copy);
}
bool SameRaw(const void* source, std::span<const u32> words) {
    struct Compare { const void* source; std::span<const u32> words; bool equal{}; } c{source, words};
    return ReadRaw([](void* p) {
        auto& c = *static_cast<Compare*>(p);
        c.equal = std::memcmp(c.source, c.words.data(), c.words.size_bytes()) == 0;
    }, &c) && c.equal;
}
// Decode only the fixed instruction grammar emitted by GenerateSrtProgram. The
// JIT pushes pointers without Windows unwind metadata, so it cannot run inside SEH.
// Unknown/patched instructions reject live preparation instead of executing code.
std::optional<std::vector<ScratchDraw::SrtReadStep>> MakeReadPlan(const Shader::Info& info) {
    using Step = ScratchDraw::SrtReadStep;
    std::vector<Step> result;
    if (!info.srt_info.walker_func) return result;
    auto code = std::span{reinterpret_cast<const u8*>(info.srt_info.walker_func),
                          info.srt_info.walker_func_size};
    ZydisDecodedInstruction instruction{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
    const auto next = [&] {
        if (code.empty() || !ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                instruction, operands, const_cast<u8*>(code.data()), code.size()))) return false;
        code = code.subspan(instruction.length);
        return true;
    };
    const auto reg = [&](size_t index, ZydisRegister value) {
        return operands[index].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[index].reg.value == value;
    };
    const auto memory = [&](size_t index, ZydisRegister base, u16 bits) {
        const auto& o = operands[index];
        return o.type == ZYDIS_OPERAND_TYPE_MEMORY && o.size == bits && o.mem.base == base &&
               o.mem.index == ZYDIS_REGISTER_NONE && o.mem.disp.value >= 0 &&
               u64(o.mem.disp.value) <= UINT32_MAX && (o.mem.disp.value & 3) == 0;
    };
    size_t depth = 0;
    while (next()) {
        if (instruction.mnemonic == ZYDIS_MNEMONIC_RET)
            return depth == 0 && code.empty() ? std::optional{std::move(result)} : std::nullopt;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_PUSH && reg(0, ZYDIS_REGISTER_RDI)) {
            if (depth >= 64 || !next() || instruction.mnemonic != ZYDIS_MNEMONIC_MOV ||
                !reg(0, ZYDIS_REGISTER_RDI) || !memory(1, ZYDIS_REGISTER_RDI, 64)) return {};
            const auto offset = u32(operands[1].mem.disp.value);
            if (depth == 0 && offset > (Shader::NUM_USER_DATA_REGS - 2) * sizeof(u32)) return {};
            result.push_back({Step::Kind::Push, offset});
            ++depth;
            if (!next() || instruction.mnemonic != ZYDIS_MNEMONIC_MOV ||
                !reg(0, ZYDIS_REGISTER_R10) || operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                operands[1].imm.value.u != 0xFFFFFFFFFFFFULL) return {};
            if (!next() || instruction.mnemonic != ZYDIS_MNEMONIC_AND ||
                !reg(0, ZYDIS_REGISTER_RDI) || !reg(1, ZYDIS_REGISTER_R10)) return {};
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_POP && reg(0, ZYDIS_REGISTER_RDI)) {
            if (depth == 0) return {};
            --depth;
            result.push_back({Step::Kind::Pop});
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV && reg(0, ZYDIS_REGISTER_R10D) &&
                   memory(1, ZYDIS_REGISTER_RDI, 32)) {
            if (depth == 0) return {};
            const auto source = u32(operands[1].mem.disp.value);
            if (!next() || instruction.mnemonic != ZYDIS_MNEMONIC_MOV ||
                !memory(0, ZYDIS_REGISTER_RSI, 32) || !reg(1, ZYDIS_REGISTER_R10D)) return {};
            const auto target = u32(operands[0].mem.disp.value / sizeof(u32));
            if (target < Shader::NUM_USER_DATA_REGS || target >= info.srt_info.flattened_bufsize_dw)
                return {};
            result.push_back({Step::Kind::Copy, source, target});
        } else return {};
    }
    return {};
}

bool ValidConversion(u32 format, u32 number) {
    if (number != u32(AmdGpu::NumberFormat::SnormNz)) return true;
    using D = AmdGpu::DataFormat;
    switch (D(format)) {
    case D::Format8: case D::Format8_8: case D::Format8_8_8_8:
    case D::Format16: case D::Format16_16: case D::Format16_16_16_16: return true;
    default: return false;
    }
}
bool ValidFlat(const Shader::Info& info) {
    const auto fits = [&](u32 offset, size_t words) {
        return offset <= info.flattened_ud_buf.size() &&
               words <= info.flattened_ud_buf.size() - offset;
    };
    for (const auto& b : info.buffers) {
        if (!b.inline_cbuf && !fits(b.sharp_idx, 4)) return false;
        const auto sharp = b.GetSharp(info);
        if (sharp && b.is_formatted && !ValidConversion(sharp.data_format, sharp.num_format))
            return false;
    }
    for (const auto& i : info.images) {
        if (!fits(i.sharp_idx, i.is_r128 ? 4 : 8)) return false;
        const auto sharp = i.GetSharp(info);
        if (sharp && (!ValidConversion(sharp.data_format, sharp.num_format) ||
                      sharp.last_level < sharp.base_level)) return false;
    }
    for (const auto& f : info.fmasks) if (!fits(f.sharp_idx, 8)) return false;
    for (const auto& s : info.samplers)
        if (!s.is_inline_sampler && !fits(s.sharp_idx, 4)) return false;
    return info.buffers.size() + info.images.size() + info.fmasks.size() <=
           Shader::StageSpecialization::MaxStageResources;
}
void InitializeWorker() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}
} // namespace

struct PipelinePreparation {
    std::atomic<u64> checked{}, missed{}, live_hits{}, live_misses{}, faults{}, rejected{}, queued{};
    std::atomic<u64> active{}, peak{};
    ScratchDraw::SpeculativePool pool{2, InitializeWorker};
};

PipelineCache::~PipelineCache() {
    // Jobs capture this cache. Stop and join them while every program is still alive.
    graphics_preparation_live.store(nullptr, std::memory_order_release);
    graphics_preparation.reset();
    // Bit 40: after the workers (they offer jobs), before the programs and Vulkan objects the
    // compile threads use.
    async_pipelines.reset();
    preparation.reset();
}

u64 PipelineCache::PrepVerifyInterval() {
    static const u64 interval = [] {
        const char* value = std::getenv("FAST_PREP_VERIFY");
        return value ? std::strtoull(value, nullptr, 0) : 0ULL;
    }();
    return interval;
}

std::optional<PrepProgramResult> PipelineCache::PrepareProgram(const PrepStageRequest& request, PrepWorker& worker) {
    std::shared_lock lock(programs_mutex, std::try_to_lock);
    if (!lock) return {};
    return PrepareProgramLocked(request, worker);
}

std::optional<PrepProgramResult> PipelineCache::PrepareProgramLocked(const PrepStageRequest& request, PrepWorker& worker) {
    using Shader::LogicalStage;
    // Tessellation reads a separate constant buffer; geometry carries copy-shader spans.
    // Until those inputs are owned, they take the ordinary selector's fallback.
    if (request.logical_stage == LogicalStage::TessellationControl ||
        request.logical_stage == LogicalStage::TessellationEval ||
        request.logical_stage == LogicalStage::Geometry ||
        request.user.size() > Shader::NUM_USER_DATA_REGS) return {};
    const auto it = program_cache.find(request.hash);
    if (it == program_cache.end() || !it->second->prep_template) return {};
    const auto& program = *it->second;
    const auto& prototype = *program.prep_template;
    if (prototype.stage != request.stage || prototype.l_stage != request.logical_stage) return {};
    auto [private_it, inserted] = worker.inputs.try_emplace(&prototype, prototype);
    auto& owned = private_it->second;
    auto& info = owned.info;
    owned.user = request.user;
    info.user_data = owned.user;
    info.pgm_base = request.program_base;
    // Hide any unrelated render-thread input scope even when called synchronously.
    ScratchDraw::Scope private_scope{{}};
    if (request.frozen) {
        info.flattened_ud_buf = request.frozen->flat;
    } else {
        if (info.srt_info.flattened_bufsize_dw < owned.user.size()) return {};
        info.flattened_ud_buf.resize(info.srt_info.flattened_bufsize_dw);
        if (!program.prep_walk || owned.user.size() != Shader::NUM_USER_DATA_REGS) return {};
        struct Walk {
            std::span<const ScratchDraw::SrtReadStep> plan;
            std::span<const u32> user;
            u32* flat;
        } walk{*program.prep_walk, owned.user, info.flattened_ud_buf.data()};
        if (!ReadRaw([](void* p) {
            const auto& w = *static_cast<Walk*>(p);
            std::memcpy(w.flat, w.user.data(), w.user.size_bytes());
            ScratchDraw::ExecuteSrtReadPlan(w.plan, w.user.data(), w.flat);
        }, &walk)) return {};
    }
    if (!ValidFlat(info)) return {};
    std::optional<Shader::Gcn::FetchShaderData> fetch;
    std::shared_ptr<const Shader::Gcn::FetchShaderSnapshot> fetch_source;
    if (info.has_fetch_shader) {
        if (request.frozen) {
            fetch = request.frozen->fetch;
        } else {
            const auto index = u32(info.fetch_shader_sgpr_base);
            if (index > owned.user.size() || owned.user.size() - index < 2) return {};
            const auto* code = Shader::Gcn::GetFetchShaderCode(info, index);
            for (const auto& m : program.modules) {
                if (m.spec.Valid() && m.fetch_snapshot && !m.fetch_snapshot->words.empty() &&
                    SameRaw(code, m.fetch_snapshot->words)) {
                    fetch = m.fetch_snapshot->data;
                    fetch_source = m.fetch_snapshot;
                    break;
                }
            }
        }
        // Never decode unbounded, possibly changing guest instructions on a worker.
        if (!fetch) return {};
    }
    owned.register_reads.clear();
    if (fetch) {
        if (fetch->attributes.size() > MaxVertexBufferCount) return {};
        owned.register_reads.resize(fetch->attributes.size());
        for (size_t i = 0; i < fetch->attributes.size(); ++i) {
            const auto& a = fetch->attributes[i];
            auto& read = owned.register_reads[i];
            read.pointer_index = a.sgpr_base;
            read.dword_offset = a.dword_offset;
            if (request.frozen) {
                const auto& reads = request.frozen->register_reads;
                const auto found = std::ranges::find_if(reads, [&](const auto& r) {
                    return r.pointer_index == a.sgpr_base && r.dword_offset == a.dword_offset;
                });
                if (found == reads.end()) return {};
                read.words = found->words;
            } else {
                const u32* base = owned.user.data();
                if (a.sgpr_base == u32(Shader::IR::NumScalarRegs)) {
                    if (a.dword_offset > owned.user.size() || owned.user.size() - a.dword_offset < 4)
                        return {};
                } else {
                    if (a.sgpr_base > owned.user.size() || owned.user.size() - a.sgpr_base < 2)
                        return {};
                    std::memcpy(&base, owned.user.data() + a.sgpr_base, sizeof(base));
                    base = reinterpret_cast<const u32*>(VAddr(base) & 0xFFFFFFFFFFFFULL);
                }
                if (!CopyRaw(read.words.data(), reinterpret_cast<const void*>(VAddr(base) + size_t(a.dword_offset) * sizeof(u32)), sizeof(read.words))) return {};
            }
        }
    }
    const ScratchDraw::Inputs input{&info, owned.user, info.flattened_ud_buf,
                                   info.pgm_base, owned.register_reads};
    ScratchDraw::Scope input_scope{{&input, 1}};
    // As GetProgram: a fragment permutation keeps only the colour targets its program writes.
    auto runtime = request.runtime;
    if (runtime.stage == Shader::Stage::Fragment)
        runtime.fs_info.KeepWrittenColorBuffers(prototype.mrt_mask, profile.needs_unorm_fixup);
    const Shader::StageSpecialization spec(info, runtime, profile, request.start, &fetch);
    // Match the same direction and first-permutation order as the ordinary selector.
    const auto module = std::ranges::find(program.modules, spec, &Program::Module::spec);
    if (module == program.modules.end()) return {};
    return PrepProgramResult{module->id, // the permutation's id, as GetProgram returns it
                             &program.info, &owned, module->module, module->spec.fetch_shader_data,
                             std::move(fetch_source)};
}

namespace {
bool ReadShaderHeader(u64 address, AmdGpu::BinaryInfo& result) {
    struct Read { uintptr_t address; AmdGpu::BinaryInfo* result; bool valid{}; } read{address, &result};
    return ReadRaw([](void* p) {
        auto& r = *static_cast<Read*>(p);
        r.valid = ScratchDraw::ReadShaderHeaderUnsafe(r.address, *r.result);
    }, &read) && read.valid;
}
bool ValidGraphicsRegisters(const AmdGpu::Regs& regs) {
    // Geometry/tessellation need additional owned code/constants. These states
    // retain ordinary selection until their inputs have a preparation contract.
    if (regs.stage_enable.raw != AmdGpu::ShaderStageEnable::VgtStages::Vs || regs.num_interp > 32)
        return false;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable) return true;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const auto& color = regs.color_buffers[cb];
        if (!color || !regs.color_target_mask.GetMask(cb)) continue;
        const auto format = AmdGpu::DataFormat(color.info.format);
        if (u32(format) > u32(AmdGpu::DataFormat::FormatBc7) ||
            !magic_enum::enum_contains(format) || format == AmdGpu::DataFormat::FormatInvalid ||
            !magic_enum::enum_contains(color.GetFixedNumberFormat())) return false;
        const auto components = AmdGpu::NumComponents(format);
        if (components < 1 || components > 4 ||
            !ValidConversion(u32(format), u32(color.GetFixedNumberFormat()))) return false;
    }
    return true;
}
}

namespace {
u64 GraphicsPrepVerifyInterval() {
    static const u64 interval = [] {
        const char* value = std::getenv("FAST_GRAPHICS_PREP_VERIFY");
        return value ? std::strtoull(value, nullptr, 0) : 0ULL;
    }();
    return interval;
}
bool IsDirectDraw(AmdGpu::PM4ItOpcode opcode) {
    return opcode == AmdGpu::PM4ItOpcode::DrawIndex2 ||
           opcode == AmdGpu::PM4ItOpcode::DrawIndexOffset2 ||
           opcode == AmdGpu::PM4ItOpcode::DrawIndexAuto;
}
// The only camera inputs of graphics runtime info: the asymmetric native projection and the
// light scissors (BuildRuntimeInfo), and only without VrPushData (then they are pushed per draw).
void CameraKey(const VR::CameraRecord& record, std::array<float, 10>& key) {
    const auto& l = record.eye_fov[0];
    const auto& r = record.eye_fov[1];
    key = {record.vfov,    record.aspect,   l.angle_left,  l.angle_right, l.angle_up,
           l.angle_down,   r.angle_left,    r.angle_right, r.angle_up,    r.angle_down};
}
// FAST_PREP_PRIORITY: 0 idle (default), 1 lowest, 2 below normal, 3 normal. Helpers never
// hold a lock Stage A waits for, so a higher priority only trades CPU with the game threads.
void SetIdlePriority() {
#ifdef _WIN32
    static const int priority = [] {
        const char* v = std::getenv("FAST_PREP_PRIORITY");
        switch (v ? std::atoi(v) : 0) {
        case 1: return THREAD_PRIORITY_LOWEST;
        case 2: return THREAD_PRIORITY_BELOW_NORMAL;
        case 3: return THREAD_PRIORITY_NORMAL;
        default: return THREAD_PRIORITY_IDLE;
        }
    }();
    SetThreadPriority(GetCurrentThread(), priority);
#endif
}
// FAST_PREP_WORKERS: preparation worker count, 1..6 (default 2).
u32 PrepWorkerCount() {
    static const u32 count = [] {
        const char* v = std::getenv("FAST_PREP_WORKERS");
        return v ? std::clamp<u32>(u32(std::atoi(v)), 1, 6) : 2u;
    }();
    return count;
}
// TryPreparedGraphics counters (Stage A only).
enum PrepCount : size_t {
    PrepAttempts,
    PrepHits,
    PrepAbsent,
    PrepPending,
    PrepUnavailable,
    PrepStale,
    PrepContext,
    PrepCamera,
    PrepPipeline,
    PrepInputs,
    PrepFetch,
    PrepSharps,
    PrepVerified,
    PrepMismatches,
    PrepCounts,
};
static_assert(PrepCounts <= 16, "PipelineCache::prepared_counts");
bool SameWords(const u32* a, const u32* b, size_t words) {
    return words == 0 || std::memcmp(a, b, words * sizeof(u32)) == 0;
}
using PrepDeltas = ScratchDraw::RegisterDeltaTracker<AmdGpu::Regs::NumRegs>;

// Bit 32: one worker's reusable buffers for a draw's packet preparation (no per-draw
// allocation in steady state; bbport keeps the same as thread_locals, vk_draw_prep.cpp:92-99).
struct PacketScratch {
    std::vector<std::byte> blob;
    std::vector<PreparedVertexInput> inputs;
    std::vector<VertexPreparation::Range> ranges;
    std::vector<PreparedConstant> constants;
    std::vector<PreparedRange> writers, captures;
    // Bit 58.
    std::vector<PreparedBindingImage> binding_images;
    PerfMemo::BindKey binding_key;
};
template <typename T>
std::byte* PutBytes(std::byte* out, const std::vector<T>& items) {
    if (!items.empty())
        std::memcpy(out, items.data(), items.size() * sizeof(T));
    return out + items.size() * sizeof(T);
}

// BufferCache::PrepareVertexInputs over GraphicsPipeline::GetVertexInputs, for the pipeline the
// worker selected (bbport PrepareVertexInputs, BBV vk_draw_prep.cpp:121-195): the fetch shader is
// the selected vertex permutation's, the V#s are the reads Stage A compares in
// TryPreparedGraphics and then reuses as the packet's. False leaves it to Stage A.
bool PrepareVertexBlob(const PreparedDraw& draw, const AmdGpu::Regs& regs,
                       const PrepGraphicsContext& context, PreparedPacket& packet,
                       PacketScratch& s) {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    const auto& fetch = context.selection.fetch_shader;
    const size_t count = fetch ? fetch->attributes.size() : 0;
    if (count > MaxVertexBufferCount)
        return false;
    const PrepWorker::Inputs* vs = nullptr;
    for (u32 k = 0; k < draw.num_stages; ++k)
        if (draw.stages[k].logical == static_cast<u32>(Shader::LogicalStage::Vertex))
            vs = context.inputs[draw.stages[k].logical];
    if (count && (!vs || vs->register_reads.size() != count))
        return false;
    s.inputs.clear();
    s.ranges.clear();
    u64 hash = 14695981039346656037ull;
    for (size_t i = 0; i < count; ++i) {
        const auto& attrib = fetch->attributes[i];
        const auto& read = vs->register_reads[i];
        if (read.pointer_index != attrib.sgpr_base || read.dword_offset != attrib.dword_offset)
            return false;
        // VertexAttribute::GetSharp (fetch_shader.h:40-48) over the V# ReadUdReg returns.
        AmdGpu::Buffer buffer;
        static_assert(sizeof(buffer) == sizeof(read.words));
        std::memcpy(&buffer, read.words.data(), sizeof(buffer));
        buffer.base_address += attrib.inst_offset;
        if (attrib.data_format) {
            buffer.data_format = attrib.data_format;
            buffer.num_format = attrib.num_format;
        }
        // SurfaceFormat would assert: Stage A meets it as before (BBV vk_draw_prep.cpp:141-150).
        const auto format =
            LiverpoolToVK::TrySurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        if (!format)
            return false;
        const auto step_rate = attrib.GetStepRate();
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? regs.vgt_instance_step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1
                                       ? regs.vgt_instance_step_rate_1
                                       : 1u);
        s.inputs.push_back(PreparedVertexInput{buffer, *format, divisor, u32(attrib.semantic),
                                               u32(step_rate != InstanceIdType::None)});
        // Always hashed; Stage A keeps it only when its pipeline has object motion.
        for (const auto byte : std::as_bytes(std::span(&buffer, 1)))
            hash = (hash ^ std::to_integer<u8>(byte)) * 1099511628211ull;
        if (buffer.base_address && buffer.GetSize())
            s.ranges.push_back({buffer.base_address, buffer.base_address + buffer.GetSize()});
    }
    std::ranges::sort(s.ranges, {}, &VertexPreparation::Range::base_address);
    size_t merged = 0;
    for (const auto range : s.ranges) {
        if (!merged || s.ranges[merged - 1].end_address < range.base_address)
            s.ranges[merged++] = range;
        else
            s.ranges[merged - 1].end_address =
                std::max<VAddr>(s.ranges[merged - 1].end_address, range.end_address);
    }
    s.ranges.resize(merged);
    // Appended after the header (and anything before it), 8-byte aligned throughout.
    const size_t start = s.blob.size();
    const size_t fetch_bytes = PreparedPacket::FetchBytes(static_cast<u32>(count));
    s.blob.resize(start + fetch_bytes + s.inputs.size() * sizeof(PreparedVertexInput) +
                      s.ranges.size() * sizeof(VertexPreparation::Range),
                  std::byte{});
    if (count)
        std::memcpy(s.blob.data() + start, fetch->attributes.data(),
                    count * sizeof(Shader::Gcn::VertexAttribute));
    PutBytes(PutBytes(s.blob.data() + start + fetch_bytes, s.inputs), s.ranges);
    packet.vertex = static_cast<u32>(start);
    packet.num_attributes = static_cast<u32>(count);
    packet.num_ranges = static_cast<u32>(s.ranges.size());
    packet.geometry = s.inputs.empty() ? 0 : hash;
    return true;
}

// DrawPacket::Build's per-stage resource loop (constant candidates, written buffers and the
// native captures) over the worker's private Info copies, which hold the very flat data and
// descriptors Stage A compares and binds. Written images need an ImageInfo footprint, which
// stays on Stage A, as do the tessellation constants (never prepared).
bool PrepareResourcesBlob(const PreparedDraw& draw, const PrepGraphicsContext& context,
                          bool native, PreparedPacket& packet, PacketScratch& s) {
    s.constants.clear();
    s.writers.clear();
    s.captures.clear();
    bool unknown = false;
    // Pure below 1 GB (memory.cpp ClampRangeSize returns `size` without a map lookup); every
    // call here asks for at most 16 KB, like Stage A's.
    auto* const memory = Core::Memory::Instance();
    for (u32 k = 0; k < draw.num_stages; ++k) {
        const auto* input = context.inputs[draw.stages[k].logical];
        if (!input)
            return false;
        const auto& stage = input->info;
        if (stage.tess_consts_dword_offset >= 0)
            return false;
        for (const auto& desc : stage.images) {
            if (!desc.is_written)
                continue;
            const auto sharp = desc.GetSharp(stage);
            if (!sharp.Address() || sharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid)
                continue;
            if (sharp.pitch < sharp.width || !magic_enum::enum_contains(sharp.GetDataFmt()) ||
                !magic_enum::enum_contains(sharp.GetNumberFmt())) {
                unknown = true;
                continue;
            }
            return false;
        }
        const u64 flat_bytes = u64(stage.flattened_ud_buf.size()) * sizeof(u32);
        for (const auto& desc : stage.buffers) {
            const auto sharp = desc.GetSharp(stage);
            if (desc.is_written && !desc.IsSpecial() && sharp.base_address && sharp.GetSize())
                s.writers.push_back({sharp.base_address, sharp.GetSize()});
            const bool flat = desc.buffer_type == Shader::BufferType::Flatbuf;
            const u64 bytes = flat ? flat_bytes : sharp.GetSize();
            if (bytes && bytes <= 16384 && !desc.is_written &&
                (flat || (!desc.IsSpecial() && sharp.base_address &&
                          memory->ClampRangeSize(sharp.base_address, bytes) == bytes)))
                s.constants.push_back(PreparedConstant{
                    sharp, static_cast<u32>(bytes),
                    static_cast<u16>(&desc - stage.buffers.data()), static_cast<u8>(k),
                    static_cast<u8>(flat)});
            if (desc.IsSpecial() || desc.is_written || !sharp.base_address)
                continue;
            const u64 size = sharp.GetSize();
            if (native &&
                (size == 864 || size == 208 || size == 272 || size == 336 || size == 416) &&
                memory->ClampRangeSize(sharp.base_address, size) == size)
                s.captures.push_back({sharp.base_address, size});
            else if (stage.l_stage == Shader::LogicalStage::Vertex && size == 416 &&
                     sharp.base_address >= 0x10000)
                s.captures.push_back({sharp.base_address, 12 * sizeof(float)});
        }
    }
    const size_t start = s.blob.size();
    s.blob.resize(start + (s.writers.size() + s.captures.size()) * sizeof(PreparedRange) +
                      s.constants.size() * sizeof(PreparedConstant),
                  std::byte{});
    PutBytes(PutBytes(PutBytes(s.blob.data() + start, s.constants), s.writers), s.captures);
    packet.resources = static_cast<u32>(start);
    packet.num_constants = static_cast<u32>(s.constants.size());
    packet.num_writers = static_cast<u32>(s.writers.size());
    packet.num_captures = static_cast<u32>(s.captures.size());
    packet.unknown = unknown ? 1u : 0u;
    return true;
}
} // namespace

// Bit 58: Rasterizer::PreparedResources::Build for every prepared stage, in draw.stages order
// (Stage A's stage order: PacketReuse checks it), over the worker's private Info copies.
bool PrepareBindingsBlob(const PreparedDraw& draw, const PrepGraphicsContext& context,
                         PreparedPacket& packet, PacketScratch& s) {
    const size_t start = s.blob.size();
    for (u32 k = 0; k < draw.num_stages; ++k) {
        const auto* input = context.inputs[draw.stages[k].logical];
        if (!input ||
            !AppendPreparedBindings(input->info, s.blob, s.binding_images, s.binding_key)) {
            s.blob.resize(start);
            return false;
        }
    }
    packet.bindings = static_cast<u32>(start);
    return true;
}

// One top-level graphics buffer, from its submission until Stage A, the scanner and the
// workers have all released it (BBV vk_draw_prep.h:131-145).
struct GraphicsSubmission {
    enum : u32 { ScanPending = 0, ScanDone = 1, ScanFailed = 2 };
    GraphicsPreparation* owner{};
    u64 seq{};
    std::vector<u32> commands;
    bool shares_copy{};
    // Written on the submit thread before the GFX queue lock hands the buffer to Stage A.
    bool enqueued{}, after_gap{};
    std::unique_ptr<PreparedDraw[]> draws;
    u32 num_draws{};
    u32 cursor{};                   // Stage A only
    std::atomic<u32> a_reached{};   // Stage A's draw ordinal, so workers skip what it passed
    VR::CameraRecord camera{};
    // Scanner: written once, before `scanned` (release) and before the helpers-mutex hand-over.
    std::atomic<u32> scanned{ScanPending};
    u64 scan_epoch{}, end_checksum{};
    PrepDeltas::Packet delta;
    std::shared_ptr<const AmdGpu::Regs> start_regs; // worker restart point
    // Claimed under the helpers mutex; only the claiming worker writes the arena. Chunks never
    // move, so Stage A reads prepared words while the worker keeps appending.
    bool claimed{};
    std::vector<std::unique_ptr<u32[]>> chunks;
    size_t chunk_used{}, chunk_words{};
    const u32* Append(std::span<const u32> words) {
        if (words.empty())
            return nullptr;
        if (chunks.empty() || chunk_words - chunk_used < words.size()) {
            chunk_words = std::max<size_t>(16384, words.size());
            chunks.push_back(std::make_unique_for_overwrite<u32[]>(chunk_words));
            chunk_used = 0;
        }
        u32* const result = chunks.back().get() + chunk_used;
        std::memcpy(result, words.data(), words.size_bytes());
        chunk_used += words.size();
        return result;
    }
    // Bit 32: raw bytes at an 8-byte boundary (chunks come from operator new[]).
    const u32* AppendBytes(std::span<const std::byte> bytes) {
        if (bytes.empty())
            return nullptr;
        const size_t words = (bytes.size() + sizeof(u32) - 1) / sizeof(u32);
        size_t at = (chunk_used + 1) & ~size_t{1};
        if (chunks.empty() || at > chunk_words || chunk_words - at < words) {
            chunk_words = std::max<size_t>(16384, words);
            chunks.push_back(std::make_unique_for_overwrite<u32[]>(chunk_words));
            at = 0;
        }
        u32* const result = chunks.back().get() + at;
        std::memcpy(result, bytes.data(), bytes.size());
        chunk_used = at + words;
        return result;
    }
};

// The scanner and the workers (BBV vk_draw_prep.cpp). Unlike bbport, Stage A and the submit
// thread never take a lock a helper holds: helpers run at idle priority, and Windows does not
// boost a preempted idle thread that owns a mutex. Stage A talks to them through atomics only.
class GraphicsPreparation {
public:
    explicit GraphicsPreparation(PipelineCache& cache_);
    ~GraphicsPreparation();
    GraphicsPreparation(const GraphicsPreparation&) = delete;
    GraphicsPreparation& operator=(const GraphicsPreparation&) = delete;

    void Enqueue(const std::shared_ptr<GraphicsSubmission>& submission, u64 gfx_index);
    void Begin(GraphicsSubmission& submission, const AmdGpu::Regs& regs);
    void End(GraphicsSubmission& submission, std::optional<u64> checksum);
    std::string Stats() const;

private:
    struct Snapshot {
        std::unique_ptr<AmdGpu::Regs> regs = std::make_unique<AmdGpu::Regs>();
        u64 seq{}, epoch{};
    };
    static constexpr u64 NoRebase = ~u64{0};
    static constexpr u64 MaxScanLag = 64;   // BBV vk_draw_prep.cpp:268
    static constexpr u32 SnapshotEvery = 16; // scanner restart points for the workers
    static constexpr size_t RingSize = 256;

    void ScannerLoop(std::stop_token stop);
    void WorkerLoop(std::stop_token stop, u32 index);
    void Prepare(GraphicsSubmission& target, PreparedDraw& draw, const AmdGpu::Regs& regs,
                 u64 checksum, u32 view_mask, PrepGraphicsContext& context,
                 std::vector<u32>& scratch, PacketScratch& packet_scratch);
    bool Pop(std::shared_ptr<GraphicsSubmission>& value);
    std::shared_ptr<GraphicsSubmission> Claimable() const; // helpers_mutex held
    void Prune();                                          // helpers_mutex held
    void Recycle(Snapshot* snapshot);
    void Wake() {
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_one();
    }

    PipelineCache& cache;
    // Submit thread (serialized by the GFX queue lock) to the scanner: a single-producer ring.
    std::array<std::shared_ptr<GraphicsSubmission>, RingSize> ring{};
    std::atomic<u64> ring_head{}, ring_tail{};
    u64 next_seq{}, next_gfx_index{}; // submit thread only
    std::atomic<u32> wake{};
    // Stage A to the helpers.
    std::atomic<u64> gpu_seq{};
    std::atomic<Snapshot*> mailbox{}, spare{};
    // The helpers to Stage A.
    std::atomic<u64> rebase_from{NoRebase};
    std::atomic<u64> scanned_through{}; // seq + 1 of the newest scan
    // Stage A only.
    u64 a_epoch{}, last_rebase_seq{}, rebase_gap{1};
    bool a_need_rebase{};
    u64 rebases{}, end_checks{}, end_mismatches{}, unprepared{};
    // Scanner and workers only.
    std::mutex helpers_mutex;
    std::condition_variable_any helpers_cv;
    std::deque<std::shared_ptr<GraphicsSubmission>> ready; // scanned, in seq order
    u64 ready_epoch{};
    std::atomic<u64> dropped{}, scans{}, scan_failures{}, adoptions{}, replays{}, prepared{},
        unavailable{}, restarts{}, skipped{}, failures{};
    // Bit 32: draws whose vertex inputs / resource loop the workers prepared, and draws prepared
    // with the bit on that left at least one part to Stage A.
    std::atomic<u64> packet_vertex{}, packet_resources{}, packet_partial{};
    std::atomic<u64> packet_bindings{}; // bit 58: draws with every stage's bindings prepared
    std::jthread scanner;
    std::vector<std::jthread> workers;
};

GraphicsPreparation::GraphicsPreparation(PipelineCache& cache_) : cache{cache_} {
    scanner = std::jthread([this](std::stop_token stop) { ScannerLoop(stop); });
    for (u32 i = 0; i < PrepWorkerCount(); ++i)
        workers.emplace_back([this, i](std::stop_token stop) { WorkerLoop(stop, i); });
    LOG_INFO(Render_Vulkan, "FAST graphics prep: ordered scanner and {} workers (priority env {})",
             PrepWorkerCount(), std::getenv("FAST_PREP_PRIORITY") ? std::getenv("FAST_PREP_PRIORITY") : "0");
}

GraphicsPreparation::~GraphicsPreparation() {
    scanner.request_stop();
    for (auto& worker : workers)
        worker.request_stop();
    Wake(); // after the stop request: the scanner rechecks it after every wake load
    workers.clear();
    if (scanner.joinable())
        scanner.join();
    delete mailbox.exchange(nullptr);
    delete spare.exchange(nullptr);
}

void GraphicsPreparation::Recycle(Snapshot* snapshot) {
    Snapshot* none = nullptr;
    if (snapshot && !spare.compare_exchange_strong(none, snapshot, std::memory_order_acq_rel))
        delete snapshot;
}

bool GraphicsPreparation::Pop(std::shared_ptr<GraphicsSubmission>& value) {
    const u64 tail = ring_tail.load(std::memory_order_relaxed);
    if (tail == ring_head.load(std::memory_order_acquire))
        return false;
    value = std::move(ring[tail % RingSize]);
    ring_tail.store(tail + 1, std::memory_order_release);
    return true;
}

void GraphicsPreparation::Enqueue(const std::shared_ptr<GraphicsSubmission>& submission,
                                  u64 gfx_index) {
    auto& s = *submission;
    s.seq = next_seq;
    s.after_gap = gfx_index != next_gfx_index;
    const u64 head = ring_head.load(std::memory_order_relaxed);
    if (head - ring_tail.load(std::memory_order_acquire) >= RingSize) {
        // Stage A falls back for this buffer; the next one carries `after_gap`.
        ++dropped;
        return;
    }
    s.enqueued = true;
    ring[head % RingSize] = submission;
    ring_head.store(head + 1, std::memory_order_release);
    ++next_seq;
    next_gfx_index = gfx_index + 1;
    Wake();
}

void GraphicsPreparation::Begin(GraphicsSubmission& s, const AmdGpu::Regs& regs) {
    s.cursor = 0;
    if (!s.enqueued) {
        ++unprepared;
        return;
    }
    gpu_seq.store(s.seq, std::memory_order_release);
    u64 from = rebase_from.load(std::memory_order_acquire);
    const bool scanner_wants = from != NoRebase && s.seq >= from;
    const bool lagging =
        s.seq > std::max(scanned_through.load(std::memory_order_acquire), last_rebase_seq) +
                    MaxScanLag;
    const bool baseline = a_epoch == 0;
    if (!baseline && !a_need_rebase && !s.after_gap && !scanner_wants && !lagging)
        return;
    // Each rebase is one register copy here: at most one per `rebase_gap` buffers, the gap
    // doubling while the scanner keeps diverging (deferred triggers stay pending).
    if (!baseline && s.seq < last_rebase_seq + rebase_gap)
        return;
    if (scanner_wants)
        rebase_from.compare_exchange_strong(from, NoRebase, std::memory_order_acq_rel);
    if (!baseline)
        rebase_gap = s.seq < last_rebase_seq + 4 * rebase_gap ? std::min<u64>(rebase_gap * 2, 64)
                                                              : std::max<u64>(rebase_gap / 2, 1);
    last_rebase_seq = s.seq;
    a_need_rebase = false;
    Snapshot* snapshot = spare.exchange(nullptr, std::memory_order_acq_rel);
    if (!snapshot)
        snapshot = new Snapshot;
    *snapshot->regs = regs; // BBV vk_draw_prep.cpp:266-279: the only register copy on Stage A
    snapshot->seq = s.seq;
    snapshot->epoch = ++a_epoch;
    Recycle(mailbox.exchange(snapshot, std::memory_order_acq_rel));
    ++rebases;
    Wake();
}

void GraphicsPreparation::End(GraphicsSubmission& s, std::optional<u64> checksum) {
    if (!s.enqueued || !checksum ||
        s.scanned.load(std::memory_order_acquire) != GraphicsSubmission::ScanDone ||
        s.scan_epoch != a_epoch)
        return;
    ++end_checks;
    if (s.end_checksum == *checksum)
        return;
    // Register writes the scanner never sees (compute-queue SH writes between buffers):
    // restart it from Stage A's state.
    ++end_mismatches;
    a_need_rebase = true;
}

std::string GraphicsPreparation::Stats() const {
    return fmt::format("dropped={} unprepared={} rebases={} gap={} end_checks={} "
                       "end_mismatches={} scans={} scan_failures={} adoptions={} replays={} "
                       "prepared={} unavailable={} restarts={} skipped={} failures={} "
                       "packet_vertex={} packet_resources={} packet_partial={} packet_bindings={}",
                       dropped.load(), unprepared, rebases, rebase_gap, end_checks, end_mismatches,
                       scans.load(), scan_failures.load(), adoptions.load(), replays.load(),
                       prepared.load(), unavailable.load(), restarts.load(), skipped.load(),
                       failures.load(), packet_vertex.load(), packet_resources.load(),
                       packet_partial.load(), packet_bindings.load());
}

void GraphicsPreparation::ScannerLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("shadPS4:GfxPrepScan");
    SetIdlePriority();
    auto regs = std::make_unique<AmdGpu::Regs>();
    auto deltas = std::make_unique<PrepDeltas>();
    PrepDeltas::Packet discard;
    std::deque<std::shared_ptr<GraphicsSubmission>> pending, history;
    u64 pos = 0, epoch = 0, adopted = NoRebase;
    bool valid = false;
    u32 until_snapshot = 0;
    const auto replay = [&](const GraphicsSubmission& s) {
        const std::span<const u32> commands{s.commands};
        for (size_t offset = 0; offset < commands.size();) {
            size_t words = 0;
            if (!ScratchDraw::ReplayGraphicsPacket(
                    *regs, commands.subspan(offset), words,
                    [&](size_t first, size_t count) { deltas->Mark(first, count); }))
                return false;
            offset += words;
        }
        return true;
    };
    // Unknown register state from here: wait for Stage A's copy at `seq` or later.
    const auto invalidate = [&](u64 seq) {
        valid = false;
        rebase_from.store(seq, std::memory_order_release);
    };
    for (;;) {
        const u32 seen = wake.load(std::memory_order_acquire);
        if (stop.stop_requested())
            return;
        bool progress = false;
        try {
            if (Snapshot* snapshot = mailbox.exchange(nullptr, std::memory_order_acq_rel)) {
                std::swap(regs, snapshot->regs);
                pos = adopted = snapshot->seq;
                epoch = snapshot->epoch;
                Recycle(snapshot);
                *deltas = PrepDeltas{};
                valid = true;
                until_snapshot = 0;
                ++adoptions;
                progress = true;
                // Buffers already scanned from the old state keep their (older epoch) scans;
                // replay them privately up to the first unscanned one.
                for (const auto& h : history) {
                    if (h->seq < pos)
                        continue;
                    if (h->seq != pos || !replay(*h)) {
                        invalidate(history.back()->seq + 1);
                        break;
                    }
                    ++pos;
                    ++replays;
                }
                deltas->Capture(regs->reg_array, discard); // the next delta starts here
            }
            for (std::shared_ptr<GraphicsSubmission> next; Pop(next);)
                pending.push_back(std::move(next));
            while (!pending.empty() && pending.front()->seq < pos)
                pending.pop_front();
            if (valid && !pending.empty()) {
                const auto s = pending.front();
                if (s->seq != pos || (s->after_gap && adopted != s->seq)) {
                    // A top-level buffer before it was not prepared (ring full or the bit was
                    // off): its register writes are unknown.
                    invalidate(s->seq);
                } else {
                    pending.pop_front();
                    progress = true;
                    if (until_snapshot == 0) {
                        s->start_regs = std::make_shared<AmdGpu::Regs>(*regs);
                        until_snapshot = SnapshotEvery;
                    }
                    --until_snapshot;
                    // BBV vk_draw_prep.cpp:327-379: replay, record the delta and end state.
                    const bool ok = replay(*s);
                    if (ok) {
                        deltas->Capture(regs->reg_array, s->delta);
                        s->end_checksum =
                            deltas->Checksum(regs->reg_array, ScratchDraw::HashRegisterWords);
                    }
                    s->scan_epoch = epoch;
                    s->scanned.store(ok ? GraphicsSubmission::ScanDone
                                        : GraphicsSubmission::ScanFailed,
                                     std::memory_order_release);
                    scanned_through.store(s->seq + 1, std::memory_order_release);
                    ++scans;
                    ++pos;
                    history.push_back(s);
                    if (!ok) {
                        // Nested buffers, conditional execution, unknown packets.
                        ++scan_failures;
                        invalidate(s->seq + 1);
                    }
                    {
                        std::scoped_lock lock{helpers_mutex};
                        ready.push_back(s);
                        ready_epoch = epoch;
                        Prune();
                    }
                    helpers_cv.notify_all();
                }
            }
        } catch (...) {
            ++failures;
            invalidate(pos + 1);
        }
        const u64 gpu = gpu_seq.load(std::memory_order_acquire);
        while (!history.empty() && (history.size() > 32 || history.front()->seq + 8 < gpu))
            history.pop_front();
        if (!progress)
            wake.wait(seen, std::memory_order_acquire);
    }
}

std::shared_ptr<GraphicsSubmission> GraphicsPreparation::Claimable() const {
    // The nearest scanned buffer Stage A has not passed (BBV vk_draw_prep.cpp:395-405).
    const u64 gpu = gpu_seq.load(std::memory_order_acquire);
    for (const auto& s : ready)
        if (!s->claimed && s->seq >= gpu && s->scan_epoch == ready_epoch)
            return s;
    return {};
}

void GraphicsPreparation::Prune() {
    // Keep from the newest restart point Stage A has reached: workers chain forward from it.
    const u64 gpu = gpu_seq.load(std::memory_order_acquire);
    size_t keep = 0;
    for (size_t i = 0; i < ready.size() && ready[i]->seq <= gpu; ++i)
        if (ready[i]->start_regs && ready[i]->scan_epoch == ready_epoch)
            keep = i;
    ready.erase(ready.begin(), ready.begin() + static_cast<std::ptrdiff_t>(keep));
    while (ready.size() > 512)
        ready.pop_front();
}

void GraphicsPreparation::WorkerLoop(std::stop_token stop, u32 index) {
    static constexpr const char* names[] = {"shadPS4:GfxPrep0", "shadPS4:GfxPrep1", "shadPS4:GfxPrep2",
                                             "shadPS4:GfxPrep3", "shadPS4:GfxPrep4", "shadPS4:GfxPrep5"};
    Common::SetCurrentThreadName(names[std::min<u32>(index, 5)]);
    SetIdlePriority();
    auto regs = std::make_unique<AmdGpu::Regs>();
    auto deltas = std::make_unique<PrepDeltas>(); // this worker's own register checksums
    auto context = std::make_unique<PrepGraphicsContext>();
    auto packet_scratch = std::make_unique<PacketScratch>();
    std::vector<std::shared_ptr<GraphicsSubmission>> path;
    std::vector<u32> scratch;
    const u32 view_mask = NativeStereo::Enabled() ? 3 : 0;
    constexpr size_t NoIndex = ~size_t{0};
    u64 pos = 0, epoch = 0; // `regs` holds the state at the start of buffer `pos`
    bool valid = false;
    while (!stop.stop_requested()) {
        std::shared_ptr<GraphicsSubmission> target;
        std::shared_ptr<const AmdGpu::Regs> restart;
        path.clear();
        {
            std::unique_lock lock{helpers_mutex};
            if (!helpers_cv.wait(lock, stop, [&] { return (target = Claimable()) != nullptr; }))
                return;
            target->claimed = true;
            // Reach the target's starting state (BBV vk_draw_prep.cpp:420-435): this worker's
            // own state plus the deltas in between, else the newest scanner restart point.
            const size_t t = static_cast<size_t>(
                std::find(ready.begin(), ready.end(), target) - ready.begin());
            const bool own = valid && epoch == target->scan_epoch && pos <= target->seq;
            size_t chain = NoIndex, snapshot = NoIndex;
            for (size_t i = t + 1; i-- > 0;) {
                const auto& entry = *ready[i];
                if (entry.scan_epoch != target->scan_epoch || entry.seq + (t - i) != target->seq ||
                    (i != t && entry.scanned.load(std::memory_order_relaxed) !=
                                   GraphicsSubmission::ScanDone))
                    break;
                if (own && entry.seq == pos) {
                    chain = i;
                    break;
                }
                if (entry.start_regs && snapshot == NoIndex) {
                    snapshot = i;
                    if (!own)
                        break;
                }
            }
            const size_t start = chain != NoIndex ? chain : snapshot;
            if (start == NoIndex) {
                ++skipped;
                continue;
            }
            if (chain == NoIndex)
                restart = ready[start]->start_regs;
            for (size_t i = start; i < t; ++i)
                path.push_back(ready[i]);
        }
        try {
            if (restart) {
                *regs = *restart;
                deltas->MarkAll();
                ++restarts;
            }
            for (const auto& step : path) {
                PrepDeltas::Apply(regs->reg_array, step->delta);
                for (const auto& block : step->delta)
                    deltas->Mark(size_t(block.index) * block.words.size(), block.words.size());
            }
            path.clear();
            epoch = target->scan_epoch;
            pos = target->seq;
            valid = false;
            bool ok = true;
            u32 ordinal = 0;
            const std::span<const u32> commands{target->commands};
            for (size_t offset = 0; offset < commands.size();) {
                size_t words = 0;
                if (!ScratchDraw::ReplayGraphicsPacket(
                        *regs, commands.subspan(offset), words,
                        [&](size_t first, size_t count) { deltas->Mark(first, count); })) {
                    ok = false;
                    break;
                }
                if (ordinal < target->num_draws && target->draws[ordinal].offset == offset) {
                    auto& draw = target->draws[ordinal++];
                    // Draws Stage A has reached are no longer worth preparing (BBV
                    // vk_draw_prep.cpp:444-446, per draw instead of per buffer).
                    const u64 gpu = gpu_seq.load(std::memory_order_acquire);
                    if (target->seq > gpu ||
                        (target->seq == gpu &&
                         ordinal > target->a_reached.load(std::memory_order_relaxed))) {
                        // Computed from the very registers the selection reads: a wrong
                        // starting state can only make the draw stale on Stage A.
                        const u64 checksum =
                            deltas->Checksum(regs->reg_array, ScratchDraw::HashRegisterWords);
                        Prepare(*target, draw, *regs, checksum, view_mask, *context, scratch,
                                *packet_scratch);
                    }
                }
                offset += words;
            }
            if (ok) {
                pos = target->seq + 1;
                valid = true;
            }
        } catch (...) {
            ++failures;
            valid = false;
        }
    }
}

void GraphicsPreparation::Prepare(GraphicsSubmission& target, PreparedDraw& draw,
                                  const AmdGpu::Regs& regs, u64 checksum, u32 view_mask,
                                  PrepGraphicsContext& context, std::vector<u32>& scratch,
                                  PacketScratch& packet_scratch) {
    const auto unavailable_draw = [&] {
        ++unavailable;
        draw.state.store(PreparedDraw::Unavailable, std::memory_order_release);
    };
    if (!cache.PrepareGraphicsDraw(context, regs, target.camera, view_mask))
        return unavailable_draw();
    u32 count = 0;
    for (u32 l = 0; l < MaxShaderStages; ++l) {
        if (!context.identity[l])
            continue;
        if (count == draw.stages.size() || !context.inputs[l])
            return unavailable_draw();
        const auto& input = *context.inputs[l];
        const auto& request = context.requests[l];
        auto& stage = draw.stages[count++];
        stage.info = context.identity[l];
        stage.hash = request.hash;
        stage.pgm_base = request.program_base;
        stage.hw_stage = static_cast<u32>(request.stage);
        stage.logical = l;
        stage.flat = target.Append(input.info.flattened_ud_buf);
        stage.flat_size = static_cast<u32>(input.info.flattened_ud_buf.size());
        const auto& fetch = context.fetch[l];
        stage.fetch_code = fetch ? target.Append(fetch->words) : nullptr;
        stage.fetch_words = fetch ? static_cast<u32>(fetch->words.size()) : 0;
        scratch.clear();
        for (const auto& read : input.register_reads) {
            scratch.push_back(read.pointer_index);
            scratch.push_back(read.dword_offset);
            scratch.insert(scratch.end(), read.words.begin(), read.words.end());
        }
        stage.reads = target.Append(scratch);
        stage.num_reads = static_cast<u32>(input.register_reads.size());
    }
    draw.num_stages = count;
    draw.reg_checksum = checksum;
    draw.generation = context.generation;
    draw.view_mask = context.view_mask; // `view_mask`, or FAST_CPU_VIEW_PREDICT's 0
    draw.native = view_mask != 0;
    draw.key = context.selection.graphics_key;
    draw.key_hash = std::hash<GraphicsPipelineKey>{}(draw.key);
    CameraKey(target.camera, draw.camera);
    // Bit 32 (read here only: with the bit off a worker does exactly what it did before). Stage A
    // ignores the result while the bit is off, and draws prepared before it was turned on carry
    // none (`draw.packet` starts null; each draw is prepared once).
    if (FastRender::On(PacketPrepBit)) {
        PreparedPacket packet{};
        auto& blob = packet_scratch.blob;
        blob.assign(sizeof(PreparedPacket), std::byte{});
        if (PrepareVertexBlob(draw, regs, context, packet, packet_scratch)) {
            packet.parts |= PreparedPacket::Vertex;
            ++packet_vertex;
        }
        // NativeStereo::Enabled() is the worker's view mask before FAST_CPU_VIEW_PREDICT
        // (draw.native; Stage A checks both).
        if (PrepareResourcesBlob(draw, context, draw.native, packet, packet_scratch)) {
            packet.parts |= PreparedPacket::Resources;
            ++packet_resources;
        }
        // Bit 58 (read here only, like bit 32): Stage A ignores the record while it is off.
        if (FastRender::On(WorkerBindingsBit) &&
            PrepareBindingsBlob(draw, context, packet, packet_scratch)) {
            packet.parts |= PreparedPacket::Bindings;
            ++packet_bindings;
        }
        if (packet.parts) {
            std::memcpy(blob.data(), &packet, sizeof(packet));
            draw.packet = target.AppendBytes(blob);
        }
        if ((packet.parts & (PreparedPacket::Vertex | PreparedPacket::Resources)) !=
            (PreparedPacket::Vertex | PreparedPacket::Resources))
            ++packet_partial;
    }
    ++prepared;
    draw.state.store(PreparedDraw::Ready, std::memory_order_release);
    // Bit 40: Stage A has not reached this draw; when its pipeline does not exist yet, start
    // compiling it now (lock-free hand-over, vk_pipeline_async.cpp).
    if (FastRender::On(AsyncPipelinesBit))
        cache.KickPreparedPipeline(context, draw.key_hash);
}

std::span<const u32> SharedCommandCopy(const GraphicsSubmission& submission) {
    return submission.shares_copy ? std::span<const u32>{submission.commands}
                                  : std::span<const u32>{};
}

void EnqueueGraphicsSubmission(const std::shared_ptr<GraphicsSubmission>& submission,
                               u64 gfx_index) {
    if (submission && submission->owner)
        submission->owner->Enqueue(submission, gfx_index);
}

void BeginGraphicsSubmission(GraphicsSubmission& submission, const AmdGpu::Regs& regs) {
    submission.owner->Begin(submission, regs);
}

void EndGraphicsSubmission(GraphicsSubmission& submission, std::optional<u64> checksum) {
    submission.owner->End(submission, checksum);
}

const PreparedDraw* NextPreparedDraw(GraphicsSubmission& s, size_t offset) {
    if (!s.enqueued)
        return nullptr;
    // Draw packets come in order: O(1) from the cursor; a skipped packet (CondExec) moves it
    // forward, anything earlier restarts the search.
    u32 i = s.cursor < s.num_draws && s.draws[s.cursor].offset <= offset ? s.cursor : 0;
    while (i < s.num_draws && s.draws[i].offset < offset)
        ++i;
    if (i == s.num_draws || s.draws[i].offset != offset)
        return nullptr;
    s.cursor = i + 1;
    s.a_reached.store(s.cursor, std::memory_order_relaxed);
    return &s.draws[i];
}

std::shared_ptr<GraphicsSubmission> PipelineCache::BuildGraphicsSubmission(
    std::span<const u32> dcb, std::vector<u32>& copy) {
    std::call_once(graphics_preparation_once, [this] {
        graphics_preparation = std::make_shared<GraphicsPreparation>(*this);
        graphics_preparation_live.store(graphics_preparation.get(), std::memory_order_release);
    });
    auto s = std::make_shared<GraphicsSubmission>();
    s->owner = graphics_preparation.get();
    if (!copy.empty()) {
        s->commands = std::move(copy);
        copy.clear();
        s->shares_copy = true;
    } else {
        s->commands.assign(dcb.begin(), dcb.end());
    }
    // BBV vk_draw_prep.cpp:224-236: count the direct draws, preallocate their results.
    thread_local std::vector<u32> offsets;
    offsets.clear();
    const std::span<const u32> commands{s->commands};
    for (size_t offset = 0; offset < commands.size();) {
        const auto* header = reinterpret_cast<const AmdGpu::PM4Header*>(commands.data() + offset);
        if (header->type == 2) {
            ++offset;
            continue;
        }
        if (header->type != 3)
            break;
        const size_t words = header->type3.NumWords() + 1;
        if (words < 2 || words > commands.size() - offset)
            break;
        if (IsDirectDraw(header->type3.opcode))
            offsets.push_back(static_cast<u32>(offset));
        offset += words;
    }
    s->num_draws = static_cast<u32>(offsets.size());
    s->draws = std::make_unique<PreparedDraw[]>(offsets.size());
    for (size_t i = 0; i < offsets.size(); ++i)
        s->draws[i].offset = offsets[i];
    // The newest projection; Stage A compares its own. With VrPushData the selection reads no
    // camera (the draw pushes it), so there is none to keep.
    if (NativeStereo::AsymmetricProjection() && !UsesVrPushData())
        s->camera = VR::DrawFrameRecord();
    return s;
}

bool PipelineCache::PrepareGraphicsStage(PipelineSelection& selected, Shader::Stage stage,
                                        Shader::LogicalStage logical,
                                        Shader::Backend::Bindings& binding, u32 view_mask,
                                        PrepGraphicsContext& context) {
    const auto index = static_cast<u32>(logical);
    auto& request = context.requests[index];
    if (!context.active[index] || request.stage != stage)
        return false;
    request.runtime = BuildRuntimeInfo(selected, stage, logical, view_mask);
    request.start = binding;
    auto prepared = PrepareProgramLocked(request, context.worker);
    if (!prepared)
        return false;
    auto& info = prepared->inputs->info;
    // The selection's runtime info as the owner's GetProgram leaves it (a compile thread builds
    // this draw's pipeline from it): the written colour targets only.
    if (stage == Shader::Stage::Fragment)
        selected.runtime_infos[index].fs_info.KeepWrittenColorBuffers(info.mrt_mask,
                                                                     profile.needs_unorm_fixup);
    context.inputs[index] = prepared->inputs;
    context.identity[index] = prepared->identity;
    context.fetch[index] = std::move(prepared->fetch_source);
    selected.infos[index] = &info;
    selected.modules[index] = prepared->module;
    selected.graphics_key.stage_hashes[index] = prepared->hash;
    if (prepared->fetch)
        selected.fetch_shader = std::move(prepared->fetch);
    info.AddBindings(binding);
    return true;
}

bool PipelineCache::PrepareGraphicsDraw(PrepGraphicsContext& context, const AmdGpu::Regs& regs,
                                        const VR::CameraRecord& camera, u32 view_mask) {
    if (!ValidGraphicsRegisters(regs))
        return false;
    // Lookup only, under the shared side; Stage A takes it exclusively only to publish.
    std::shared_lock lock(programs_mutex, std::try_to_lock);
    if (!lock)
        return false;
    context.generation = programs_generation.load(std::memory_order_relaxed);
    context.failed = false;
    context.active.fill(false);
    context.inputs.fill(nullptr);
    context.identity.fill(nullptr);
    for (auto& fetch : context.fetch)
        fetch.reset();
    auto& selected = context.selection;
    selected.regs = &regs;
    selected.camera = &camera;
    selected.vr_probe_mode = 0;
    selected.vr_fs_cut = 0;
    selected.vr_motion = {};
    selected.prepared_vertex_hash.reset();
    for (const auto stage : {Shader::Stage::Fragment, Shader::Stage::Vertex}) {
        const auto logical = stage == Shader::Stage::Fragment ? Shader::LogicalStage::Fragment
                                                              : Shader::LogicalStage::Vertex;
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage));
        if (!pgm || !pgm->Address<u32*>())
            continue;
        AmdGpu::BinaryInfo header{};
        if (!ReadShaderHeader(pgm->Address<u64>(), header))
            return false;
        if (stage == Shader::Stage::Vertex)
            selected.prepared_vertex_hash = header.shader_hash;
        if (!regs.stage_enable.IsStageEnabled(static_cast<u32>(stage)))
            continue;
        auto& request = context.requests[static_cast<u32>(logical)];
        request.hash = header.shader_hash;
        request.program_base = pgm->Address<u64>();
        request.stage = stage;
        request.logical_stage = logical;
        request.user.assign(pgm->user_data.begin(), pgm->user_data.end());
        context.active[static_cast<u32>(logical)] = true;
    }
    // FAST_CPU_VIEW_PREDICT: Stage A's guess (GetDrawPipeline) from this worker's registers, the
    // fragment program's written colour targets read from its immutable snapshot (prep_template;
    // a program without one cannot be prepared anyway). Stage A keeps it (TryPreparedGraphics).
    context.view_mask = view_mask;
    if (view_predict && view_mask == 3) {
        const auto fs = static_cast<u32>(Shader::LogicalStage::Fragment);
        u32 mrt_mask = 0;
        bool mrt_known = true;
        if (context.active[fs]) {
            const auto it = program_cache.find(context.requests[fs].hash);
            mrt_known = it != program_cache.end() && it->second->prep_template;
            mrt_mask = mrt_known ? it->second->prep_template->mrt_mask : 0u;
        }
        context.view_mask = PredictViewMask(regs, mrt_mask, mrt_known);
    }
    ScratchDraw::Scope inputs{{}};
    const bool selected_ok = RefreshGraphicsKey(selected, context.view_mask, &context) &&
                             !context.failed;
    if (selected_ok && context.view_mask != view_mask) {
        CountViewPredict(ViewWorker);
    }
    return selected_ok;
}

const GraphicsPipeline* PipelineCache::TryPreparedGraphics(const PreparedDraw* draw, u64 checksum,
                                                           u32 view_mask,
                                                           const VR::CameraRecord* camera,
                                                           bool* validated) {
    auto& counts = prepared_counts;
    if ((++counts[PrepAttempts] & 32767) == 0) {
        const auto* prep = graphics_preparation_live.load(std::memory_order_acquire);
        LOG_INFO(Render_Vulkan,
                 "FAST graphics prep: hits={} misses={} absent={} pending={} unavailable={} "
                 "stale={} context={} camera={} pipeline={} inputs={} fetch={} sharps={} "
                 "verified={} mismatches={} {}",
                 counts[PrepHits], counts[PrepAttempts] - 1 - counts[PrepHits], counts[PrepAbsent],
                 counts[PrepPending], counts[PrepUnavailable], counts[PrepStale],
                 counts[PrepContext], counts[PrepCamera], counts[PrepPipeline], counts[PrepInputs],
                 counts[PrepFetch], counts[PrepSharps], counts[PrepVerified],
                 counts[PrepMismatches], prep ? prep->Stats() : std::string{});
    }
    const auto reject = [&](PrepCount reason) -> const GraphicsPipeline* {
        ++counts[reason];
        return nullptr;
    };
    if (!draw)
        return reject(PrepAbsent);
    const u32 state = draw->state.load(std::memory_order_acquire);
    if (state != PreparedDraw::Ready)
        return reject(state == PreparedDraw::Pending ? PrepPending : PrepUnavailable);
    // BBV vk_pipeline_cache.cpp:454-485. Same registers (a state checksum, so this also covers
    // user data, program bases and every runtime-info register input)...
    if (draw->reg_checksum != checksum)
        return reject(PrepStale);
    // ...same programs, base selection and VR context (FAST_CPU_VIEW_PREDICT: the worker's guess
    // of 0 for a draw selected here with 3 stands for Stage A's own, GetDrawPipeline; the
    // recording reselects when it must)...
    const bool view_predicted = view_predict && view_mask == 3 && draw->view_mask == 0;
    if (draw->generation != programs_generation.load(std::memory_order_relaxed) ||
        (draw->view_mask != view_mask && !view_predicted) || selection.vr_probe_mode ||
        selection.vr_fs_cut || selection.vr_motion.enabled)
        return reject(PrepContext);
    // ...and the same camera, when the runtime info bakes it in (VrPushData: never, the draw
    // pushes its projection and light tiles)...
    if (NativeStereo::AsymmetricProjection() && !UsesVrPushData()) {
        std::array<float, 10> key;
        CameraKey(camera ? *camera : VR::DrawFrameRecord(), key);
        if (std::memcmp(key.data(), draw->camera.data(), sizeof(key)) != 0)
            return reject(PrepCamera);
    }
    const auto it = graphics_pipelines.find(draw->key, draw->key_hash);
    if (it == graphics_pipelines.end() || !it->second)
        return reject(PrepPipeline);
    const GraphicsPipeline* pipeline = it->second.get();
    const auto stages = pipeline->GetStages();
    const auto& regs = ScratchDraw::Registers(liverpool);
    for (u32 i = 0; i < draw->num_stages; ++i) {
        const auto& stage = draw->stages[i];
        if (stage.logical >= stages.size() || stages[stage.logical] != stage.info)
            return reject(PrepPipeline);
        // ...the same shader code (the header hash, as GetParams reads it on this thread)...
        const auto* pgm = regs.ProgramForStage(stage.hw_stage);
        if (!pgm || pgm->Address<u64>() != stage.pgm_base ||
            AmdGpu::SearchBinaryInfo(pgm->Address<u32*>()).shader_hash != stage.hash)
            return reject(PrepInputs);
        // ...and the same guest tables: the walk GetProgram would run, compared word for word.
        auto& info = const_cast<Shader::Info&>(*stage.info);
        if (ScratchDraw::Find(&info))
            return reject(PrepInputs);
        info.pgm_base = stage.pgm_base;
        info.user_data = pgm->user_data;
        info.RefreshFlatBuf();
        if (info.flattened_ud_buf.size() != stage.flat_size ||
            !SameWords(info.flattened_ud_buf.data(), stage.flat, stage.flat_size))
            return reject(PrepInputs);
        // Stricter than bbport: the fetch shader words and the fetched V#s the selection used.
        // DrawPacket::Build reads the same V#s on this thread right after.
        if (stage.fetch_words) {
            const u32* code =
                Shader::Gcn::GetFetchShaderCode(info, u32(info.fetch_shader_sgpr_base));
            if (!code || !SameWords(code, stage.fetch_code, stage.fetch_words))
                return reject(PrepFetch);
        }
        for (u32 r = 0; r < stage.num_reads; ++r) {
            const u32* read = stage.reads + size_t(r) * 6;
            const auto sharp = info.ReadUdReg<AmdGpu::Buffer>(read[0], read[1]);
            static_assert(sizeof(sharp) == 4 * sizeof(u32));
            if (std::memcmp(&sharp, read + 2, sizeof(sharp)) != 0)
                return reject(PrepSharps);
        }
    }
    ++counts[PrepHits];
    if (view_predicted) {
        CountViewPredict(ViewPrepared);
    }
    if (const u64 every = GraphicsPrepVerifyInterval(); every && counts[PrepHits] % every == 0) {
        // FAST_GRAPHICS_PREP_VERIFY=N: the ordinary selection from the same registers (and the
        // same view mask).
        const GraphicsPipeline* ordinary = GetGraphicsPipeline(draw->view_mask);
        ++counts[PrepVerified];
        if (ordinary != pipeline) {
            ++counts[PrepMismatches];
            LOG_ERROR(Render_Vulkan,
                      "FAST graphics prep mismatch #{}: prepared key {:#x}, ordinary key {:#x}, "
                      "checksum {:#x}",
                      counts[PrepMismatches], draw->key_hash,
                      ordinary ? std::hash<GraphicsPipelineKey>{}(ordinary->GetGraphicsKey()) : 0,
                      checksum);
            return ordinary;
        }
    }
    if (validated)
        *validated = true;
    return pipeline;
}


void PipelineCache::OnProgramSelected(Program& program, size_t perm_idx,
                                     const Shader::RuntimeInfo& runtime,
                                     const Shader::Backend::Bindings& start) {
    const auto& info = program.info;
    auto& selected = program.modules[perm_idx];
    // Called under selection_mutex. Only structural publication needs the exclusive
    // program lock; normal Info refreshes and memo hits remain independent of workers.
    // A failed snapshot lookup is recorded: it is retried every 64th selection of the
    // permutation, never with the exclusive lock per draw. The lookup itself reads this
    // thread's parse cache, so it runs outside the lock.
    std::shared_ptr<const Shader::Gcn::FetchShaderSnapshot> snapshot;
    if (selected.spec.fetch_shader_data && !selected.fetch_snapshot && info.has_fetch_shader &&
        (selected.fetch_snapshot_tries++ & 63) == 0)
        snapshot = Shader::Gcn::FindFetchShaderSnapshot(info, *selected.spec.fetch_shader_data);
    if (!program.prep_template || snapshot) {
        // Bit 31 workers hold the shared side at idle priority: never wait for them here.
        std::unique_lock lock(programs_mutex, std::try_to_lock);
        if (lock) {
            if (!program.prep_template) {
                auto prototype = std::make_unique<Shader::Info>(info);
                prototype->user_data = {};
                prototype->flattened_ud_buf.clear();
                prototype->pgm_base = 0;
                program.prep_walk = MakeReadPlan(info);
                program.prep_template = std::move(prototype);
            }
            if (snapshot)
                selected.fetch_snapshot = std::move(snapshot);
        } else if (snapshot) {
            selected.fetch_snapshot_tries = 0; // publish at the next selection
        }
    }
    if (!PrepVerifyInterval() || ++prep_sequence % PrepVerifyInterval()) return;
    if (info.l_stage == Shader::LogicalStage::TessellationControl ||
        info.l_stage == Shader::LogicalStage::TessellationEval ||
        info.l_stage == Shader::LogicalStage::Geometry) return;
    if (!preparation) preparation = std::make_shared<PipelinePreparation>();
    PrepStageRequest request;
    request.hash = info.pgm_hash;
    request.program_base = info.ProgramBase();
    request.stage = info.stage;
    request.logical_stage = info.l_stage;
    request.runtime = runtime;
    request.start = start;
    request.user.assign(info.UserData().begin(), info.UserData().end());
    auto& frozen = request.frozen.emplace();
    frozen.flat.assign(info.FlatUserData().begin(), info.FlatUserData().end());
    frozen.fetch = selected.spec.fetch_shader_data;
    if (frozen.fetch) {
        for (const auto& a : frozen.fetch->attributes) {
            ScratchDraw::RegisterRead r{a.sgpr_base, a.dword_offset};
            const auto sharp = info.ReadUdReg<AmdGpu::Buffer>(a.sgpr_base, a.dword_offset);
            std::memcpy(r.words.data(), &sharp, sizeof(sharp));
            frozen.register_reads.push_back(r);
        }
    }
    const auto expected = selected.id; // the stage hash GetProgram returns for it
    auto* state = preparation.get();
    const bool has_walk = program.prep_walk && !program.prep_walk->empty();
    if (!state->pool.TrySubmit([this, state, request = std::move(request), expected, has_walk]() mutable {
        // Each pool thread retains private vectors across jobs; the map key is an
        // immutable template, and its lifetime extends through pool shutdown.
        struct ActiveJob {
            PipelinePreparation& state;
            ~ActiveJob() { --state.active; }
        } active{*state};
        const u64 concurrency = ++state->active;
        auto peak = state->peak.load();
        while (peak < concurrency && !state->peak.compare_exchange_weak(peak, concurrency)) {}
        thread_local PrepWorker worker;
        const auto result = PrepareProgram(request, worker);
        if (result) {
            ASSERT_MSG(result->hash == expected, "Worker stage selection differs from owned draw inputs");
            ++state->checked;
        } else ++state->missed;
        auto bad = request;
        bad.user.resize(Shader::NUM_USER_DATA_REGS + 1);
        ASSERT_MSG(!PrepareProgram(bad, worker), "Worker accepted out-of-range user data");
        ++state->rejected;
        if (has_walk) {
            bad = request;
            bad.frozen.reset();
            bad.user.assign(Shader::NUM_USER_DATA_REGS, 0);
            ASSERT_MSG(!PrepareProgram(bad, worker), "Worker accepted a null SRT root");
            ++state->rejected;
        }
        request.frozen.reset();
        if (PrepareProgram(request, worker)) ++state->live_hits;
        else ++state->live_misses;
#ifdef _WIN32
        u32 scratch{};
        ASSERT_MSG(!CopyRaw(&scratch, nullptr, sizeof(scratch)), "Speculative null read did not fail");
        ++state->faults;
#endif
        const auto completed = state->checked.load() + state->missed.load();
        if ((completed & 511) == 0)
            LOG_INFO(Render_Vulkan, "FAST preparation: checked={} missed={} live_hits={} live_misses={} faults={} rejected={} peak={} failures={} mismatches=0",
                     state->checked.load(), state->missed.load(), state->live_hits.load(),
                     state->live_misses.load(), state->faults.load(), state->rejected.load(),
                     state->peak.load(), state->pool.failures.load());
    })) return;
    ++state->queued;
}
} // namespace Vulkan
