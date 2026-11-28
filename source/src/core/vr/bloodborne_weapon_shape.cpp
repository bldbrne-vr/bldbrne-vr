// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <miniz.h>
#include <cmrc/cmrc.hpp>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/file_sys/fs.h"
#include "core/vr/bloodborne_assets.h"
#include "core/vr/blade_shine_mesh.h"
#include "core/vr/chikage.h"
#include "core/vr/bloodborne_weapon_shape.h"
#include "core/vr/weapon_simple.h"
#include "core/vr/weapon_authored_data.h"
#include "core/vr/c0000_rig.h"
#include "core/vr/tpf_patch.h"

CMRC_DECLARE(res);

namespace VR::Bloodborne::WeaponShape {

namespace {

namespace F = VR::WeaponFit;
namespace WM = VR::WeaponMesh;

struct Entry {
    bool done{false};
    bool ok{false};
    F::Fit fit{};
    // Request::shine: its model (VR::Assets::PutModel's id, 0 none) and the blade's line in its
    // frame (core/vr/blade_shine_mesh.h).
    s32 shine_model{0};
    VR::DualBlades::P3 shine_base{};
    VR::DualBlades::P3 shine_tip{};
};

// The split blades' shine models: VR::Assets runtime ids of their own, far above the ones the
// Beast Claw's severed pieces count up from RuntimeModelBase (bloodborne_sever.cpp), one per blade
// form fitted in a session (a handful).
constexpr s32 ShineModelBase = VR::Assets::RuntimeModelBase + 0x3F000000;

// Request::shine: the blade's steel as posed (core/vr/blade_shine_mesh.h), as a model for the
// in-frame renderer (its shine shader reads positions and normals; no texture).
std::shared_ptr<VR::Assets::Model> ShineModel(const VR::DualBlades::ShineMesh& mesh) {
    if (mesh.indices.empty() || mesh.position.size() != mesh.normal.size()) {
        return nullptr;
    }
    auto model = std::make_shared<VR::Assets::Model>();
    model->vertices.resize(mesh.position.size());
    for (std::size_t i = 0; i < mesh.position.size(); ++i) {
        auto& v = model->vertices[i];
        for (int k = 0; k < 3; ++k) {
            v.position[k] = mesh.position[i][static_cast<std::size_t>(k)];
            v.normal[k] = mesh.normal[i][static_cast<std::size_t>(k)];
            model->min[k] = i == 0 ? v.position[k] : std::min(model->min[k], v.position[k]);
            model->max[k] = i == 0 ? v.position[k] : std::max(model->max[k], v.position[k]);
        }
    }
    model->indices = mesh.indices;
    return model;
}

// The Chikage's scabbard (wp_a_1063's second model, WP_A_1063_1) with its texture, for the
// in-frame renderer: the game hangs it on the hunter but does not draw it in first person
// (pictures of 2026-11-19: the same eye with it on the blade and not). Built once, on the worker.
constexpr s32 ChikageModel = 1063;
constexpr s32 ScabbardModelId = VR::Assets::RuntimeModelBase + 0x3E000000;
std::atomic<s32> g_scabbard_model{0};

std::optional<std::vector<u8>> ReadParts(s32 model, std::string& why, bool own = false);

void BuildScabbard() {
    // As the fitted parts are read (Fit): the weapon file's own FLVER of the part, its full-detail
    // triangles, the normals from the surface (blade_shine_mesh.h).
    // From the game's own file, never a mod's: the first
    // person mod's wp_a_1063 keeps the scabbard's corners and empties its triangles (so the game
    // draws none: found 2026-11-19).
    std::string why;
    const auto bnd = ReadParts(ChikageModel, why, true);
    const std::string name = fmt::format("wp_a_{:04}_1.flver", ChikageModel);
    const auto flver = bnd ? VR::Body::C0000::Rig::Bnd4Find(*bnd, name) : std::nullopt;
    const auto mesh = flver ? WM::ParseFlverMesh(*flver, why) : std::nullopt;
    if (!mesh) {
        LOG_WARNING(Core, "VR chikage: no scabbard model ({}: {})", name,
                    !bnd ? why : !flver ? "not in the weapon file" : why);
        return;
    }
    std::vector<VR::DualBlades::P3> corners;
    corners.reserve(mesh->verts.size());
    for (const auto& v : mesh->verts) {
        corners.push_back({v.x, v.y, v.z});
    }
    const VR::DualBlades::ShineMesh surface = VR::DualBlades::BuildShineMesh(corners, mesh->tris, {});
    auto model = ShineModel(surface);
    if (!model) {
        LOG_WARNING(Core, "VR chikage: the scabbard's model holds no triangles ({} corners, {} "
                          "indices in the file)",
                    mesh->verts.size(), mesh->tris.size());
        return;
    }
    // Project the same inlay onto both broad faces, following the curve of the saya.
    // This keeps the native vertices and seams intact. UVs are built once on this worker.
    auto& finish = model->diffuse;
    finish.width = 128;
    finish.height = 1024;
    finish.levels = 11;
    finish.format = VR::Assets::ModelTexture::Format::Bc1;
    u32 bytes = 0;
    for (u32 level = 0; level < finish.levels; ++level) {
        finish.level_offset.push_back(bytes);
        bytes += 8 * std::max(1u, (finish.width >> level) / 4) *
                     std::max(1u, (finish.height >> level) / 4);
    }
    const auto atlas = cmrc::res::get_filesystem().open("src/resources/vr/chikage_scabbard.dds");
    // A fixed, embedded BC1 mip chain. Do not upload partial or mis-sized levels.
    if (atlas.size() == 128 + bytes && std::memcmp(atlas.begin(), "DDS ", 4) == 0 &&
        std::memcmp(atlas.begin() + 84, "DXT1", 4) == 0) {
        finish.blocks.assign(atlas.begin() + 128, atlas.end());
    } else {
        LOG_WARNING(Core, "VR chikage: invalid scabbard texture; using plain lacquer");
        finish = {};
        finish.format = VR::Assets::ModelTexture::Format::Rgba8;
        finish.width = finish.height = finish.levels = 1;
        finish.level_offset = {0};
        finish.blocks = {41, 45, 53, 255};
    }
    const float span = std::max(0.001f, model->max[1] - model->min[1]);
    for (auto& vertex : model->vertices) {
        const float along = std::clamp(model->max[1] - vertex.position[1], 0.0f, span);
        const float across = vertex.position[0] - VR::Chikage::Bend(along);
        vertex.uv[0] = std::clamp(0.5f + across / 0.080f, 0.5f / 128, 1.0f - 0.5f / 128);
        vertex.uv[1] = (0.5f + 1023.0f * along / span) / 1024.0f;
    }
    LOG_INFO(Core,
             "VR chikage: the scabbard's model: {} triangles, ({:.3f} {:.3f} {:.3f}) .. ({:.3f} "
             "{:.3f} {:.3f}) m",
             model->indices.size() / 3, model->min[0], model->min[1], model->min[2], model->max[0],
             model->max[1], model->max[2]);
    VR::Assets::PutModel(ScabbardModelId, std::move(model));
    g_scabbard_model.store(ScabbardModelId, std::memory_order_release);
}

struct Worker {
    std::mutex mutex;
    std::condition_variable cv;
    bool started{false};
    bool scabbard_pending{false};
    std::unordered_map<u64, Entry> cache;
    std::deque<Request> queue;
};

// Never destroyed: the detached thread waits on it until the process ends.
Worker& W() {
    static Worker& w = *new Worker;
    return w;
}

// The weapon file's unpacked BND4 (nullopt and `why` when unreadable).
// `own`: the game's own file (its patch's, else its base's), whatever a mod puts in its place.
std::optional<std::vector<u8>> ReadParts(s32 model, std::string& why, bool own) {
    auto* const mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto relative = fmt::format("dvdroot_ps4/parts/wp_a_{:04}.partsbnd.dcx", model);
    const std::string path = "/app0/" + relative;
    const auto file = own ? mnt->ReadGameAsset(relative, 64u << 20)
                          : mnt->ReadFile(path);
    if (!file) {
        why = path + " not readable";
        return std::nullopt;
    }
    const auto dcx = TpfPatch::ParseDcx(*file);
    if (!dcx || dcx->unpacked > (64u << 20)) {
        why = path + " is not a DCX";
        return std::nullopt;
    }
    std::vector<u8> bnd(dcx->unpacked);
    mz_ulong length = dcx->unpacked;
    if (mz_uncompress(bnd.data(), &length, file->data() + dcx->stream, dcx->packed) != MZ_OK ||
        length != dcx->unpacked) {
        why = path + ": the zlib stream does not inflate";
        return std::nullopt;
    }
    return bnd;
}

// A posed part whose dummies lie further than this (rms) from the game's is not used.
constexpr float MaxRmsM = 0.01f;

// Every part's posed corners in the grip's frame, then the fit; `checked`: how well each part's
// dummies matched the game's.
bool Fit(const Request& r, F::Fit& out, std::string& why, std::string& checked,
         VR::DualBlades::ShineMesh* shine) {
    const auto bnd = ReadParts(r.model, why);
    if (!bnd) {
        return false;
    }
    const auto* authored = VR::WeaponAuthored::Find(r.model, r.form_b, r.left);
    bool used_authored = false;
    std::vector<WM::Vec3> verts, handle, blade;
    std::vector<u32> tris;
    std::vector<u8> steel; // each of `verts` on an edge bone (Request::shine)
    for (const Part& part : r.parts) {
        const std::string name = part.index == 0
                                     ? fmt::format("wp_a_{:04}.flver", r.model)
                                     : fmt::format("wp_a_{:04}_{}.flver", r.model, part.index);
        const auto flver = VR::Body::C0000::Rig::Bnd4Find(*bnd, name);
        if (!flver) {
            why = fmt::format("no {} in the weapon file", name);
            return false;
        }
        std::string mesh_why;
        const auto mesh = WM::ParseFlverMesh(*flver, mesh_why);
        if (!mesh) {
            why = fmt::format("{}: {}", name, mesh_why);
            return false;
        }
        // Where the game shows it: the posed model's own dummies on the game's (MaxRmsM), else
        // no fit (the line as before) rather than a shape off the weapon.
        const WM::Posed posed =
            WM::PoseChecked(*mesh, mesh->dummies, part.bones, part.to_grip, part.markers);
        if (posed.rms < 0.0f || posed.rms > MaxRmsM) {
            why = posed.rms < 0.0f
                      ? fmt::format("{}: none of its dummies found to check it", name)
                      : fmt::format("{} posed {:.1f} cm off the game's dummies ({} of them, {} "
                                    "bone{} laid on them)",
                                    name, posed.rms * 100.0f, posed.matched, posed.corrected,
                                    posed.corrected == 1 ? "" : "s");
            return false;
        }
        checked += fmt::format(
            " {} {:.1f} mm on {} dummies{}", name, posed.rms * 1000.0f, posed.matched,
            posed.corrected > 0 ? fmt::format(" ({} bone{} laid on them)", posed.corrected,
                                              posed.corrected == 1 ? "" : "s")
                                : std::string{});
        if (authored && part.index == authored->part) {
            if (!VR::WeaponAuthored::Apply(*authored, *mesh, posed, out)) {
                why = "authored shape has an unavailable bone or invalid endpoint";
                return false;
            }
            used_authored = true;
            checked += " authored";
        }
        // Collect structural pieces after posing. Named heads may span attached parts;
        // the original handle/edge construction applies only to a single folding part.
        const std::size_t first_steel = steel.size();
        steel.resize(first_steel + posed.verts.size(), 0);
        for (std::size_t i = 0; i < posed.verts.size(); ++i) {
            const auto b = mesh->vbone[i];
            if (b < 0 || static_cast<std::size_t>(b) >= mesh->bone_names.size())
                continue;
            const auto& bone_name = mesh->bone_names[static_cast<std::size_t>(b)];
            if (r.parts.size() == 1 && bone_name == "handle")
                handle.push_back(posed.verts[i]);
            if ((r.parts.size() == 1 && bone_name == "edge") ||
                F::RefinedBladeBone(r.model, bone_name)) {
                blade.push_back(posed.verts[i]);
                steel[first_steel + i] = 1;
            }
        }
        const u32 base = static_cast<u32>(verts.size());
        verts.insert(verts.end(), posed.verts.begin(), posed.verts.end());
        for (const u32 i : mesh->tris) {
            tris.push_back(base + i);
        }
    }
    // Request::shine: the steel as posed, whether the capsules fit or not (it needs only the
    // posed model, checked on the game's dummies above).
    if (shine != nullptr && r.shine) {
        std::vector<VR::DualBlades::P3> corners(verts.size());
        for (std::size_t i = 0; i < verts.size(); ++i) {
            corners[i] = {verts[i].x, verts[i].y, verts[i].z};
        }
        *shine = VR::DualBlades::BuildShineMesh(corners, tris, steel);
    }
    if (authored) {
        if (!used_authored) {
            why = "authored form part is not attached to the requested hand";
            return false;
        }
        F::MeasureSimple(out, verts, tris);
    } else {
        out = F::FitSimpleCapsules(verts, tris, handle, blade, r.model);
    }
    if (out.count == 0) {
        why = "no capsule fitted";
        return false;
    }
    return true;
}

void Run() {
    Common::SetCurrentThreadName("VR:WeaponShape");
    Worker& w = W();
    for (;;) {
        Request r;
        {
            std::unique_lock lock{w.mutex};
            w.cv.wait(lock, [&] { return w.scabbard_pending || !w.queue.empty(); });
            if (w.scabbard_pending) {
                w.scabbard_pending = false;
                lock.unlock();
                if (!ScabbardModel()) BuildScabbard();
                continue;
            }
            r = std::move(w.queue.front());
            w.queue.pop_front();
        }
        const auto started = std::chrono::steady_clock::now();
        Entry e{};
        std::string why, checked;
        VR::DualBlades::ShineMesh shine{};
        e.ok = Fit(r, e.fit, why, checked, &shine);
        e.done = true;
        if (auto model = ShineModel(shine)) {
            // One id per blade form fitted this session (the worker is the only one counting).
            static s32 next = 0;
            e.shine_model = ShineModelBase + next++;
            e.shine_base = shine.base;
            e.shine_tip = shine.tip;
            VR::Assets::PutModel(e.shine_model, std::move(model));
            LOG_INFO(Core,
                     "VR solid: weapon {} {} blade's shine model {:#x}: {} of its {} triangles "
                     "({}), line {:.0f} cm",
                     r.weapon, r.left ? "left" : "right", e.shine_model, shine.indices.size() / 3,
                     shine.triangles_in, shine.steel_only ? "its edge bones" : "the whole part",
                     100.0f * std::sqrt((shine.tip[0] - shine.base[0]) * (shine.tip[0] - shine.base[0]) +
                                        (shine.tip[1] - shine.base[1]) * (shine.tip[1] - shine.base[1]) +
                                        (shine.tip[2] - shine.base[2]) * (shine.tip[2] - shine.base[2])));
        } else if (r.shine) {
            LOG_WARNING(Core, "VR solid: weapon {} {} blade: no shine model ({})", r.weapon,
                        r.left ? "left" : "right", e.ok || why.empty() ? "no steel" : why);
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        if (e.ok) {
            std::string caps;
            for (int k = 0; k < e.fit.count; ++k) {
                const F::Capsule& c = e.fit.caps[static_cast<std::size_t>(k)];
                caps += fmt::format(" ({:.0f},{:.0f},{:.0f})->({:.0f},{:.0f},{:.0f}) r{:.0f}",
                                    c.a.x * 100.0f, c.a.y * 100.0f, c.a.z * 100.0f, c.b.x * 100.0f,
                                    c.b.y * 100.0f, c.b.z * 100.0f, c.r * 100.0f);
            }
            LOG_INFO(Core,
                     "VR solid: weapon {} style {} (model {:04}, {} part{}): {} capsule{} in "
                     "{:.0f} ms; it reaches at most {:.0f} cm past them, {:.0f}% of it inside, "
                     "{:.0f} cm in the air; checked:{}; cm:{}",
                     r.weapon, r.style, r.model, r.parts.size(), r.parts.size() == 1 ? "" : "s",
                     e.fit.count, e.fit.count == 1 ? "" : "s", ms, e.fit.poke * 100.0f,
                     e.fit.coverage * 100.0f, e.fit.air * 100.0f, checked, caps);
        } else {
            LOG_WARNING(Core,
                        "VR solid: weapon {} style {} (model {:04}): no fitted shape ({}); its "
                        "line as before",
                        r.weapon, r.style, r.model, why);
        }
        std::scoped_lock lock{w.mutex};
        w.cache[r.key] = e;
    }
}

void WakeWorker(Worker& w) {
    if (!w.started) {
        w.started = true;
        std::thread(Run).detach();
    }
    w.cv.notify_one();
}

} // namespace

void AskScabbard() {
    auto& w = W();
    std::scoped_lock lock{w.mutex};
    w.scabbard_pending = true;
    WakeWorker(w);
}

void Ask(Request request) {
    Worker& w = W();
    std::scoped_lock lock{w.mutex};
    if (w.cache.contains(request.key)) {
        return;
    }
    // Refused on the game thread (logged there): the line, nothing to fit.
    if (request.model < 0 || request.parts.empty()) {
        w.cache[request.key] = Entry{true, false, {}};
        return;
    }
    w.cache[request.key] = Entry{};
    w.queue.push_back(std::move(request));
    WakeWorker(w);
}

bool Asked(u64 key) {
    Worker& w = W();
    std::scoped_lock lock{w.mutex};
    return w.cache.contains(key);
}

bool Get(u64 key, F::Fit& out) {
    Worker& w = W();
    std::scoped_lock lock{w.mutex};
    const auto it = w.cache.find(key);
    if (it == w.cache.end() || !it->second.done || !it->second.ok) {
        return false;
    }
    out = it->second.fit;
    return true;
}

bool GetShine(u64 key, Shine& out) {
    Worker& w = W();
    std::scoped_lock lock{w.mutex};
    const auto it = w.cache.find(key);
    if (it == w.cache.end() || !it->second.done) {
        return false;
    }
    out.model = it->second.shine_model;
    out.base = it->second.shine_base;
    out.tip = it->second.shine_tip;
    return true;
}

s32 ScabbardModel() {
    return g_scabbard_model.load(std::memory_order_acquire);
}

} // namespace VR::Bloodborne::WeaponShape
