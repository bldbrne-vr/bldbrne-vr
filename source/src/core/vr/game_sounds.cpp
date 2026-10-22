// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include <atomic>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/vr/fsb_sample.h"
#include "core/vr/game_sounds.h"
#include "core/vr/grind_audio.h"
#include "core/vr/lever_audio.h"
#include "core/vr/whip_audio.h"

namespace VR::GameSounds {
namespace {
constexpr const char* Bank = "/app0/dvdroot_ps4/sound/sprj_main_eng.fsb";
// Set once, never freed (the process's life).
std::atomic<const std::vector<char>*> grind{nullptr};
std::atomic<const std::vector<char>*> cutter_close{nullptr};
std::atomic<const std::vector<char>*> lever_clunk{nullptr};
std::atomic<bool> started{false};

std::span<const char> Of(const std::atomic<const std::vector<char>*>& a) {
    const auto* p = a.load(std::memory_order_acquire);
    return p ? std::span<const char>{p->data(), p->size()} : std::span<const char>{};
}
} // namespace

void LoadAsync() {
    if (started.exchange(true)) {
        return;
    }
    std::thread([] {
        auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
        const auto bank = mnt->ReadFile(Bank);
        if (!bank) {
            LOG_WARNING(Core, "VR sounds: no {}: no blade grind, the Beast Cutter's old clink",
                        Bank);
            return;
        }
        auto loop = Fsb::DecodeSample(*bank, Grind::SampleName);
        if (loop.size() >= 2 * 4 * Grind::LoopFade) {
            auto* p = new std::vector<char>(Grind::MakeSeamless(loop));
            grind.store(p, std::memory_order_release);
            LOG_INFO(Core, "VR sounds: blade grind loop {} ({:.2f} s)", Grind::SampleName,
                     static_cast<double>(p->size() / 2) / 24000.0);
        } else {
            LOG_WARNING(Core, "VR sounds: {} not decoded: no blade grind", Grind::SampleName);
        }
        auto close = Whip::MakeCutterClose(Fsb::DecodeSample(*bank, Whip::CutterLockSample),
                                           Fsb::DecodeSample(*bank, Whip::CutterRingSample));
        if (!close.empty()) {
            auto* p = new std::vector<char>(std::move(close));
            cutter_close.store(p, std::memory_order_release);
            LOG_INFO(Core, "VR sounds: Beast Cutter close {} + {} ({:.2f} s)",
                     Whip::CutterLockSample, Whip::CutterRingSample,
                     static_cast<double>(p->size() / 2) / 24000.0);
        } else {
            LOG_WARNING(Core, "VR sounds: {} / {} not decoded: the Beast Cutter's old clink",
                        Whip::CutterLockSample, Whip::CutterRingSample);
        }
        // The lever's clunk: in the map banks, not the main one (the first that has it).
        for (const char* path : LeverAudio::Banks) {
            const auto map_bank = mnt->ReadFile(path);
            if (!map_bank) {
                continue;
            }
            auto clunk = Fsb::DecodeSample(*map_bank, LeverAudio::SampleName);
            if (clunk.size() >= 4) {
                auto* p = new std::vector<char>(std::move(clunk));
                lever_clunk.store(p, std::memory_order_release);
                LOG_INFO(Core, "VR sounds: lever clunk {} from {} ({:.2f} s)",
                         LeverAudio::SampleName, path,
                         static_cast<double>(p->size() / 2) / 24000.0);
                break;
            }
        }
        if (lever_clunk.load(std::memory_order_acquire) == nullptr) {
            LOG_WARNING(Core, "VR sounds: {} not found in the map banks: levers pull silently",
                        LeverAudio::SampleName);
        }
    }).detach();
}

std::span<const char> GrindLoop() {
    return Of(grind);
}

std::span<const char> CutterClose() {
    return Of(cutter_close);
}

std::span<const char> LeverClunk() {
    return Of(lever_clunk);
}

} // namespace VR::GameSounds
