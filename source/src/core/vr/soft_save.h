// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <mutex>
#include <string_view>
#include "common/types.h"

// Enabled only on a save home with a verified backup made before this launch: the portable trial's
// prelaunch backup, or the launcher's backup of the player's save (soft_save_backup.h). Never on a
// save home it found by searching.
namespace VR::SoftSave {
void EbootLoaded(VAddr address, u64 size);
void Death();
void LanternUsed();
// PREY SLAUGHTERED / NIGHTMARE SLAIN: a boss is dead. Clears the soft save as a lantern does, so a
// reload can never bring the boss back and take its reward away.
void BossKilled();
// Held before an open can create/truncate a file, and around path mutations.
// Empty for ordinary files or when soft-save support is disabled.
std::unique_lock<std::recursive_mutex> LockIo(std::string_view path, std::string_view other = {});
std::filesystem::path ReadOverride(std::string_view path, bool read_only);
void NoteOpen(int fd, std::string_view path, bool write, bool snapshot_read = false);
void NoteClose(int fd, std::string_view path);
} // namespace VR::SoftSave
