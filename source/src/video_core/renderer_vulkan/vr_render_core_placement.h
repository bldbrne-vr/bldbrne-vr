// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifdef _WIN32
#include <array>

namespace Vulkan {
// Producer-owned. Borrowed thread handles stay valid until Reset(), before either worker joins.
class RenderCorePlacement {
public:
    RenderCorePlacement() = default;
    RenderCorePlacement(const RenderCorePlacement&) = delete;
    RenderCorePlacement& operator=(const RenderCorePlacement&) = delete;
    bool Apply(void* stage, void* recorder);
    void Reset();

private:
    void* stage_thread{};
    void* recorder_thread{};
    unsigned long stage_id{}, recorder_id{};
    std::array<unsigned long, 14> other_ids{};
    bool installed_defaults{};
};
} // namespace Vulkan
#endif
