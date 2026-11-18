// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/startup_compile.h"

#include <cstdio>
#include <SDL3/SDL.h>
#include "common/logging/log.h"

namespace VideoCore::StartupCompile {
struct DisplayState {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    const char* stage{"Preparing shaders"};
    std::size_t total{}, done{};
    bool attempted{}, hidden{};
    std::chrono::steady_clock::time_point drawn{};

    void DropWindowEvents() {
        if (window)
            SDL_FilterEvents(
                [](void* data, SDL_Event* event) -> bool {
                    auto& state = *static_cast<DisplayState*>(data);
                    if (SDL_GetWindowFromEvent(event) != state.window)
                        return true;
                    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                        state.hidden = true;
                    return false;
                },
                this);
    }
    ~DisplayState() {
        DropWindowEvents();
        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);
    }
    void Draw(bool force = false) {
        if (!attempted) {
            attempted = true;
            if (!SDL_IsMainThread() || !(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO))
                return;
            window =
                SDL_CreateWindow("BB-VR - Preparing shaders", 720, 240, SDL_WINDOW_ALWAYS_ON_TOP);
            if (window)
                renderer = SDL_CreateRenderer(window, "software");
            if (!renderer) {
                LOG_WARNING(Render_Vulkan, "Startup shader progress unavailable: {}",
                            SDL_GetError());
                if (window)
                    SDL_DestroyWindow(window);
                window = nullptr;
                return;
            }
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
        if (!renderer)
            return;
        // Pump without consuming the main window's resize/device events. Its presenter is
        // still being constructed and will process those through the normal event loop.
        SDL_PumpEvents();
        DropWindowEvents(); // Never feed this dialog's resize/input events into the game window.
        if (hidden) {
            SDL_HideWindow(window);
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - drawn < std::chrono::milliseconds{50})
            return;
        drawn = now;
        SDL_SetRenderDrawColor(renderer, 20, 22, 27, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderScale(renderer, 2, 2);
        SDL_SetRenderDrawColor(renderer, 230, 225, 210, 255);
        SDL_RenderDebugText(renderer, 16, 16, stage);
        SDL_SetRenderScale(renderer, 1, 1);
        SDL_RenderDebugText(renderer, 32, 74, "Usually needed on first launch or after updates.");
        SDL_RenderDebugText(renderer, 32, 92,
                            "Existing caches are also prepared at startup. Please wait.");
        const SDL_FRect track{32, 125, 656, 20};
        SDL_SetRenderDrawColor(renderer, 55, 58, 65, 255);
        SDL_RenderFillRect(renderer, &track);
        const float fraction = total ? std::min(1.0, double(done) / double(total)) : 0.0;
        SDL_FRect fill = track;
        fill.w *= fraction;
        SDL_SetRenderDrawColor(renderer, 155, 190, 175, 255);
        if (total)
            SDL_RenderFillRect(renderer, &fill);
        char label[96];
        if (total)
            std::snprintf(label, sizeof(label), "%zu / %zu  (%d%%)", done, total,
                          int(fraction * 100));
        else
            std::snprintf(label, sizeof(label), "Reading cache...");
        SDL_SetRenderDrawColor(renderer, 230, 225, 210, 255);
        SDL_RenderDebugText(renderer, 32, 161, label);
        SDL_RenderDebugText(renderer, 32, 195,
                            "You can close this window; preparation will continue.");
        SDL_RenderPresent(renderer);
    }
};
namespace {
thread_local DisplayState* active{};
}
Display::Display() {
    if (!active)
        active = impl = new DisplayState;
}
Display::~Display() {
    if (impl) {
        active = nullptr;
        delete impl;
    }
}
void Begin(const char* stage, std::size_t total) {
    LOG_INFO(Render_Vulkan,
             "Startup shaders: {} ({} jobs; {} workers, batches of {}, {} ms pauses)", stage, total,
             Workers(), BatchSize, BatchPause.count());
    if (active) {
        active->stage = stage;
        active->done = 0;
        active->total = total;
        active->Draw(true);
    }
}
void Update(std::size_t done) {
    if (active) {
        active->done = done;
        active->Draw();
    }
}
} // namespace VideoCore::StartupCompile
