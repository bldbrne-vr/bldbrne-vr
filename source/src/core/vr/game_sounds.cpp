// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <thread>
#include <tuple>
#include <vector>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/vr/charge_audio.h"
#include "core/vr/fsb_sample.h"
#include "core/vr/furnace_audio.h"
#include "core/vr/game_sounds.h"
#include "core/vr/grind_audio.h"
#include "core/vr/item_audio.h"
#include "core/vr/lever_audio.h"
#include "core/vr/quick_use_audio.h"
#include "core/vr/thunder_audio.h"
#include "core/vr/whip_audio.h"
#include "core/vr/whistle_audio.h"

namespace VR::GameSounds {
namespace {
constexpr const char* Bank = "/app0/dvdroot_ps4/sound/sprj_main_eng.fsb";
// Set once, never freed (the process's life).
std::atomic<const std::vector<char>*> grind{nullptr};
std::atomic<const std::vector<char>*> cutter_close{nullptr};
std::atomic<const std::vector<char>*> lever_clunk{nullptr};
std::atomic<const std::vector<char>*> whistle_call{nullptr};
std::atomic<const std::vector<char>*> augur_splash{nullptr};
std::atomic<const std::vector<char>*> gulp{nullptr};
std::atomic<const std::vector<char>*> furnace_snap{nullptr};
std::atomic<const std::vector<char>*> furnace_whoomph{nullptr};
std::atomic<const std::vector<char>*> furnace_growl{nullptr};
std::atomic<const std::vector<char>*> beyond_call{nullptr};
std::atomic<const std::vector<char>*> charge_ready{nullptr};
std::atomic<const std::vector<char>*> thunder_clap{nullptr};
std::atomic<float> whistle_s{0.0f};
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
        auto call = Fsb::DecodeSample(*bank, WhistleAudio::SampleName);
        if (call.size() >= 4) {
            auto* p = new std::vector<char>(std::move(call));
            whistle_call.store(p, std::memory_order_release);
            const float blown = WhistleAudio::RequiredHoldS(WhistleAudio::CallLength(*p));
            whistle_s.store(blown, std::memory_order_release);
            LOG_INFO(Core, "VR sounds: Madaras Whistle call {} ({:.2f} s, blown {:.2f} s)",
                     WhistleAudio::SampleName, static_cast<double>(p->size() / 2) / 24000.0,
                     blown);
        } else {
            LOG_WARNING(Core, "VR sounds: {} not decoded: the Madaras Whistle is used at once, "
                              "with the game's own sound",
                        WhistleAudio::SampleName);
        }
        // An item used at once: the squeezed Augur's splash and the swallow, each cut short.
        for (const auto& [cut, out, what] :
             {std::tuple{ItemAudio::SplashCut, &augur_splash, "Augur splash"},
              std::tuple{ItemAudio::GulpCut, &gulp, "gulp"}}) {
            auto clip = ItemAudio::Cut(Fsb::DecodeSample(*bank, cut.sample), cut);
            if (clip.empty()) {
                LOG_WARNING(Core, "VR sounds: {} not decoded: no {}", cut.sample, what);
                continue;
            }
            auto* p = new std::vector<char>(std::move(clip));
            out->store(p, std::memory_order_release);
            LOG_INFO(Core, "VR sounds: {} {} {}..{} ms ({:.2f} s)", what, cut.sample, cut.from_ms,
                     cut.to_ms, static_cast<double>(p->size() / 2) / 24000.0);
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
        // The Boom Hammer's furnace: the flint snap, the snap with the torch's fwomp, the growl's
        // loop (core/vr/furnace_audio.h).
        namespace Fa = FurnaceAudio;
        auto snap = ItemAudio::Cut(Fsb::DecodeSample(*bank, Fa::SnapCut.sample), Fa::SnapCut);
        auto swing = ItemAudio::Cut(Fsb::DecodeSample(*bank, Fa::SwingCut.sample), Fa::SwingCut);
        auto whoomph = Fa::MakeWhoomph(snap, swing);
        if (!snap.empty() && !whoomph.empty()) {
            furnace_whoomph.store(new std::vector<char>(std::move(whoomph)),
                                  std::memory_order_release);
            furnace_snap.store(new std::vector<char>(std::move(snap)), std::memory_order_release);
            LOG_INFO(Core, "VR sounds: Boom Hammer pulls {} {}..{} ms, + {} {}..{} ms",
                     Fa::SnapCut.sample, Fa::SnapCut.from_ms, Fa::SnapCut.to_ms,
                     Fa::SwingCut.sample, Fa::SwingCut.from_ms, Fa::SwingCut.to_ms);
        } else {
            LOG_WARNING(Core, "VR sounds: {} / {} not decoded: the Boom Hammer's pulls are felt "
                              "only",
                        Fa::SnapCut.sample, Fa::SwingCut.sample);
        }
        const auto& g = Fa::GrowlLoop;
        auto growl = ItemAudio::Cut(Fsb::DecodeSample(*bank, g.sample),
                                    {g.sample, g.from_ms, g.to_ms, 0, 0});
        if (growl.size() >= 2 * 4 * Fa::GrowlFade) {
            auto* p = new std::vector<char>(Grind::MakeSeamless(growl, Fa::GrowlFade));
            furnace_growl.store(p, std::memory_order_release);
            LOG_INFO(Core, "VR sounds: Boom Hammer growl {} {}..{} ms ({:.2f} s loop)", g.sample,
                     g.from_ms, g.to_ms, static_cast<double>(p->size() / 2) / 24000.0);
        } else {
            LOG_WARNING(Core, "VR sounds: {} not decoded: the Boom Hammer's furnace idles "
                              "silently",
                        g.sample);
        }
        // A Call Beyond raised: the game's own call from where its stars launch
        // (core/vr/quick_use_audio.h CallSample).
        namespace Q = Quick;
        auto beyond = ItemAudio::Cut(Fsb::DecodeSample(*bank, Q::CallSample),
                                     {Q::CallSample, Q::CallFromMs, 60'000, Q::CallFadeInMs,
                                      Q::CallFadeOutMs});
        if (!beyond.empty()) {
            auto* p = new std::vector<char>(std::move(beyond));
            beyond_call.store(p, std::memory_order_release);
            LOG_INFO(Core, "VR sounds: A Call Beyond's call {} from {} ms ({:.2f} s)",
                     Q::CallSample, Q::CallFromMs, static_cast<double>(p->size() / 2) / 24000.0);
        } else {
            LOG_WARNING(Core, "VR sounds: {} not decoded: A Call Beyond is called silently",
                        Q::CallSample);
        }
        // A melee attack at its full charge: the game's charge flash, cut short.
        const auto& rc = ChargeAudio::ReadyCut;
        auto ring = ItemAudio::Cut(Fsb::DecodeSample(*bank, rc.sample), rc);
        if (!ring.empty()) {
            auto* p = new std::vector<char>(std::move(ring));
            charge_ready.store(p, std::memory_order_release);
            LOG_INFO(Core, "VR sounds: full charge {} ({:.2f} s)", rc.sample,
                     static_cast<double>(p->size() / 2) / 24000.0);
        } else {
            LOG_WARNING(Core, "VR sounds: {} not decoded: a full charge rings the hand only",
                        rc.sample);
        }
        // Tonitrus charged: its thunderclap, layered from the game's own.
        {
            namespace Th = ThunderAudio;
            std::array<std::vector<char>, Th::Layers.size()> cuts;
            for (std::size_t k = 0; k < Th::Layers.size(); ++k) {
                const auto& cut = Th::Layers[k].cut;
                cuts[k] = ItemAudio::Cut(Fsb::DecodeSample(*bank, cut.sample), cut);
            }
            auto clap = Th::Mix(cuts, Th::Layers);
            if (!clap.empty()) {
                auto* p = new std::vector<char>(std::move(clap));
                thunder_clap.store(p, std::memory_order_release);
                LOG_INFO(Core, "VR sounds: Tonitrus thunderclap, {} layers ({:.2f} s)",
                         Th::Layers.size(), static_cast<double>(p->size() / 2) / 24000.0);
            } else {
                LOG_WARNING(Core, "VR sounds: no thunderclap decoded: Tonitrus charges silently");
            }
        }
    }).detach();
}

std::span<const char> FurnaceSnap() {
    return Of(furnace_snap);
}

std::span<const char> FurnaceWhoomph() {
    return Of(furnace_whoomph);
}

std::span<const char> FurnaceGrowl() {
    return Of(furnace_growl);
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

std::span<const char> WhistleCall() {
    return Of(whistle_call);
}

std::span<const char> AugurSplash() {
    return Of(augur_splash);
}

std::span<const char> Gulp() {
    return Of(gulp);
}

std::span<const char> BeyondCall() {
    return Of(beyond_call);
}

std::span<const char> ChargeReady() {
    return Of(charge_ready);
}

std::span<const char> ThunderClap() {
    return Of(thunder_clap);
}

float WhistleCallS() {
    return whistle_s.load(std::memory_order_acquire);
}

} // namespace VR::GameSounds
