// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Vulkan {

class Instance;
class Scheduler;

// VR_GPU_TIMING=1 (profiling): the GPU time of the game's frames. Every command buffer of the
// scheduler that presents (the draw scheduler) gets a timestamp at its start and its end; the
// buffers submitted between two flips are one game frame, whose GPU busy time (the union of the
// buffers' spans) and span (first start -> last end) are logged every 300 frames as the
// `VR gpu frame:` line. Off (the default): nothing is recorded.
//
// Called by Presenter::PrepareFrame after the flip's submission: the first call picks the
// scheduler and creates the query pool, each later one ends a game frame and reads the finished
// timestamps (never waits for the GPU).
void GpuFrameTimerMark(Scheduler& scheduler, const Instance& instance);

} // namespace Vulkan
