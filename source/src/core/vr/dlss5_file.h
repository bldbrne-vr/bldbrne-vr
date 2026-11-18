// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <string>

// DLSS 5's runtime (nvngx_dlssnr.dll) is NVIDIA's and is never shipped (2026-10-27):
// the player brings their own copy. The launcher's DLSS 5 tab finds one on the PC (a game that
// ships it) or lets the player pick it, checks it and copies it to user/dlss5; the game loads it
// from there (vr_dlss_runtime.cpp VerifiedNeuralFile). Shared by the game and the launcher.
namespace VR::Dlss5File {

inline constexpr const wchar_t* Name = L"nvngx_dlssnr.dll";

// Where the player's copy lives: <user folder>/dlss5/nvngx_dlssnr.dll (the launcher's
// Paths::User() is the game's portable user folder).
inline std::filesystem::path InUser(const std::filesystem::path& user) {
    return user / "dlss5" / Name;
}

struct Check {
    bool ok{};
    std::string version; // "310.9.16.0", when the file has a version
    std::string problem; // why it is refused, for the player
};

// Accepts only NVIDIA's own file, unmodified: an intact Authenticode signature by NVIDIA
// Corporation and the product name "NVIDIA DLSSNR" (not DLSS Super Resolution renamed). It hashes
// the whole file (about 160 MB, a fraction of a second): call it off the UI and render threads.
Check Verify(const std::filesystem::path& file);

// "310.9.16.0" is newer than "310.8.0.0" (numeric parts, left to right).
bool Newer(const std::string& a, const std::string& b);

} // namespace VR::Dlss5File
