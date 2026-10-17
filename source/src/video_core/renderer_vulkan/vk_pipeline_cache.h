// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <type_traits>
#include <unordered_map>
#include "../../../scratchpad/dual-eye-perf/srt_read_plan.h"

#include <variant>
#include <cstdlib>
#include <tsl/robin_map.h>
#include "common/logging/log.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/fast_render.h"
#include "video_core/pipeline_cache_policy.h"
#include "video_core/renderer_vulkan/vk_compile_reason.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_perf_memo.h"
#include "video_core/renderer_vulkan/vk_pipeline_skip.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
union Regs;
struct ComputeProgram;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace VR { struct CameraRecord; }

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
        // Its permutation id (PermutationId, pipeline_cache_policy.h): the pipeline keys' stage
        // hash, its meta's and binary's name; never 0, unique in the program. The index in
        // `modules` is only where the lookups find it.
        u64 id{};
        std::shared_ptr<const Shader::Gcn::FetchShaderSnapshot> fetch_snapshot;
        u32 fetch_snapshot_tries{}; // failed snapshot lookups: retried every 64th selection
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    // perf_spec_memo (Bloodborne VR): the permutation this program's last uses resolved to, with
    // everything its specialization was built from: the stages, the binding start, the runtime
    // info's bytes, the fetch shader's words and the projection of the bound sharps
    // (vk_perf_memo.h). Equal inputs build an equal StageSpecialization, so GetProgram returns
    // the same permutation without building and comparing one. Valid while `epoch` is current.
    struct SpecMemo {
        u32 epoch{0};
        u32 perm_idx{0};
        Shader::Stage stage{};
        Shader::LogicalStage l_stage{};
        Shader::Backend::Bindings start{};
        std::array<unsigned char, sizeof(Shader::RuntimeInfo)> runtime_info{};
        const u32* fs_code{};
        std::vector<u32> fs_words;
        std::optional<Shader::Gcn::FetchShaderData> fs_data;
        std::vector<u32> proj;
    };

    Shader::Info info;
    std::unique_ptr<const Shader::Info> prep_template;
    std::optional<std::vector<ScratchDraw::SrtReadStep>> prep_walk;
    ModuleList modules{};
    static bool SpecVariantHistory() {
        static const bool enabled = std::getenv("VR_SPEC_VARIANT_HISTORY") != nullptr;
        return enabled;
    }
    // Allocated once per program; retaining more exact variants never changes eligibility.
    std::vector<SpecMemo> spec_memo = std::vector<SpecMemo>(SpecVariantHistory() ? 8 : 2);
    static bool SpecMru() {
        static const bool enabled = SpecVariantHistory() && std::getenv("VR_SPEC_MRU") != nullptr;
        return enabled;
    }
    u32 spec_memo_next{0};
    u32 spec_memo_recent{0};

    Program() = default;
    Program(Shader::Stage stage, Shader::LogicalStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec, u64 id) {
        modules.emplace_back(module, std::move(spec), id);
        PerfMemo::BumpEpoch();
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx, u64 id) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec), id};
        PerfMemo::BumpEpoch();
    }

    // A permutation of this program is named `id` (the warm-up's gaps are none).
    [[nodiscard]] bool HasId(u64 id) const {
        return std::ranges::any_of(
            modules, [id](const Module& m) { return m.spec.Valid() && m.id == id; });
    }
};

// Mutable inputs and outputs of one selection. Render-side calls retain one instance;
// preparation workers will supply independent instances and copied register inputs.
struct PipelineSelection {
    const AmdGpu::Regs* regs{};
    const AmdGpu::ComputeProgram* compute{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    u32 vr_probe_mode{};
    Shader::VrMotion vr_motion{};
    u32 vr_fs_cut{};
    // Worker discovery owns this hash; runtime construction must not rediscover
    // the light-rectangle vertex shader through unguarded guest memory.
    std::optional<u64> prepared_vertex_hash;
    // Preparation workers only: their camera (null: VR::DrawFrameRecord, as before).
    const VR::CameraRecord* camera{};
};

// An owned stage request. Runtime values are supplied by the draw selector, including
// VR projection/cut/motion state; workers never consult the renderer's mutable state.
struct PrepStageRequest {
    u64 hash{}, program_base{};
    Shader::Stage stage{};
    Shader::LogicalStage logical_stage{};
    Shader::RuntimeInfo runtime{};
    Shader::Backend::Bindings start{};
    std::vector<u32> user;
    struct Frozen {
        std::vector<u32> flat;
        std::vector<ScratchDraw::RegisterRead> register_reads;
        std::optional<Shader::Gcn::FetchShaderData> fetch;
    };
    // Used by the concurrent shadow check to compare exactly the same draw inputs.
    std::optional<Frozen> frozen;
};
struct PrepWorker {
    struct Inputs {
        Shader::Info info;
        std::vector<u32> user;
        std::vector<ScratchDraw::RegisterRead> register_reads;
        explicit Inputs(const Shader::Info& source) : info(source) {}
    };
    std::unordered_map<const Shader::Info*, Inputs> inputs;
};
struct PrepProgramResult {
    u64 hash{};
    const Shader::Info* identity{};
    PrepWorker::Inputs* inputs{}; // Private scratch, valid until this worker reuses it.
    vk::ShaderModule module{};
    std::optional<Shader::Gcn::FetchShaderData> fetch;
    std::shared_ptr<const Shader::Gcn::FetchShaderSnapshot> fetch_source;
};
// One preparation worker's reusable whole-draw selection state (steady state allocates
// nothing per draw except the StageSpecialization's fetch copy).
struct PrepGraphicsContext {
    PrepWorker worker;
    PipelineSelection selection;
    std::array<PrepStageRequest, MaxShaderStages> requests{};
    std::array<bool, MaxShaderStages> active{};
    std::array<const PrepWorker::Inputs*, MaxShaderStages> inputs{}; // worker scratch
    std::array<const Shader::Info*, MaxShaderStages> identity{};      // the programs' shared Info
    std::array<std::shared_ptr<const Shader::Gcn::FetchShaderSnapshot>, MaxShaderStages> fetch{};
    u64 generation{};
    u32 view_mask{}; // the selection's (PrepareGraphicsDraw: FAST_CPU_VIEW_PREDICT's guess)
    bool failed{};
};

// Toggle bit 31: bbport speculative preparation workers (BBV vk_draw_prep.h). An ordered
// scanner replays every queued top-level graphics buffer into register deltas; two idle
// workers reach each buffer's state from deltas and select the pipeline of every direct draw.
// Stage A accepts a prepared draw only after cheap validation (TryPreparedGraphics).
static constexpr auto GraphicsPrepBit = static_cast<FastRender::Bit>(31);

// Toggle bit 40: bbport async graphics pipeline creation (BB_ASYNC_PIPELINES, BBV
// vk_pipeline_cache.cpp:370-418, 540-598, 626-693), vk_pipeline_async.cpp. Only the driver's
// vkCreateGraphicsPipelines runs on compile threads; the selection owner installs the result.
// Jobs come from the bit-31 workers' lookahead (a prepared draw whose pipeline does not exist
// yet), from the VR body-cutaway prebuilds and the VR variants that never wait, and from
// bbport's skip-the-draw rule (native stereo by default, with its VR vetoes; flat with
// FAST_ASYNC_PIPELINE_SKIP=1). Otherwise a draw whose pipeline is still compiling waits for it.
// The bit in the start mask also turns the disk pipeline cache on (FAST_ASYNC_PIPELINE_CACHE=0:
// as configured) and makes a driver rejection during its warm-up prune the entry instead of
// asserting (vk_pipeline_serialization.cpp).
static constexpr auto AsyncPipelinesBit = static_cast<FastRender::Bit>(40);

struct PreparedStage {
    const Shader::Info* info{}; // the program's shared Info: identity of the pipeline stage
    u64 hash{};                 // shader header hash
    u64 pgm_base{};
    u32 hw_stage{}, logical{};
    // Worker-owned copies in the submission's arena, all compared on Stage A.
    const u32* flat{};
    const u32* fetch_code{}; // the fetch shader words the selected permutation was built from
    const u32* reads{};      // fetch V#s: {pointer_index, dword_offset, 4 words} each
    u32 flat_size{}, fetch_words{}, num_reads{};
};

// Toggle bit 32: the pure part of DrawPacket::Build moves to the bit 31 workers too (bbport
// PrepareResources/PrepareVertexInputs, BBV vk_draw_prep.cpp:91-195, carried with the draw as at
// BBV vk_rasterizer.cpp:1191). Stage A takes it only for a validated bit 31 hit that it records
// with that very pipeline, after checking what the results read beyond the hit's own checks
// (Rasterizer::DrawPacket::PacketReuse); anything else is computed on Stage A as before.
static constexpr auto PacketPrepBit = static_cast<FastRender::Bit>(32);
// Bit 58: the workers also prepare each stage's resource sharps, texture hashes and bind key
// (vk_prepared_bindings.h); Stage A hands them to Stage B by view (DrawPacket::ReuseBindings).
static constexpr auto WorkerBindingsBit = static_cast<FastRender::Bit>(58);

// One fetch attribute's dynamic vertex input (GraphicsPipeline::GetVertexInputs for the
// VertexInputBindingDescription2EXT form BufferCache::PrepareVertexInputs builds).
struct PreparedVertexInput {
    AmdGpu::Buffer buffer; // the V# after the attribute's inst_offset and format override
    vk::Format format;
    u32 divisor;
    u32 semantic;
    u32 instance; // input rate: per instance
};
// A read-only buffer binding DrawPacket::Build lists for the constant ring.
struct PreparedConstant {
    AmdGpu::Buffer sharp;
    u32 bytes;
    u16 binding;
    u8 input; // the prepared stage index, equal to the packet's stage index on reuse
    u8 flat;
};
struct PreparedRange {
    u64 address, bytes;
};
static_assert(sizeof(PreparedVertexInput) == 32 && sizeof(PreparedConstant) == 24 &&
              sizeof(PreparedRange) == 16);
static_assert(std::is_trivially_copyable_v<Shader::Gcn::VertexAttribute> &&
              std::has_unique_object_representations_v<Shader::Gcn::VertexAttribute>);
// One draw's packet preparation: this header, then its parts, in the submission's arena
// (8-byte aligned; Stage A copies everything out with memcpy). Offsets count bytes from the
// header and are valid only for the parts `parts` names.
struct PreparedPacket {
    enum : u32 { Vertex = 1, Resources = 2, Bindings = 4 };
    u64 geometry{}; // the stream hash PrepareVertexInputs keeps for a motion pipeline
    u32 parts{};
    // Vertex: the fetch attributes the worker used (Stage A compares them bytewise with its
    // pipeline's), padded to 8 bytes, then PreparedVertexInput[num_attributes] and
    // VertexPreparation::Range[num_ranges] (sorted and merged).
    u32 vertex{};
    // Resources: PreparedConstant[num_constants], then PreparedRange[num_writers] (written
    // buffers) and PreparedRange[num_captures] (DrawPacket::CaptureMemory, in call order).
    u32 resources{};
    u32 num_attributes{}, num_ranges{};
    u32 num_constants{}, num_writers{}, num_captures{};
    u32 unknown{}; // a written image of unknown footprint (DrawPacket::constant_unknown)
    // Bit 58 Bindings: one PreparedBindingStage record per prepared stage, in stage order
    // (vk_prepared_bindings.h).
    u32 bindings{};
    static constexpr size_t FetchBytes(u32 count) {
        return (size_t(count) * sizeof(Shader::Gcn::VertexAttribute) + 7) & ~size_t{7};
    }
};
static_assert(sizeof(PreparedPacket) == 48 && std::is_trivially_copyable_v<PreparedPacket>);
struct PreparedDraw {
    enum : u32 { Pending = 0, Ready = 1, Unavailable = 2 };
    std::atomic<u32> state{Pending}; // release-published by the claiming worker
    u32 offset{};                    // packet dword offset in the buffer (immutable)
    // The selection's view mask: 3 in native stereo, or FAST_CPU_VIEW_PREDICT's 0.
    u32 view_mask{}, num_stages{};
    u64 reg_checksum{}, generation{}, key_hash{};
    std::array<float, 10> camera{}; // vfov, aspect and both eye FOVs (asymmetric projection)
    bool native{}; // the worker's NativeStereo::Enabled() (the packet's native captures)
    GraphicsPipelineKey key{};
    std::array<PreparedStage, 2> stages{};
    const u32* packet{}; // bit 32: a PreparedPacket in the arena (null: none), before `state`
};
struct GraphicsSubmission;
// The DCB copy `BuildGraphicsSubmission` took over from the malformed-packet recovery
// (empty when it made its own).
std::span<const u32> SharedCommandCopy(const GraphicsSubmission& submission);
// Submit thread, under the GFX queue lock: hands the buffer to the scanner in queue order.
// `gfx_index` counts every top-level GFX submission (a gap means one was not prepared).
void EnqueueGraphicsSubmission(const std::shared_ptr<GraphicsSubmission>& submission, u64 gfx_index);
// Stage A: brackets the buffer; copies the registers only when the scanner needs a rebase.
void BeginGraphicsSubmission(GraphicsSubmission& submission, const AmdGpu::Regs& regs);
void EndGraphicsSubmission(GraphicsSubmission& submission, std::optional<u64> checksum);
// Stage A: the prepared state of the direct draw packet at `offset` (dwords), or null.
const PreparedDraw* NextPreparedDraw(GraphicsSubmission& submission, size_t offset);
class GraphicsPreparation;
struct PipelinePreparation;

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    // The warm-up's loaders: false = not preloaded, `load_reject` says why (the pipeline is then
    // compiled on first use, as without a cache).
    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage, u64 expected_id);

    std::unique_lock<std::recursive_mutex> LockSelection() {
        return std::unique_lock{selection_mutex};
    }
    // `may_skip` (bit 40 with skip-the-draw on: native stereo, or FAST_ASYNC_PIPELINE_SKIP=1):
    // the caller, Stage A's direct draw, accepts null for a pass drawn every frame whose pipeline
    // is still compiling.
    const GraphicsPipeline* GetGraphicsPipeline(u32 view_mask, bool may_skip = false) {
        return SelectGraphicsPipeline(view_mask, may_skip ? AsyncWait::MaySkip : AsyncWait::Wait);
    }
    // Stage A's draws, under the selection lock: GetGraphicsPipeline, with FAST_CPU_VIEW_PREDICT
    // (native stereo, `view_mask` 3) the mask PredictViewMask gives this draw's registers and its
    // fragment program (3 while that program was never translated and a colour target is bound).
    const GraphicsPipeline* GetDrawPipeline(u32 view_mask, bool may_skip = false);
    // FAST_CPU_VIEW_PREDICT (vk_pipeline_skip.h PredictedViewMask): the view mask the recording
    // will want for the draw `regs` describe, its fragment program writing `mrt_mask`
    // (`mrt_known` false: a program not translated yet). Stage A or a preparation worker.
    u32 PredictViewMask(const AmdGpu::Regs& regs, u32 mrt_mask, bool mrt_known) const;
    bool ViewPredict() const {
        return view_predict;
    }
    // FAST_CPU_VIEW_PREDICT's counts ('FAST_CPU_VIEW_PREDICT:' line, Rasterizer::VrViewPredicted).
    enum ViewPredictCounter : u32 {
        ViewSelected, // Stage A selections with the predicted 0 (GetDrawPipeline)
        ViewPrepared, // Stage A prepared hits with a worker's predicted 0 (TryPreparedGraphics)
        ViewWorker,   // draws a worker prepared with its predicted 0
        ViewBuilt,    // pipelines Stage A compiled or queued with it: multiview twins not built
        ViewQueued,   // ... and the workers offered to the compile threads
        NumViewPredictCounts,
    };
    u64 ViewPredictCount(ViewPredictCounter count) const {
        return (ViewPredictWorker(count) ? view_predict_workers : view_predict_owner)
            .n[count]
            .load(std::memory_order_relaxed);
    }
    // Counts one (logs only: nothing without them). Stage A's counters are written only under
    // the selection lock (GetDrawPipeline, TryPreparedGraphics, the owner's ViewBuilt), so a
    // relaxed load and store, no locked add; the workers' take a relaxed add.
    void CountViewPredict(ViewPredictCounter count) {
        if constexpr (Common::Log::Enabled) {
            if (ViewPredictWorker(count)) {
                view_predict_workers.n[count].fetch_add(1, std::memory_order_relaxed);
            } else {
                auto& n = view_predict_owner.n[count];
                n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            }
        }
    }

    // Bit 40, Stage A per direct draw under the selection lock: starts the compile threads,
    // keeps the skip rule's render-target streaks and prints the 'FAST async_pipelines' line.
    void NoteAsyncDraw() {
        if (FastRender::On(AsyncPipelinesBit))
            NoteAsyncDrawSlow();
    }
    // Bit 40, Stage A: the body-cutaway level `cut` of `base` selected from this draw's
    // registers, its pipeline compiled in the background (GetVrCutPipeline installs it).
    // `ahead`: before the cutaway asks for it; nothing at all when it cannot go to the background.
    void PrebuildVrCutPipeline(const GraphicsPipeline* base, u32 cut, bool ahead = false);

    // Bit 40's skip rule in native stereo (vk_pipeline_skip.h): the rasterizer's VR vetoes for a
    // draw the rule would leave out (Stage A, the would-skip path only; the vertex program may be
    // null): not an immersive frame, the UI phase, the hunter's own draw.
    void SetVrSkipVeto(std::function<PipelineSkip::Veto(const Shader::Info* vs)> veto) {
        vr_skip_veto = std::move(veto);
    }
    // Skip-the-draw with its VR vetoes is on (native stereo, the bit on).
    bool VrSkipVetoes() const {
        return vr_skip_vetoes && FastRender::On(AsyncPipelinesBit);
    }
    // Native stereo, the recording thread, per draw: the view mask its targets are drawn with
    // (`desired`; `targets`: their PipelineSkip::AddTarget key); the skip rule leaves out only
    // draws into eye pairs, and FAST_CPU_VIEW_PREDICT selects the next ones with it. `memo`:
    // FAST_CPU_PASS_MEMO's (TargetViews::Note); false only when its verify found a slot the memo
    // would have skipped not holding the entry.
    bool NoteTargetViews(u64 targets, u32 view_mask,
                         PipelineSkip::TargetViews::NoteMemo memo = {}) {
        if (skip_target_views && (view_predict || FastRender::On(AsyncPipelinesBit)))
            return skip_target_views->Note(targets, view_mask, memo);
        return true;
    }
    // Bit 40, bit-31 preparation workers: offers the pipeline of a prepared draw that has none
    // yet to the compile threads. Lock-free: never blocks Stage A behind an idle-priority worker.
    void KickPreparedPipeline(const PrepGraphicsContext& context, u64 key_hash);

    // Lookup only: no compilation, Vulkan calls, shared Info writes, or memo updates.
    // Results are owned candidates; a consumer must revalidate the draw and module generation.
    std::optional<PrepProgramResult> PrepareProgram(const PrepStageRequest& request, PrepWorker& worker);

    // Bit 31. Submit thread: copies the DCB (taking over `copy`, the malformed-packet copy,
    // when it has one) and starts the scanner and workers on first use.
    std::shared_ptr<GraphicsSubmission> BuildGraphicsSubmission(std::span<const u32> dcb,
                                                                std::vector<u32>& copy);
    // Preparation workers: lookup-only selection of the draw `regs` describe, into `context`.
    bool PrepareGraphicsDraw(PrepGraphicsContext& context, const AmdGpu::Regs& regs,
                             const VR::CameraRecord& camera, u32 view_mask);
    // Stage A, under the selection lock: the prepared pipeline when every input it was selected
    // from still holds (bbport TryPreparedPipeline, BBV vk_pipeline_cache.cpp:454-485), else null.
    // Refreshes the stages' shared Info exactly as GetProgram does. `validated` (optional) is set
    // only when the result is the prepared pipeline itself (bit 32 may then reuse `draw`).
    const GraphicsPipeline* TryPreparedGraphics(const PreparedDraw* draw, u64 checksum, u32 view_mask,
                                                const VR::CameraRecord* camera,
                                                bool* validated = nullptr);


    // VR body cutaway: the permutation of the current draw's pipeline (`base`, just returned by
    // GetGraphicsPipeline for the same registers) whose fragment shader cuts the fragments
    // nearer than `cut` (FragmentRuntimeInfo::vr_cut). Remembered per (base, cut); nullptr when
    // the draw has no fragment shader or it writes SampleMask itself. VrPushData: one variant
    // per base for every cut (the draw pushes its radii, VrCutVariant). `wait` false (bit 40):
    // also nullptr while its pipeline compiles in the background (not remembered: later draws
    // ask its job's state alone, vr_cut_pending, then install it), so the draw goes uncut
    // meanwhile instead of waiting.
    const GraphicsPipeline* GetVrCutPipeline(const GraphicsPipeline* base, u32 cut,
                                             bool wait = true);

    // Native stereo with Shader::VrPushData (Profile::SupportsVrPushData): every graphics
    // pipeline layout pushes VrPushData::End bytes and each draw pushes its projection, light
    // tiles, cut radii and motion buffers, which the shader permutations no longer bake in.
    bool UsesVrPushData() const;
    // The vr_cut value a cut variant is built and kept with: the cut itself, or with VrPushData
    // only its flag (one variant serves every radius).
    u32 VrCutVariant(u32 cut) const {
        return cut != 0 && UsesVrPushData() ? 1u : cut;
    }
    // The vr_motion a motion variant is built and kept with: the motion buffers' addresses too,
    // or with VrPushData only its flag (one variant serves every allocation; the draw pushes
    // them), so it is cached on disk and found again after the buffers are allocated anew.
    Shader::VrMotion VrMotionVariant(const Shader::VrMotion& motion) const {
        return motion.Variant(UsesVrPushData());
    }

    const GraphicsPipeline* GetForegroundProbePipeline(const GraphicsPipeline* base, u32 mode);
    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(PipelineSelection& selection, Shader::Stage stage, Shader::LogicalStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding,
                      std::optional<Shader::Gcn::FetchShaderData>* spare_fetch = nullptr,
                      u32 view_mask = 0);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::Stage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool RefreshGraphicsKey(PipelineSelection& selection, u32 view_mask, PrepGraphicsContext* prep = nullptr);
    bool RefreshGraphicsStages(PipelineSelection& selection, u32 view_mask, PrepGraphicsContext* prep = nullptr);
    bool RefreshComputeKey(PipelineSelection& selection);

    void DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::Stage stage, size_t perm_idx,
                                                   std::string_view ext);
    // `existing` and `wanted` (a new permutation: the program's permutations so far and the one
    // wanted) and `reason_hash` (its permutation id; for a new program, whose id needs the
    // translation, HashCombine(program, 0), the key the warm-up notes a program with a stale
    // permutation under) only tell the log line why it compiles (vk_compile_reason.h; null: a new
    // program). `spv` receives the SPIR-V: the caller writes it to the disk cache under the
    // permutation's id.
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   u64 reason_hash, Shader::Backend::Bindings& binding,
                                   std::vector<u32>& spv,
                                   const Program::ModuleList* existing = nullptr,
                                   const Shader::StageSpecialization* wanted = nullptr);
    const Shader::RuntimeInfo& BuildRuntimeInfo(PipelineSelection& selection, Shader::Stage stage, Shader::LogicalStage l_stage,
                                                u32 view_mask);

    // perf_spec_memo: the permutation of `program` remembered for these inputs, or -1. `proj`
    // receives the projection of the bound sharps (with the remembered fetch shader's attributes
    // when its words are unchanged; `proj_valid` says whether it can be stored as is).
    int FindSpecMemo(PipelineSelection& selection, Program& program, Shader::Stage stage, Shader::LogicalStage l_stage,
                     const Shader::Backend::Bindings& start, PerfMemo::Projection& proj,
                     bool& proj_valid);
    void StoreSpecMemo(PipelineSelection& selection, Program& program, Shader::Stage stage, Shader::LogicalStage l_stage,
                       const Shader::Backend::Bindings& start,
                       const std::optional<Shader::Gcn::FetchShaderData>& fs_data,
                       PerfMemo::Projection& proj, bool proj_valid, u32 perm_idx);

public:
    // Render-owner only. Call after selecting the base draw with the final view mask.
    // Returns null when the native G-buffer draw cannot carry object motion. `motion`: the live
    // buffers (VrObjectMotion::Specialization); the variant is kept per base with
    // VrMotionVariant(motion). With VrPushData its pipeline is an ordinary cached one: a session
    // finds the one the warm-up preloaded (same permutations, same key) instead of compiling it.
    // `wait` false (bit 40): also null while it compiles in the background, `deferred` then set
    // (not remembered: later draws ask its job's state alone, vr_motion_pending, then install
    // it).
    const GraphicsPipeline* GetVrMotionPipeline(const GraphicsPipeline* base,
                                                const Shader::VrMotion& motion, bool wait = true,
                                                bool* deferred = nullptr);

    // perf_spec_memo statistics and shadow-check results (the CP thread; read and reset by the
    // rasterizer's 5 s 'VR perf check:' line).
    PerfMemo::Counters spec_counters{};

private:

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    void OnProgramSelected(Program& program, size_t perm_idx,
                           const Shader::RuntimeInfo& runtime,
                           const Shader::Backend::Bindings& start);
    static u64 PrepVerifyInterval();
    std::optional<PrepProgramResult> PrepareProgramLocked(const PrepStageRequest& request, PrepWorker& worker);
    bool PrepareGraphicsStage(PipelineSelection& selection, Shader::Stage stage, Shader::LogicalStage logical,
                              Shader::Backend::Bindings& binding, u32 view_mask, PrepGraphicsContext& prep);

    // Bit 40 (vk_pipeline_async.cpp). The owner is whoever holds selection_mutex.
    struct AsyncPipelines;
    // What a selection does while its new pipeline compiles in the background.
    enum class AsyncWait : u8 {
        Wait,    // waits for it (or compiles it here): never null for that
        MaySkip, // Stage A's direct draw: null when the skip rule leaves the draw out
        Never,   // a VR variant: null until it is ready (the base pipeline meanwhile)
    };
    // A VR variant's pipeline still compiling (AsyncWait::Never): its job's state, the job kept
    // alive by it, so a later draw asks that alone instead of selecting the variant again.
    using AsyncInFlight = std::shared_ptr<const std::atomic<u32>>;
    // GetGraphicsPipeline; `in_flight` (optional) is set when AsyncWait::Never returned null for
    // a pipeline still compiling.
    const GraphicsPipeline* SelectGraphicsPipeline(u32 view_mask, AsyncWait wait,
                                                   AsyncInFlight* in_flight = nullptr);
    // The job is still compiling (false when it is done, or for null).
    static bool AsyncPending(const AsyncInFlight& job);
    // A draw went on without its VR variant, still compiling (cut_deferred / motion_deferred).
    void NoteVrVariantDeferred(bool motion);
    void InitAsyncPipelines();
    void NoteAsyncDrawSlow();
    // Owner: a finished or pending job for the selected key; true when it decided the result
    // (`out`: the installed pipeline, or null to skip or defer the draw), false to compile here.
    bool AsyncGraphicsPipeline(PipelineSelection& selection, AsyncWait wait,
                               const GraphicsPipeline*& out, AsyncInFlight* in_flight);
    // Owner, after compiling a graphics pipeline here: `start_ns` is 0 while the bit is off;
    // `reason`: why it compiled, for the stall time by reason (vk_compile_reason.h).
    void NoteSyncGraphicsPipeline(const GraphicsPipelineKey& key, u64 hash, u64 start_ns,
                                  const CompileReason::Tags& reason);
    static u64 AsyncNowNs();
    // ReplaceShader: waits for running compiles and drops every pending job.
    void DrainAsyncPipelines();
    // Shared with the compile threads; reset in ~PipelineCache after the preparation workers.
    std::shared_ptr<AsyncPipelines> async_pipelines;
    // Skip-the-draw's VR vetoes: native stereo with the rule on (set once, InitAsyncPipelines),
    // the rasterizer's part of them (SetVrSkipVeto; Stage A) and the recording's view mask per
    // set of targets (NoteTargetViews; the mono veto, and FAST_CPU_VIEW_PREDICT's guess).
    bool vr_skip_vetoes{};
    std::function<PipelineSkip::Veto(const Shader::Info* vs)> vr_skip_veto;
    std::unique_ptr<PipelineSkip::TargetViews> skip_target_views;
    // FAST_CPU_VIEW_PREDICT: native stereo with the switch on (set once, InitAsyncPipelines; the
    // table above then exists); the owner, under the selection lock, while GetDrawPipeline
    // selects with its predicted 0 (ViewBuilt); the counts (Stage A, the workers).
    bool view_predict{};
    bool view_predicting{};
    // The counts (CountViewPredict, logs only), each writer's on cache lines of their own, away
    // from the members above that Stage A and the workers read per draw: Stage A's (ViewSelected,
    // ViewPrepared, ViewBuilt) and the preparation workers' (ViewWorker, ViewQueued).
    static constexpr bool ViewPredictWorker(ViewPredictCounter count) {
        return count == ViewWorker || count == ViewQueued;
    }
    struct alignas(64) ViewPredictCounts {
        std::array<std::atomic<u64>, NumViewPredictCounts> n{};
    };
    ViewPredictCounts view_predict_owner{};
    ViewPredictCounts view_predict_workers{};
    // The owner: why the selection being decided stalls, if it does (a PipelineSkip::Veto, the
    // age cap's wait, or None: not a candidate), for the stall statistics.
    u32 async_stall_reason{};
    // Why each shader and pipeline compiled: the log lines' 'why:' and the 'FAST compile
    // reasons:' counters (vk_compile_reason.h; logs only, its own lock).
    CompileReason::Tracker compile_reasons;

    std::recursive_mutex selection_mutex;
    std::shared_mutex programs_mutex;
    std::shared_ptr<PipelinePreparation> preparation;
    u64 prep_sequence{};
    // Bit 31: created once by the submit thread; Stage A reads the pointer for statistics.
    std::once_flag graphics_preparation_once;
    std::shared_ptr<GraphicsPreparation> graphics_preparation; // shared_ptr: incomplete here
    std::atomic<GraphicsPreparation*> graphics_preparation_live{};
    // Stage A only (TryPreparedGraphics): uses and rejections by reason.
    std::array<u64, 16> prepared_counts{};
    // Written under the exclusive programs_mutex; replaced module handles expire results.
    std::atomic<u64> programs_generation{1};
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    PipelineSelection selection{};
    u64 selection_sequence{}, selection_verified{};
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start
    struct VrMotionEntry {
        Shader::VrMotion motion{};
        const GraphicsPipeline* variant{};
    };
    tsl::robin_map<const GraphicsPipeline*, VrMotionEntry> vr_motion_pipelines;
    // Bit 40: motion variants still compiling (GetVrMotionPipeline with `wait` false).
    struct VrMotionPending {
        Shader::VrMotion motion{};
        AsyncInFlight job;
    };
    tsl::robin_map<const GraphicsPipeline*, VrMotionPending> vr_motion_pending;
    PipelineCachePolicy::Reject load_reject{}; // why the last Load*Pipeline returned false
    bool load_other_mode{}; // a stage of the pipeline being loaded is the other mode's
    bool load_motion_idle{}; // ... an object-motion variant's, and the session has no upscaler
    // The loaded stages' own runtime infos (LoadPipelineStage), for the pipeline created from
    // them: the loaders' own, so the render selection starts the session untouched.
    std::array<Shader::RuntimeInfo, MaxShaderStages> load_runtime_infos{};
    // The warm-up: the loaders queue each pipeline here (its stages loaded, a placeholder in the
    // map) and the pipelines are created on worker threads before the warm-up returns.
    struct PreloadJob {
        bool is_compute{};
        GraphicsPipelineKey graphics_key{};
        ComputePipelineKey compute_key{};
        std::array<const Shader::Info*, MaxShaderStages> infos{};
        // The loaded permutations' own (GraphicsPipeline reads its motion flag from them).
        std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
        GraphicsPipeline::SerializationSupport graphics_sdata{};
        ComputePipeline::SerializationSupport compute_sdata{};
        std::unique_ptr<GraphicsPipeline> graphics{};
        std::unique_ptr<ComputePipeline> compute{};
    };
    std::vector<PreloadJob>* preload_jobs{};
    struct VrCutKey {
        const GraphicsPipeline* base;
        u32 cut;
        bool operator==(const VrCutKey&) const = default;
    };
    struct VrCutHash {
        size_t operator()(const VrCutKey& k) const noexcept {
            return std::hash<const void*>{}(k.base) ^ (size_t(k.cut) * 0x9E3779B97F4A7C15ull);
        }
    };
    tsl::robin_map<VrCutKey, const GraphicsPipeline*, VrCutHash> vr_cut_pipelines;
    tsl::robin_map<const GraphicsPipeline*, u32> vr_cut_values;
    // Bit 40: cut levels already prebuilt in the background (PrebuildVrCutPipeline), and cut
    // variants still compiling (GetVrCutPipeline with `wait` false).
    tsl::robin_map<VrCutKey, bool, VrCutHash> vr_cut_prebuilt;
    tsl::robin_map<VrCutKey, AsyncInFlight, VrCutHash> vr_cut_pending;

    tsl::robin_map<VrCutKey, const GraphicsPipeline*, VrCutHash> probe_pipelines;

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

} // namespace Vulkan
