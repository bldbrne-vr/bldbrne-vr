// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Native stereo's Auto eyes, shared by the runtime (NativeStereo::ChooseLayerDraws, the card it
// opens) and the launcher (Native stereo eyes, the card DXGI reports): layer draws on AMD RX 6000
// cards (RDNA 2), whose own multiview left the right eye white once the world loaded (GitHub #9,
// RX 6700), and on the older RX 400, RX 500 and RX 5000 cards, which start in DIBR (the launcher's
// Auto stereo, DibrByDefault; the user's choice, 2026-10-26); multiview on every other card (an
// RX 9070 XT runs it fine, 2026-10-25).
#pragma once

#include <cstdint>
#include <string_view>

namespace VR::NativeViews {

// "RX " and a model number of exactly `digits` digits starting with `first` in the name
// ("AMD Radeon RX 6700": '6', 4).
inline bool RxModel(std::string_view name, char first, size_t digits) {
    const auto digit = [&](size_t i) {
        return i < name.size() && name[i] >= '0' && name[i] <= '9';
    };
    for (size_t at = name.find("RX "); at != std::string_view::npos; at = name.find("RX ", at + 1)) {
        const size_t model = at + 3;
        if (model >= name.size() || name[model] != first)
            continue;
        size_t n = 0;
        while (digit(model + n))
            ++n;
        if (n == digits)
            return true;
    }
    return false;
}

// RX 6000 (RDNA 2: Navi 21, 22, 23 and 24, device ids 0x73a0..0x743f), or "RX 6" and three more
// digits in the name ("AMD Radeon RX 6700"; not the older RX 640).
inline bool AmdRx6000(uint32_t device_id, std::string_view name) {
    return (device_id >= 0x73a0 && device_id <= 0x743f) || RxModel(name, '6', 4);
}

// RX 400 and RX 500 (Polaris 10, 11, 12, 20 and 30; the two series share chips and device ids:
// an RX 480 and an RX 580 are both 0x67df) and RX 5000 (RDNA 1: Navi 10, 12 and 14), device ids
// from pci.ids; or the model in the name ("RX 480", "RX 580", "RX 5700 XT").
inline bool AmdRx400To5000(uint32_t device_id, std::string_view name) {
    const bool polaris = (device_id >= 0x67c0 && device_id <= 0x67ff) ||
                         (device_id >= 0x6980 && device_id <= 0x699f) || device_id == 0x6fdf;
    const bool rdna1 = (device_id >= 0x7310 && device_id <= 0x731f) ||
                       (device_id >= 0x7340 && device_id <= 0x734f) || device_id == 0x7360 ||
                       device_id == 0x7362;
    return polaris || rdna1 || RxModel(name, '4', 3) || RxModel(name, '5', 3) ||
           RxModel(name, '5', 4);
}

// The launcher's Auto stereo starts DIBR on these cards; native stereo, when chosen, draws their
// eyes with layers (LayersByDefault).
inline bool DibrByDefault(uint32_t vendor_id, uint32_t device_id, std::string_view name) {
    return vendor_id == 0x1002 && AmdRx400To5000(device_id, name);
}

inline bool LayersByDefault(uint32_t vendor_id, uint32_t device_id, std::string_view name) {
    return vendor_id == 0x1002 && (AmdRx6000(device_id, name) || AmdRx400To5000(device_id, name));
}

} // namespace VR::NativeViews
