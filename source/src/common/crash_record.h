// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

// A crash's one line in user/log/crash.txt, written in every build (the play build has no log):
// a failed check's source file and line, or an exception's code and module+offset (the
// executable loads at its fixed base, so an offset maps to a function in its linker map).
namespace Common::CrashRecord {

// A failed ASSERT / UNREACHABLE at `file`:`line` (the file's name only is written).
void NoteFailedCheck(const char* file, int line, bool unreachable);
// A plain line of explanation for the crash about to follow (out of memory, for one).
void Note(const char* what);
// Windows: an unhandled exception filter that notes the exception, then lets it end the process
// as before (the exit code is unchanged). Call once at start.
void InstallExceptionNote();

} // namespace Common::CrashRecord
