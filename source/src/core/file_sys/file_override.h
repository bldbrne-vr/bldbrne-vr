// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "core/file_sys/ifile.h"

// Files the emulator itself serves in place of a game file: read-only, from memory, ahead of the
// mount's whole backend stack (-mods, -UPDATE / -patch, the base). Used by the Bloodborne VR mod
// for its Options movie (core/vr/bloodborne_vrtile.cpp). The game's own file is left untouched on
// disk; with nothing registered (the default) every open behaves as before.
namespace Core::FileSys {

// Serves `bytes` for reads of `guest_path` (a sanitized guest path such as
// "game asset", compared case-insensitively). `tag` names the owner in
// the log line of the first opens. Replaces an earlier registration of the same path.
void SetFileOverride(const std::string& guest_path, std::vector<u8> bytes, std::string tag);

// As SetFileOverride, but the bytes are built by `build` on a worker thread started now (for a
// file that takes a while to make, e.g. the VR tile's icon atlas: ~0.4 s of deflate). An open
// before it is done waits for it (logged). An empty result (or an exception) drops the
// registration: the game's own file is served after all (logged).
void SetFileOverrideDeferred(const std::string& guest_path,
                             std::function<std::vector<u8>()> build, std::string tag);

// Removes a registration (no-op if there is none).
void ClearFileOverride(const std::string& guest_path);

// MntPoints::Open: the registered file for this sanitized guest path, or nullptr.
std::unique_ptr<IFile> OpenFileOverride(std::string_view guest_path);

// MntPoints::Open, every read-only open: notes the opens of the game's cutscene files
// ("/remo/", FromSoftware's in-engine cutscenes), so the VR mod can tell one is about to play.
void NoteGuestOpen(std::string_view guest_path);
// steady_clock nanoseconds of the last cutscene file opened (0: none yet). Any thread.
s64 LastCutsceneOpenNs();

} // namespace Core::FileSys
