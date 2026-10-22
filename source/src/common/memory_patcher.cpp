// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <codecvt>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <set>
#include "core/vr/graphics_resolution.h"
#include "core/vr/near_code.h"
#include "core/vr/vr_session.h"
#include "video_core/native_stereo/native_stereo.h"
#include <string>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_state.h"
#include "core/file_format/psf.h"
#include "memory_patcher.h"

namespace MemoryPatcher {

EXPORT uintptr_t g_eboot_address;
uint64_t g_eboot_image_size;
std::string g_game_serial;
std::string patch_file;
bool patches_applied = false;
std::vector<patchInfo> pending_patches;

std::string toHex(u64 value, size_t byteSize) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(byteSize * 2) << value;
    return ss.str();
}

static bool isHexSym(const std::string& s) {
    return (s.size() >= 2 && (s[0] == '$' || s[0] == '#'));
}

static bool isHex0x(const std::string& s) {
    return (s.size() >= 3 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
}

// possible prefixes == (s == `0x` || s == `#` || s == `$`) will be interpet as hex value, else is
// decimal
static int convertNumBase(const std::string& s) {
    return (isHex0x(s) || isHexSym(s)) ? 16 : 10;
}

std::string convertValueToHex(const std::string type, const std::string valueStr) {
    std::string result;

    if (type == "byte") {
        const u32 value = std::stoul(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 1);
    } else if (type == "bytes16") {
        const u32 value = std::stoul(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 2);
    } else if (type == "bytes32") {
        const u32 value = std::stoul(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 4);
    } else if (type == "bytes64") {
        const u64 value = std::stoull(valueStr, nullptr, convertNumBase(valueStr));
        result = toHex(value, 8);
    } else if (type == "float32") {
        union {
            float f;
            uint32_t i;
        } floatUnion;
        floatUnion.f = std::stof(valueStr);
        result = toHex(std::byteswap(floatUnion.i), sizeof(floatUnion.i));
    } else if (type == "float64") {
        union {
            double d;
            uint64_t i;
        } doubleUnion;
        doubleUnion.d = std::stod(valueStr);
        result = toHex(std::byteswap(doubleUnion.i), sizeof(doubleUnion.i));
    } else if (type == "utf8") {
        std::vector<unsigned char> byteArray =
            std::vector<unsigned char>(valueStr.begin(), valueStr.end());
        byteArray.push_back('\0');
        std::stringstream ss;
        for (unsigned char c : byteArray) {
            ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);
        }
        result = ss.str();
    } else if (type == "utf16") {
        std::wstring wide_str(valueStr.size(), L'\0');
        std::mbstowcs(&wide_str[0], valueStr.c_str(), valueStr.size());
        wide_str.resize(std::wcslen(wide_str.c_str()));

        std::u16string valueStringU16;

        for (wchar_t wc : wide_str) {
            if (wc <= 0xFFFF) {
                valueStringU16.push_back(static_cast<char16_t>(wc));
            } else {
                wc -= 0x10000;
                valueStringU16.push_back(static_cast<char16_t>(0xD800 | (wc >> 10)));
                valueStringU16.push_back(static_cast<char16_t>(0xDC00 | (wc & 0x3FF)));
            }
        }

        std::vector<unsigned char> byteArray;
        // convert to little endian
        for (char16_t ch : valueStringU16) {
            unsigned char low_byte = static_cast<unsigned char>(ch & 0x00FF);
            unsigned char high_byte = static_cast<unsigned char>((ch >> 8) & 0x00FF);

            byteArray.push_back(low_byte);
            byteArray.push_back(high_byte);
        }
        byteArray.push_back('\0');
        byteArray.push_back('\0');
        std::stringstream ss;

        for (unsigned char ch : byteArray) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
        result = ss.str();
    } else if (type == "bytes") {
        result = valueStr;
    } else if (type == "mask" || type == "mask_jump32") {
        result = valueStr;
    } else {
        LOG_INFO(Loader, "Error applying Patch, unknown type: {}", type);
    }
    return result;
}

void ApplyPendingPatches();

// FAST bit 51 (S1, graphics_settings.h): a boot-time switch, never flipped while the game runs
// (the patch below is applied before the game's first instruction). FAST_TOGGLES is read here
// because FastRender::Start() runs later, with the renderer.
static bool WholeFrameRequested() {
    constexpr unsigned WholeFrameBit = 51;
    if (const char* env = std::getenv("FAST_S1"); env && (env[0] == '1' || env[0] == '0'))
        return env[0] == '1';
    if (const char* env = std::getenv("FAST_TOGGLES")) {
        char* end{};
        const unsigned long long mask = std::strtoull(env, &end, 0);
        if (end != env && ((mask >> WholeFrameBit) & 1)) return true;
    }
    return VR::Graphics::whole_frame_requested.load(std::memory_order_relaxed);
}

// The supplied template's own output size (VR_NATIVE_FIXED_RESOLUTION, S1 only): the width and
// height immediates of its mov eax at 0x02196A6B and 0x02196A7A.
static VR::Graphics::Extent TemplateResolution(pugi::xml_document& doc, std::string_view version) {
    uint32_t width{}, height{};
    for (auto metadata : doc.child("Patch").children("Metadata")) {
        const std::string_view name = metadata.attribute("Name").value();
        if (std::string_view(metadata.attribute("Title").value()) != "Bloodborne" ||
            std::string_view(metadata.attribute("AppVer").value()) != version ||
            !metadata.attribute("isEnabled").as_bool() ||
            !(name.starts_with("Resolution Patch ") && name.ends_with(" VR"))) continue;
        for (auto line : metadata.child("PatchList").children("Line")) {
            const std::string_view address = line.attribute("Address").value();
            const std::string_view value = line.attribute("Value").value();
            if (std::string_view(line.attribute("Type").value()) != "bytes32" ||
                !value.starts_with("0x")) continue;
            uint32_t v{};
            const auto [end, error] = std::from_chars(value.data() + 2,
                value.data() + value.size(), v, 16);
            if (error != std::errc{} || end != value.data() + value.size() || (v & 0xFF) != 0xB8)
                continue;
            if (address == "0x02196A6B") width = v >> 8;
            else if (address == "0x02196A7A") height = v >> 8;
        }
    }
    return {width, height};
}

// Adapt only the complete, enabled Kyo-derived VR allocation template in memory.
// The XML on disk is never modified. Guest code is patched before its first instruction.
// APP_VER 01.00 or 01.09: the same 1.09 executable (the launcher checks it and gives the launch
// copy of the patch file the game's own APP_VER).
static void ApplyHeadsetResolution(pugi::xml_document& doc, std::string_view version) {
    const bool dibr = VR::IsEnabled() && !std::getenv("VR_NATIVE_STEREO");
    // The isolated DLSS experiment supplies its own input-resolution patch and output size.
    const char* experiment = std::getenv("VR_DIBR_DLSS");
    if (dibr && experiment && std::string_view(experiment) == "1") return;
    if ((!NativeStereo::AsymmetricProjection() && !dibr) ||
        (version != "01.00" && version != "01.09"))
        return;
    const auto config = VR::Graphics::Accepted();
    const bool fixed = !dibr && std::getenv("VR_NATIVE_FIXED_RESOLUTION") != nullptr;
    // S1: the whole template at the upscaler's input size (DLSS 5: its neural working size).
    const bool s1 = !dibr && config.upscaler != VR::Graphics::Upscaler::Off && WholeFrameRequested();
    if (fixed && !s1) return;
    const auto output = fixed ? TemplateResolution(doc, version)
                              : VR::Graphics::OutputResolution(
                                    VR::Graphics::Headset(0), VR::Graphics::Headset(1),
                                    config.scale_percent, VR::Graphics::HeadsetLimit());
    if (!output.width || !output.height) return;
    const auto target = s1 ? VR::Graphics::WholeFrameResolution(output, config) : output;
    std::vector<std::pair<pugi::xml_node, std::string>> edits;
    std::set<uint32_t> addresses;
    bool invalid = false;
    for (auto metadata : doc.child("Patch").children("Metadata")) {
        const std::string_view name = metadata.attribute("Name").value();
        if (std::string_view(metadata.attribute("Title").value()) != "Bloodborne" ||
            std::string_view(metadata.attribute("AppVer").value()) != version ||
            !metadata.attribute("isEnabled").as_bool() ||
            !((name.starts_with("Resolution Patch ") && name.ends_with(" VR")) ||
              name.starts_with("VR Light Grid "))) continue;
        for (auto line : metadata.child("PatchList").children("Line")) {
            const std::string address = line.attribute("Address").value();
            if (!address.starts_with("0x")) { invalid = true; continue; }
            uint32_t offset{};
            const auto [end, error] = std::from_chars(address.data() + 2,
                address.data() + address.size(), offset, 16);
            if (error != std::errc{} || end != address.data() + address.size()) {
                invalid = true; continue;
            }
            const auto value = VR::Graphics::ResolutionPatchValue(offset,
                line.attribute("Value").value(), target);
            if (!value) continue;
            const std::string_view type = line.attribute("Type").value();
            if (type != (value->starts_with("0x") ? "bytes32" : "bytes") ||
                !addresses.insert(offset).second) { invalid = true; continue; }
            edits.emplace_back(line, *value);
        }
    }
    if (invalid || edits.size() != 20) {
        LOG_WARNING(Loader, "VR resolution: complete audited VR resolution/light-grid template "
                    "not found ({} of 20 values); keeping the supplied patch", edits.size());
        return;
    }
    for (auto& [line, value] : edits) line.attribute("Value").set_value(value.c_str());
    if (s1) {
        VR::Graphics::SetS1(output, config);
        LOG_BENCHMARK_INFO(Loader, "VR resolution: S1 (FAST bit 51) whole frame {}x{} per eye, "
                 "reconstructed by {} preset {} (scale {:.2f}) to {}x{} after the composite; "
                 "provider OFF or scale changes require restart",
                 target.width, target.height,
                 VR::Graphics::Neural(config)                     ? "DLSS 5"
                 : VR::Graphics::UpscalerName(config.upscaler),
                 int(VR::Graphics::EffectiveProfile(config)),
                 VR::Graphics::WholeFrameRatio(config), output.width, output.height);
        if (fixed) return;
    }
    VR::Graphics::SetAppliedResolution(output, config.scale_percent);
    const auto left = VR::Graphics::Headset(0), right = VR::Graphics::Headset(1);
    LOG_BENCHMARK_INFO(Loader, "VR resolution: headset L {}x{} R {}x{}, scale {:+}%, scene/post/output "
             "{}x{} (8-pixel alignment, {}); scale changes require restart",
             left.width, left.height, right.width, right.height, config.scale_percent,
             target.width, target.height, dibr ? "DIBR center render" : "shared multiview extent");
}

// The Model LOD patch's mov ecx, imm32 (b9 <bias> 90) in the game's code, once it is applied.
constexpr uintptr_t LodPatchAddress = 0x0216fc09;
static bool g_lod_patch_enabled = false;

static u8* LodInstruction() {
    if (!g_lod_patch_enabled || g_eboot_address == 0) {
        return nullptr;
    }
    auto* const p = reinterpret_cast<u8*>(g_eboot_address + (LodPatchAddress - 0x400000));
    // The patch's own shape (mov ecx, imm32; nop), or nothing is touched.
    return p[0] == 0xB9 && p[5] == 0x90 ? p : nullptr;
}

// Live changes (2026-10-17 review: rewriting code other threads may be executing is
// cross-modifying code, not defined without synchronizing them): once every startup patch is in,
// before the game's first instruction, the patch becomes mov ecx, [rip + slot] (8B 0D disp32, the
// same 6 bytes), the slot four bytes of our own near memory (core/vr/near_code.h: mapped
// read / write within rel32 reach of the eboot, a 64-byte line of its own). A change in game is
// then an aligned 4-byte data store, read whole by every thread. No near memory: the immediate
// stays and a change applies at the next start.
static s32* g_lod_slot = nullptr;

static const char* LodName(int bias) {
    return bias == 0 ? "Performance" : bias == -1 ? "Balanced" : "Quality";
}

static void SetUpLodSlot(int bias) {
    if (g_lod_slot != nullptr) {
        return; // once
    }
    u8* const p = LodInstruction();
    if (p == nullptr || g_eboot_image_size == 0) {
        return;
    }
    u8* const slot = VR::Hook::NearAlloc(g_eboot_address, g_eboot_image_size, 64, 64);
    s32 disp32{};
    if (slot == nullptr ||
        !VR::Hook::Rel32(reinterpret_cast<u64>(p + 6), reinterpret_cast<u64>(slot), disp32)) {
        LOG_WARNING(Loader, "VR distant detail: no near memory for a live slot; changes take a "
                            "restart");
        return;
    }
    __atomic_store_n(reinterpret_cast<s32*>(slot), static_cast<s32>(bias), __ATOMIC_SEQ_CST);
    p[0] = 0x8B;
    p[1] = 0x0D; // mov ecx, [rip + disp32]
    std::memcpy(p + 2, &disp32, sizeof(disp32));
    g_lod_slot = reinterpret_cast<s32*>(slot);
    LOG_INFO(Loader, "VR distant detail: LOD bias {} ({}), live slot at {:#x}", bias,
             LodName(bias), reinterpret_cast<uintptr_t>(slot));
}

bool LodLiveAvailable() {
    return g_lod_slot != nullptr;
}

bool SetLodBiasLive(int bias) {
    s32* const slot = g_lod_slot;
    if (slot == nullptr) {
        return false;
    }
    __atomic_store_n(slot, static_cast<s32>(bias), __ATOMIC_SEQ_CST);
    LOG_INFO(Loader, "VR distant detail: LOD bias {} ({}), applied live", bias, LodName(bias));
    return true;
}

// vr.ini lod_bias (the VR page's Display > Distant detail): the Model LOD patch's bias, its
// mov ecx, imm32 at 0x0216fc09 ("b9 <bias> 90"), set in memory; the XML on disk is not modified.
// SetUpLodSlot then has it read the bias from a slot (a change in game: SetLodBiasLive).
static void ApplyLodBias(pugi::xml_document& doc) {
    const int bias = VR::GetSettings().lod_bias;
    for (auto metadata : doc.child("Patch").children("Metadata")) {
        if (!std::string_view(metadata.attribute("Name").value()).starts_with("Model LOD") ||
            !metadata.attribute("isEnabled").as_bool())
            continue;
        for (auto line : metadata.child("PatchList").children("Line")) {
            if (std::string_view(line.attribute("Address").value()) != "0x0216fc09")
                continue;
            const auto le = static_cast<u32>(bias);
            const std::string value =
                fmt::format("b9{:02X}{:02X}{:02X}{:02X}90", le & 0xFF, (le >> 8) & 0xFF,
                            (le >> 16) & 0xFF, (le >> 24) & 0xFF);
            line.attribute("Value").set_value(value.c_str());
            g_lod_patch_enabled = true; // (SetLodBiasLive checks the bytes it finds)
            LOG_INFO(Loader, "VR distant detail: LOD bias {} ({})", bias,
                     bias == 0 ? "Performance" : bias == -1 ? "Balanced" : "Quality");
        }
    }
}

void ApplyPatchesFromXML(std::filesystem::path path) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(path.c_str());

    auto* param_sfo = Common::Singleton<PSF>::Instance();
    auto app_version = param_sfo->GetString("APP_VER").value_or("Unknown version");

    if (result) {
        ApplyHeadsetResolution(doc, app_version);
        ApplyLodBias(doc);
        auto patchXML = doc.child("Patch");
        for (pugi::xml_node_iterator it = patchXML.children().begin();
             it != patchXML.children().end(); ++it) {

            if (std::string(it->name()) == "Metadata") {
                if (std::string(it->attribute("isEnabled").value()) == "true") {
                    std::string currentPatchName = it->attribute("Name").value();
                    std::string metadataAppVer = it->attribute("AppVer").value();
                    bool versionMatches = metadataAppVer == app_version;

                    auto patchList = it->first_child();
                    for (pugi::xml_node_iterator patchLineIt = patchList.children().begin();
                         patchLineIt != patchList.children().end(); ++patchLineIt) {

                        std::string type = patchLineIt->attribute("Type").value();
                        if (!versionMatches && type != "mask" && type != "mask_jump32")
                            continue;

                        std::string address = patchLineIt->attribute("Address").value();
                        std::string patchValue = patchLineIt->attribute("Value").value();
                        std::string maskOffsetStr = patchLineIt->attribute("Offset").value();
                        std::string targetStr = "";
                        std::string sizeStr = "";
                        if (type == "mask_jump32") {
                            targetStr = patchLineIt->attribute("Target").value();
                            sizeStr = patchLineIt->attribute("Size").value();
                        } else {
                            try {
                                patchValue = convertValueToHex(type, patchValue);
                            } catch (std::exception& e) {
                                ASSERT_MSG(false,
                                           "Failed to parse patch value \"{}\" for \"{}\" in "
                                           "patch \"{}\", error: \"{}\"\n"
                                           "If the patch was working on earlier versions, then it "
                                           "was using a format that shadPS4 handled incorrectly, "
                                           "and the patch should instead be fixed.",
                                           patchValue, address, currentPatchName, e.what());
                            }
                        }

                        bool littleEndian = false;
                        if (type == "bytes16" || type == "bytes32" || type == "bytes64") {
                            littleEndian = true;
                        }

                        MemoryPatcher::PatchMask patchMask = MemoryPatcher::PatchMask::None;
                        int maskOffsetValue = 0;

                        if (type == "mask")
                            patchMask = MemoryPatcher::PatchMask::Mask;

                        if (type == "mask_jump32")
                            patchMask = MemoryPatcher::PatchMask::Mask_Jump32;

                        if ((type == "mask" || type == "mask_jump32") && !maskOffsetStr.empty()) {
                            maskOffsetValue = std::stoi(maskOffsetStr, 0, 10);
                        }

                        const patchInfo patch = {
                            .gameSerial = "*",
                            .modNameStr = currentPatchName,
                            .offsetStr = address,
                            .valueStr = patchValue,
                            .targetStr = targetStr,
                            .sizeStr = sizeStr,
                            .littleEndian = littleEndian,
                            .patchMask = patchMask,
                            .maskOffset = maskOffsetValue,
                        };
                        MemoryPatcher::PatchMemory(patch);
                    }
                }
            }
        }
    } else {
        LOG_ERROR(Loader, "Could not parse patch XML: {}", result.description());
    }
}

void OnGameLoaded() {
    std::filesystem::path patch_dir = Common::FS::GetUserPath(Common::FS::PathType::PatchesDir);
    if (!patch_file.empty()) {

        auto file_path = (patch_dir / patch_file).native();
        if (std::filesystem::exists(patch_file)) {
            ApplyPatchesFromXML(patch_file);
        } else {
            ApplyPatchesFromXML(file_path);
        }
    } else if (EmulatorState::GetInstance()->IsAutoPatchesLoadEnabled()) {
        for (auto const& repo : std::filesystem::directory_iterator(patch_dir)) {
            if (!repo.is_directory()) {
                continue;
            }
            std::ifstream json_file{repo.path() / "files.json"};
            nlohmann::json available_patches = nlohmann::json::parse(json_file);
            std::filesystem::path game_patch_file;
            for (auto const& [filename, serials] : available_patches.items()) {
                if (std::find(serials.begin(), serials.end(), g_game_serial) != serials.end()) {
                    game_patch_file = repo.path() / filename;
                    break;
                }
            }
            if (std::filesystem::exists(game_patch_file)) {
                ApplyPatchesFromXML(game_patch_file);
            }
        }
    }
    ApplyPendingPatches();
    // Every startup patch is in (every patch file and the queued ones): the Model LOD patch reads
    // its bias from a slot from now on.
    SetUpLodSlot(VR::GetSettings().lod_bias);
}

void AddPatchToQueue(const patchInfo& patchToAdd) {
    if (patches_applied) {
        PatchMemory(patchToAdd);
        return;
    }
    pending_patches.push_back(patchToAdd);
}

void ApplyPendingPatches() {
    patches_applied = true;
    for (size_t i = 0; i < pending_patches.size(); ++i) {
        const patchInfo& currentPatch = pending_patches[i];

        if (currentPatch.gameSerial != "*" && currentPatch.gameSerial != g_game_serial)
            continue;

        PatchMemory(currentPatch);
    }

    pending_patches.clear();
}

void PatchMemory(const patchInfo& patch) {
    // Send a request to modify the process memory.
    void* cheatAddress = nullptr;

    if (patch.patchMask == PatchMask::None) {
        if (patch.isOffset) {
            cheatAddress =
                reinterpret_cast<void*>(g_eboot_address + std::stoi(patch.offsetStr, 0, 16));
        } else {
            cheatAddress = reinterpret_cast<void*>(g_eboot_address +
                                                   (std::stoi(patch.offsetStr, 0, 16) - 0x400000));
        }
    }

    if (patch.patchMask == PatchMask::Mask) {
        cheatAddress = reinterpret_cast<void*>(PatternScan(patch.offsetStr) + patch.maskOffset);
    }

    if (patch.patchMask == PatchMask::Mask_Jump32) {
        int jumpSize = std::stoi(patch.sizeStr);

        constexpr int MAX_PATTERN_LENGTH = 256;
        if (jumpSize < 5) {
            LOG_ERROR(Loader, "Jump size must be at least 5 bytes");
            return;
        }
        if (jumpSize > MAX_PATTERN_LENGTH) {
            LOG_ERROR(Loader, "Jump size must be no more than {} bytes.", MAX_PATTERN_LENGTH);
            return;
        }

        // Find the base address using "Address"
        uintptr_t baseAddress = PatternScan(patch.offsetStr);
        if (baseAddress == 0) {
            LOG_ERROR(Loader, "PatternScan failed for mask_jump32 with pattern: {}",
                      patch.offsetStr);
            return;
        }
        uintptr_t patchAddress = baseAddress + patch.maskOffset;

        // Fills the original region (jumpSize bytes) with NOPs
        std::vector<u8> nopBytes(jumpSize, 0x90);
        std::memcpy(reinterpret_cast<void*>(patchAddress), nopBytes.data(), nopBytes.size());

        // Use "Target" to locate the start of the code cave
        uintptr_t jump_target = PatternScan(patch.targetStr);
        if (jump_target == 0) {
            LOG_ERROR(Loader, "PatternScan failed to Target with pattern: {}", patch.targetStr);
            return;
        }

        // Converts the Value attribute to a byte array (payload)
        std::vector<u8> payload;
        for (size_t i = 0; i < patch.valueStr.length(); i += 2) {
            std::string tempStr = patch.valueStr.substr(i, 2);
            const char* byteStr = tempStr.c_str();
            char* endPtr;
            unsigned int byteVal = std::strtoul(byteStr, &endPtr, 16);

            if (endPtr != byteStr + 2) {
                LOG_ERROR(Loader, "Invalid byte in Value: {}", patch.valueStr.substr(i, 2));
                return;
            }
            payload.push_back(static_cast<u8>(byteVal));
        }

        // Calculates the end of the code cave (where the return jump will be inserted)
        uintptr_t code_cave_end = jump_target + payload.size();

        // Write the payload to the code cave, from jump_target
        std::memcpy(reinterpret_cast<void*>(jump_target), payload.data(), payload.size());

        // Inserts the initial jump in the original region to divert to the code cave
        u8 jumpInstruction[5];
        jumpInstruction[0] = 0xE9;
        s32 relJump = static_cast<s32>(jump_target - patchAddress - 5);
        std::memcpy(&jumpInstruction[1], &relJump, sizeof(relJump));
        std::memcpy(reinterpret_cast<void*>(patchAddress), jumpInstruction,
                    sizeof(jumpInstruction));

        // Inserts jump back at the end of the code cave to resume execution after patching
        u8 jumpBack[5];
        jumpBack[0] = 0xE9;
        // Calculates the relative offset to return to the instruction immediately following the
        // overwritten region
        s32 target_return = static_cast<s32>((patchAddress + jumpSize) - (code_cave_end + 5));
        std::memcpy(&jumpBack[1], &target_return, sizeof(target_return));
        std::memcpy(reinterpret_cast<void*>(code_cave_end), jumpBack, sizeof(jumpBack));

        LOG_INFO(Loader,
                 "Applied Patch mask_jump32: {}, PatchAddress: {:#x}, JumpTarget: {:#x}, "
                 "CodeCaveEnd: {:#x}, JumpSize: {}",
                 patch.modNameStr, patchAddress, jump_target, code_cave_end, jumpSize);
        return;
    }

    if (cheatAddress == nullptr) {
        LOG_ERROR(Loader, "Failed to get address for patch {}", patch.modNameStr);
        return;
    }

    std::vector<unsigned char> bytePatch;

    for (size_t i = 0; i < patch.valueStr.length(); i += 2) {
        unsigned char byte = static_cast<unsigned char>(
            std::strtol(patch.valueStr.substr(i, 2).c_str(), nullptr, 16));

        bytePatch.push_back(byte);
    }

    if (patch.littleEndian) {
        std::reverse(bytePatch.begin(), bytePatch.end());
    }

    std::memcpy(cheatAddress, bytePatch.data(), bytePatch.size());

    LOG_INFO(Loader, "Applied patch: {}, Offset: {:#x}, Value: {}", patch.modNameStr,
             (uintptr_t)cheatAddress, patch.valueStr);
}

static std::vector<int32_t> PatternToByte(const std::string& pattern) {
    std::vector<int32_t> bytes;
    const char* start = pattern.data();
    const char* end = start + pattern.size();

    for (const char* current = start; current < end; ++current) {
        if (*current == '?') {
            ++current;
            if (*current == '?')
                ++current;
            bytes.push_back(-1);
        } else {
            bytes.push_back(strtoul(current, const_cast<char**>(&current), 16));
        }
    }

    return bytes;
}

uintptr_t PatternScan(const std::string& signature) {
    std::vector<int32_t> patternBytes = PatternToByte(signature);
    const auto scanBytes = static_cast<uint8_t*>((void*)g_eboot_address);

    const int32_t* sigPtr = patternBytes.data();
    const size_t sigSize = patternBytes.size();

    uint32_t foundResults = 0;
    for (uint32_t i = 0; i < g_eboot_image_size - sigSize; ++i) {
        bool found = true;
        for (uint32_t j = 0; j < sigSize; ++j) {
            if (scanBytes[i + j] != sigPtr[j] && sigPtr[j] != -1) {
                found = false;
                break;
            }
        }

        if (found) {
            foundResults++;
            return reinterpret_cast<uintptr_t>(&scanBytes[i]);
        }
    }

    return 0;
}

} // namespace MemoryPatcher
