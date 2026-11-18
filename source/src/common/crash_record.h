// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <string>

// A crash's one line in user/log/crash.txt, written in every build (the play build has no log):
// a failed check's source file and line, or an exception's code and module+offset (the
// executable loads at its fixed base, so an offset maps to a function in its linker map).
namespace Common::CrashRecord {

// A failed ASSERT / UNREACHABLE at `file`:`line` (the file's name only is written).
void NoteFailedCheck(const char* file, int line, bool unreachable);
// A plain line of explanation for the crash about to follow (out of memory, for one).
void Note(const char* what);
// Windows: an unhandled exception filter that notes the exception, then lets it end the process
// as before (the exit code is unchanged). Call once at start. With dumps on (DumpsEnabled) it
// also writes a minidump of the process: every thread's stack.
void InstallExceptionNote();

// Minidumps (Windows, dbghelp): in logs-on builds, or with VR_CRASH_DUMP=1 in any build
// (VR_CRASH_DUMP=0: never). Their symbols are the exact build's PDB, kept by whoever built it.
bool DumpsEnabled();
// A minidump of this process into user/log/<tag>-<time>.dmp (`exception`: an
// EXCEPTION_POINTERS*, or null for a live process); its file name, or empty when none was
// written. At most a few per log folder.
std::string WriteDump(const char* tag, void* exception = nullptr);

} // namespace Common::CrashRecord
