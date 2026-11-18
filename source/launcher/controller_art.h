#pragma once

#include <array>
#include <vector>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "resource.h"
#include "embedded_image.h"

namespace ControlsGuide {
using ControllerArt = Launcher::EmbeddedImage;
inline ControllerArt LoadControllerArt(ID3D11Device* device, int id) {
    return Launcher::LoadEmbeddedImage(device, id);
}

// Only the selected controller pair is resident. An absent resource leaves the text guide usable.
class ControllerArtCache {
public:
    const std::array<ControllerArt, 2>& Get(ID3D11Device* device, int platform) {
        if (platform != platform_) {
            art_ = {};
            platform_ = platform;
            if (platform >= 0 && platform < 4) {
                for (int hand = 0; hand < 2; ++hand)
                    art_[hand] = LoadControllerArt(device, IDR_CONTROLLER_QUEST_LEFT + platform * 2 + hand);
            }
        }
        return art_;
    }
private:
    int platform_{-1};
    std::array<ControllerArt, 2> art_;
};
} // namespace ControlsGuide
