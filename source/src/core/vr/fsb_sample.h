// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// One named sample of a Bloodborne FMOD sound bank (FSB5, ATRAC9: game asset), read
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
