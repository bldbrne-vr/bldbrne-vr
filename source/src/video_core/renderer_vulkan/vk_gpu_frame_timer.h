// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

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
