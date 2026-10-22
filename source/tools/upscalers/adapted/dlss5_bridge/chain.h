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
void Release(unsigned eye);
void Shutdown();
} // namespace VrNeural
