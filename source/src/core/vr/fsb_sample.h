// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// One named sample of a Bloodborne FMOD sound bank (FSB5, ATRAC9: dvdroot_ps4/sound/*.fsb), read
// from the player's own game files at runtime (nothing of the game is shipped), decoded with
// LibAtrac9 and handed to the VR mixers as mono S16LE at 24 kHz (core/vr/whip_audio.h). Empty when
// the bank or the sample is not there, or anything about it is not as expected.
//   FSB5: "FSB5", version, sample count, sample headers' size, name table's size, data size, mode;
//   header 60 bytes (version 1) else 64. Each sample: a u64 (bit 0: extra chunks follow; bits 5..6:
//   channels 1 / 2 / 6 / 8; bits 7..33: the data offset / 32; bits 34..63: its sample count), then
//   its chunks (a u32: bit 0 another follows, bits 1..24 its size, bits 25..31 its type; type 9: the
//   ATRAC9 config, its bytes 4..7 the decoder's 4-byte config). The names: one u32 offset per
//   sample, then the strings. The data after the names; a sample's runs to the next offset.
namespace VR::Fsb {

std::vector<char> DecodeSample(std::span<const std::uint8_t> bank, std::string_view name);

} // namespace VR::Fsb
