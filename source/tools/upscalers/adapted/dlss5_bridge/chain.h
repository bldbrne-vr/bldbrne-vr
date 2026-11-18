// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../dlss_bridge/bbport_dlss_bridge.h"
namespace VrNeural {
bool Configure(bool enabled, unsigned profile, unsigned intensity,
               unsigned detail, unsigned lighting);
void Attach(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
            const wchar_t *directory, const wchar_t *data, BbDlssLogFn log);
bool Create(unsigned eye, VkCommandBuffer cmd, const BbDlssFeature &desc);
BbDlssImage Output(unsigned eye, VkCommandBuffer cmd,
                   const BbDlssImage &fallback);
bool Evaluate(unsigned eye, VkCommandBuffer cmd, const BbDlssEvaluate &frame);
bool Enabled();
// VR_DLSS5_ORDER=nr_first or overlay: Before runs the model on the input and replaces
// `color` with its decoded output for Super Resolution; Evaluate does nothing.
bool NeuralFirst();
bool Before(unsigned eye, VkCommandBuffer cmd, const BbDlssEvaluate &frame,
            BbDlssImage &color);
// VR_DLSS5_SHARED=1: the left eye's Before leaves `color` empty (its SR is
// deferred); after the right eye's Before, HasDeferredLeft hands back the left
// eye's frame, whose SR colour is Input(0).
bool HasDeferredLeft(BbDlssEvaluate &left);
BbDlssImage Input(unsigned eye);
// GPU timestamps: 0 before the chain, 1 between its two stages (the model and
// SR, in either order), 2 after it.
void Stamp(unsigned eye, VkCommandBuffer cmd, unsigned point);
void Release(unsigned eye);
void Shutdown();
} // namespace VrNeural
