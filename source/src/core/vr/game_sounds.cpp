// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/vr/fsb_sample.h"
#include "core/vr/game_sounds.h"
#include "core/vr/grind_audio.h"
#include "core/vr/whip_audio.h"

namespace VR::GameSounds {
namespace {
constexpr const char* Bank = "/app0/dvdroot_ps4/sound/sprj_main_eng.fsb";
// Set once, never freed (the process's life).
std::atomic<const std::vector<char>*> grind{nullptr};
std::atomic<const std::vector<char>*> cutter_close{nullptr};
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
    }).detach();
}

std::span<const char> GrindLoop() {
    return Of(grind);
}

std::span<const char> CutterClose() {
    return Of(cutter_close);
}

} // namespace VR::GameSounds
