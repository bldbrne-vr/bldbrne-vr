// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
//
// FAST_CPU_LEAN_SCOPES (on unless '0'; read once at start-up, a plain load per use): instrumentation
// that is off costs one load and a predictable branch where it sits, with its work out of line:
// ScratchProfile::Cpu (VR_CPU_PROFILE), BProfile::Scope (FAST_B_PROFILE), the FAST_PIPE_VERIFY
// asserts, and the VR state and native-stereo registry accessors (their function-local statics'
// guards read thread-local storage on every call). The recorder times 1 packet in 256 instead of
// 1 in 64 for the FAST encode line. '0': each of them takes its previous path again, for A/B timing.
// What any of them measures, checks or returns is the same either way.
#pragma once
#include <cstdlib>
namespace FastRender {
inline const bool g_lean_scopes = [] {
    const char* value = std::getenv("FAST_CPU_LEAN_SCOPES");
    return !value || value[0] != '0';
}();
} // namespace FastRender
