// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// ASUS GPU Tweak III's OSD is a Vulkan layer every Vulkan program loads (VK_LAYER_ASUS_GTIII_OSD,
// an implicit layer registered under HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers). It hooks the
// desktop window's present to draw its counters. With an old GPU Tweak III (OSD 3.0.7.2, May
// 2025) an RTX 4080 player crashed within seconds of every start (2026-11-10): the desktop
// present thread, inside the OSD's hook, was working in the NVIDIA driver on the same queue
// context as the sparse binds on their own queue (3) at that moment, and the driver's state for
// that queue broke (an access violation in vkQueueBindSparse; on 0.9.4, device lost). The OSD of
// GPU Tweak III 2.2.1.1 (3.0.9.1) draws on the present queue only and did not crash here, but in
// VR it only ever counted the desktop mirror's 30 presents a second, never the headset's frames.
// So the layer stays out of this process, whatever its version, two ways the Vulkan loader reads
// when it looks for layers (so this runs before the Vulkan instance):
// - the layer's own switch: each variable of its manifest's disable_environment (the manifests of
//   the registered layers whose path has "GTIII"; ASUS_GTIII_OSD_VK_DISABLE=1 in 3.0.9.1, also
//   set when no manifest is read);
// - the loader's layer filter (VK_LOADER_LAYERS_DISABLE, loader 1.3.234+): those manifests' layer
//   names and any layer whose name has "GTIII", after any filter the player set.
// GPU Tweak III's other OSD libraries may still be injected; they add no Vulkan work (checked with
// the OSD on: one submit per desktop present, as without GPU Tweak III).

#include "video_core/renderer_vulkan/vr_overlay_guard.h"

#ifdef _WIN32

#include <windows.h>

#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/string_util.h"

namespace Vulkan {

namespace {

// The registered implicit layer manifests of GPU Tweak III's OSD (paths with "GTIII").
std::vector<std::wstring> FindGtiiiManifests() {
    std::vector<std::wstring> found;
    for (const HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        HKEY key{};
        if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", 0, KEY_READ, &key) !=
            ERROR_SUCCESS) {
            continue;
        }
        wchar_t name[2048];
        for (DWORD i = 0;; ++i) {
            DWORD length = static_cast<DWORD>(std::size(name));
            if (RegEnumValueW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS) {
                break;
            }
            std::wstring upper(name, length);
            for (wchar_t& c : upper) {
                c = static_cast<wchar_t>(std::towupper(c));
            }
            if (upper.find(L"GTIII") != std::wstring::npos) {
                found.emplace_back(name, length);
            }
        }
        RegCloseKey(key);
    }
    return found;
}

struct ManifestLayer {
    std::string name;
    std::vector<std::pair<std::string, std::string>> disable; // disable_environment
};

// The layers a manifest declares ("layer" or "layers"); none when it cannot be read.
std::vector<ManifestLayer> ReadManifest(const std::wstring& path) {
    std::vector<ManifestLayer> layers;
    std::ifstream file{std::filesystem::path{path}};
    if (!file) {
        return layers;
    }
    const nlohmann::json json = nlohmann::json::parse(file, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return layers;
    }
    const auto add = [&layers](const nlohmann::json& layer) {
        if (!layer.is_object()) {
            return;
        }
        ManifestLayer out;
        if (const auto name = layer.find("name"); name != layer.end() && name->is_string()) {
            out.name = name->get<std::string>();
        }
        if (const auto env = layer.find("disable_environment");
            env != layer.end() && env->is_object()) {
            for (const auto& [variable, value] : env->items()) {
                if (value.is_string()) {
                    out.disable.emplace_back(variable, value.get<std::string>());
                }
            }
        }
        layers.push_back(std::move(out));
    };
    if (const auto layer = json.find("layer"); layer != json.end()) {
        add(*layer);
    }
    if (const auto list = json.find("layers"); list != json.end() && list->is_array()) {
        for (const auto& layer : *list) {
            add(layer);
        }
    }
    return layers;
}

void SetVariable(const std::string& name, const std::string& value) {
    SetEnvironmentVariableW(Common::UTF8ToUTF16W(name).c_str(),
                            Common::UTF8ToUTF16W(value).c_str());
}

void KeepOut() {
    const std::vector<std::wstring> manifests = FindGtiiiManifests();
    if (const char* allow = std::getenv("VR_ALLOW_GTIII_OSD"); allow && allow[0] == '1') {
        if (!manifests.empty()) {
            LOG_INFO(Render_Vulkan,
                     "Overlays: GPU Tweak III's OSD layer let in (VR_ALLOW_GTIII_OSD=1)");
        }
        return;
    }
    std::string names;    // for the filter, comma-separated
    std::string switches; // for the log
    for (const std::wstring& manifest : manifests) {
        for (const ManifestLayer& layer : ReadManifest(manifest)) {
            if (!layer.name.empty()) {
                names += layer.name + ',';
            }
            for (const auto& [variable, value] : layer.disable) {
                SetVariable(variable, value);
                switches += (switches.empty() ? "" : ", ") + variable + '=' + value;
            }
        }
    }
    SetEnvironmentVariableW(L"ASUS_GTIII_OSD_VK_DISABLE", L"1");
    // The loader's filter, after any the player set (a comma-separated list of name globs).
    std::wstring filter;
    wchar_t current[4096];
    const DWORD length = GetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", current,
                                                 static_cast<DWORD>(std::size(current)));
    if (length > 0 && length < std::size(current)) {
        filter.assign(current, length);
        filter += L',';
    }
    filter += Common::UTF8ToUTF16W(names) + L"*GTIII*";
    SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE", filter.c_str());
    for (const std::wstring& manifest : manifests) {
        LOG_INFO(Render_Vulkan,
                 "Overlays: GPU Tweak III's OSD layer kept out of the game ({}; layer filter {}, "
                 "its switches {}): in VR it only counted the desktop mirror, and an old one "
                 "crashed the sparse binds (2026-11-10)",
                 Common::UTF16ToUTF8(manifest), Common::UTF16ToUTF8(filter),
                 switches.empty() ? "unread" : switches);
    }
}

} // namespace

// Once per process, before the first Vulkan instance: DLSS's early one at startup (emulator.cpp,
// 2026-11-11: it loaded the layer before CreateInstance set these, on every DLSS start since
// 0.9.5's guard), and CreateInstance's.
void KeepOverlayLayersOut() {
    static const bool once = (KeepOut(), true);
    (void)once;
}

} // namespace Vulkan

#else

namespace Vulkan {
void KeepOverlayLayersOut() {}
} // namespace Vulkan

#endif
