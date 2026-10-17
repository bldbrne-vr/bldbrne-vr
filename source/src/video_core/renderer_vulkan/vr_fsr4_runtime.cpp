// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vr_fsr4_runtime.h"
#ifdef _WIN32
#include <windows.h>
#endif
namespace Vulkan {
VrFsr4Runtime::~VrFsr4Runtime() {
    // The recording owner must Shutdown with real GPU completion before destruction.
    session.reset();
#ifdef _WIN32
    if (module)
        FreeLibrary(static_cast<HMODULE>(module));
#endif
}
bool VrFsr4Runtime::Load() {
    if (attempted)
        return api != nullptr;
    attempted = true;
#ifdef _WIN32
    std::filesystem::path directory;
    if (const char* p = std::getenv("VR_FSR4_DIR"); p && *p)
        directory = std::filesystem::absolute(p);
    else {
        std::wstring path(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (!n || n >= path.size()) {
            problem = "Cannot locate FSR runtime directory";
            return false;
        }
        path.resize(n);
        directory = std::filesystem::path(path).parent_path() / "fsr4";
    }
    assets = directory / "fsr4_shaders";
    std::error_code error;
    if (!std::filesystem::is_regular_file(directory / "vr_fsr4_stereo.dll", error) ||
        !std::filesystem::is_regular_file(assets / "LICENSE-FSR4-v07.txt", error)) {
        problem = "FSR bridge/assets missing (fsr4 folder or VR_FSR4_DIR)";
        return false;
    }
    module = LoadLibraryExW((directory / "vr_fsr4_stereo.dll").c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto get = module ? reinterpret_cast<VrFsr4GetApiFn>(GetProcAddress(
                                  static_cast<HMODULE>(module), "VrFsr4GetStereoApi"))
                            : nullptr;
    const auto* candidate = get ? get() : nullptr;
    // ABI 1 bridges (before provider flags) keep working; they never get the optimized post.
    if (!candidate || (candidate->abi != VR_FSR4_ABI && candidate->abi != VR_FSR4_ABI_V1) ||
        !candidate->Create || !candidate->Evaluate || !candidate->Completed ||
        !candidate->Release || !candidate->Problem) {
        problem = "FSR stereo bridge ABI mismatch/incomplete API";
        return false;
    }
    api = candidate;
    // ABI 2 bridges accept the optimized post; newer ones list further flags in an export.
    uint32_t bridge_flags = api->abi >= 2 ? VR_FSR4_FLAG_OPTIMIZED_POST : 0u;
    if (const auto get_flags = reinterpret_cast<VrFsr4GetFlagsFn>(
            GetProcAddress(static_cast<HMODULE>(module), "VrFsr4GetStereoFlags"));
        get_flags && api->abi >= 2)
        bridge_flags |= get_flags();
    // FAST bit 45: the lockstep pair entry is an optional export. A bridge without it (or with
    // another pair ABI) keeps the per-eye Evaluate.
    const auto get_pair = reinterpret_cast<VrFsr4GetPairApiFn>(
        GetProcAddress(static_cast<HMODULE>(module), VR_FSR4_PAIR_EXPORT));
    const auto* pair = get_pair ? get_pair() : nullptr;
    session = std::make_unique<VrFsr4Session>(
        api, bridge_flags,
        pair && pair->abi == VR_FSR4_PAIR_ABI && pair->EvaluatePair ? pair : nullptr);
    // FAST bit 37 uses bbport's post passes only when their manifest was packaged, and the
    // fenced SPD auto-exposure pass (packaged with that manifest) only when the bridge takes
    // its flag. The bridge loads either only for a context whose flags request it.
    optimized_post = api->abi >= 2 &&
                     std::filesystem::is_regular_file(assets / "opt" / "post-lds.json", error);
    fenced_spd = optimized_post && (bridge_flags & VR_FSR4_FLAG_FENCED_SPD) != 0 &&
                 std::filesystem::is_regular_file(assets / "opt" / "spd_auto_exposure.spv", error);
    LOG_INFO(Render_Vulkan, "FSR4 runtime: bridge flags {:#x}, optimized post {}, fenced SPD {}",
             bridge_flags, optimized_post ? "installed" : "absent",
             fenced_spd ? "installed" : "absent");
    return true;
#else
    problem = "FSR stereo runtime loader is currently implemented for Windows only";
    return false;
#endif
}
VrFsr4Session* VrFsr4Runtime::Activate(const VR::Graphics::Config& accepted,
                                       const Instance& instance) {
    if (accepted.upscaler != VR::Graphics::Upscaler::Fsr4) {
        if (session)
            session->Disable();
        return nullptr;
    }
    if (!instance.IsVrFsr4Supported()) {
        problem = "FSR 4 required device features are unavailable";
        return nullptr;
    }
    if (!Load())
        return nullptr;
    if (session->Activate(accepted, true) != VrFsr4Session::Result::Ready) {
        problem = session->Problem();
        return nullptr;
    }
    problem.clear();
    return session.get();
}
bool VrFsr4Runtime::Shutdown(uint64_t completed) {
    if (!session)
        return true;
    session->Disable();
    session->Completed(completed);
    return session->Retire();
}
} // namespace Vulkan
