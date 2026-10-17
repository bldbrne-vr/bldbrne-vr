// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Windows: what the exception handler (core/signals.cpp) does with a privileged-instruction fault
// (EXCEPTION_PRIV_INSTRUCTION, 0xC0000096).
//
// The CPU patcher (core/cpu_patches.cpp) rewrites a guest instruction (an FS-relative access, e.g.
// `64 48 8b 04 25 ..`) into a near jump to its trampoline the first time it faults, while other
// guest threads (the job workers run the same code on several cores) may be fetching it
// (PublishNearJump: a 2-byte `jmp $` parks the site, then the tail is written, then the head). A
// fetch that still tears across that rewrite - the old first bytes (the FS / REX prefixes) with
// the jump's new tail - decodes an instruction from the jump's displacement. In every crash it was
// port I/O, privileged in user mode, so Windows raised EXCEPTION_PRIV_INSTRUCTION at the site,
// whose bytes by then were the finished jump: the user's usertest_1113_crash.txt "Unhandled
// Exception code 0xc0000096 at 0x6c7b799 .. bytes e9 df ec 42 04 90 90 90" (byte 2, EC = in al,dx)
// and 4 of 1843 mock runs (ee, ed, ef: out / in; 6d: insd), in CSChrThread* / HavokWorkerThread.
// Until 2026-10-09 such a fault reached the patch handler only with static red-zone patching on;
// otherwise it was reported as unhandled and the emulator shut down. (A torn fetch that decodes
// an FS access instead faults as an access violation at the site, which the patch handler already
// resumed; on Linux / macOS port I/O raises SIGSEGV, the same path.)
// Now, in every mode, a privileged-instruction fault exactly at a site the patcher rewrote resumes
// there (the finished jump runs; the fault itself serializes the core's fetch); any other
// privileged instruction is handled as before (static patching: the patch handler; otherwise
// reported).
// Pure for tests/test_patch_fault.cpp.
namespace Core::PatchFault {

enum class PrivAction {
    ResumeAtSite, // a torn fetch of a patched site: run the finished jump
    PatchHandler, // static red-zone patching: the illegal-instruction handlers (as before)
    Report,       // anything else: reported as unhandled (as before)
};

// `at_patched_site`: the fault's address is exactly a site the patcher rewrote
// (cpu_patches.cpp RetryAtPatchedSite, which waits for a rewrite in progress).
constexpr PrivAction OnPrivilegedInstruction(bool at_patched_site, bool static_patching) {
    if (at_patched_site) {
        return PrivAction::ResumeAtSite;
    }
    return static_patching ? PrivAction::PatchHandler : PrivAction::Report;
}

} // namespace Core::PatchFault
