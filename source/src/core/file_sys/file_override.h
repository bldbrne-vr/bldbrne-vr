// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

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
// "/app0/dvdroot_ps4/menu/ingametop.gfx", compared case-insensitively). `tag` names the owner in
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

// A switched layer over a path (the mod's fog, core/vr/bloodborne_woods_shadows.cpp: the menu's
// Fog row applies at the next load of an area, without a restart). While `on()` says so at an open
// (asked outside the lock; one load's opens and stats see the same file: file_override_decide.h),
// the layer's file is served; otherwise the path is served as if no layer were registered
// (its plain override, or the game's own file). `opened`, where given, is told at
// every open of the path (not a stat's: FileOverrideStatScope) whether the layer applied (it
// applies too while on when its build left the file as it was; not while on and never built).
// Replaces an earlier layer of the same path.
using FileOverrideLayerOn = bool (*)();
using FileOverrideLayerOpened = void (*)(std::string_view guest_path, bool applied);
void SetFileOverrideLayer(const std::string& guest_path, FileOverrideLayerOn on,
                          FileOverrideLayerOpened opened, std::string tag);

// The layer's file, built by `build` on a worker thread started now (the start: every layer at
// once); once (a layer built, being built or queued is left as it is; a path without a layer is
// ignored). `build` reads the file under the layer itself (in a FileOverrideLayerBypass scope) and
// returns the changed file, or an empty vector when it leaves the file as it was; an exception:
// not built, the file under it is served and the open told the layer did not apply (logged). An
// open while the layer is on and its build runs waits for it (logged).
void BuildFileOverrideLayer(const std::string& guest_path, std::function<std::vector<u8>()> build);

// As BuildFileOverrideLayer, queued (in play): one worker builds the queued layers one after
// another; an open that wants a layer whose build has not started builds it itself, at once (it
// waits behind no other layer's build), and one whose build is running waits for that one only.
// No exception leaves it (none in the worker either).
void QueueFileOverrideLayer(const std::string& guest_path, std::function<std::vector<u8>()> build);

// While one of these lives on a thread, that thread's opens see no layers: the file as served
// under them (a layer's build reads its own path so).
class FileOverrideLayerBypass {
public:
    FileOverrideLayerBypass();
    ~FileOverrideLayerBypass();
    FileOverrideLayerBypass(const FileOverrideLayerBypass&) = delete;
    FileOverrideLayerBypass& operator=(const FileOverrideLayerBypass&) = delete;
};

// While one of these lives on a thread, that thread's opens are a stat's (the kernel's stat: the
// file's size as the next open serves it): a layer decides as for an open, and that decision is
// the next open's, however late (a code review, 2026-11-21), but `opened` is not told (a stat is
// no load).
class FileOverrideStatScope {
public:
    FileOverrideStatScope();
    ~FileOverrideStatScope();
    FileOverrideStatScope(const FileOverrideStatScope&) = delete;
    FileOverrideStatScope& operator=(const FileOverrideStatScope&) = delete;
};

// MntPoints::Open: the registered file for this sanitized guest path, or nullptr.
std::unique_ptr<IFile> OpenFileOverride(std::string_view guest_path);

// MntPoints::Open, every read-only open: notes the opens of the game's cutscene files
// ("/remo/", FromSoftware's in-engine cutscenes), so the VR mod can tell one is about to play.
void NoteGuestOpen(std::string_view guest_path);
// steady_clock nanoseconds of the last cutscene file opened (0: none yet). Any thread.
s64 LastCutsceneOpenNs();
// Optional process-lifetime observer, called before opening cutscene assets. It
// must not retain guest_path, open files recursively, or alter the file itself.
using CutsceneOpenObserver = void (*)(std::string_view guest_path);
void SetCutsceneOpenObserver(CutsceneOpenObserver observer);

} // namespace Core::FileSys
