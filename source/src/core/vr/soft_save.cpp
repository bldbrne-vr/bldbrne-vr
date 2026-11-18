// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/vr/hook_util.h"
#include "core/vr/soft_save.h"
#include "core/vr/soft_save_backup.h"
#include "core/vr/soft_save_channel.h"
#include "core/vr/soft_save_policy.h"
#include "core/vr/soft_save_read_lease.h"
#include "core/vr/soft_save_storage.h"

namespace VR::SoftSave {
namespace {
using Hook::Plausible;
using Hook::Read;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
constexpr u64 WorldChr = 0x553e878, GameMan = 0x5556678, GameData = 0x553b130;
constexpr u64 LuaEventMan = 0x553b0c8, SaveRequest = 0x1336da0;
constexpr std::array<u8, 116> SaveSignature = {
    0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x05, 0x1d, 0x43, 0x20, 0x04, 0x48, 0x83, 0x38, 0x00,
    0x75, 0x21, 0x48, 0x8d, 0x3d, 0xb1, 0x45, 0x60, 0x03, 0x48, 0x8d, 0x15, 0xfe, 0x45, 0x60,
    0x03, 0x48, 0x8d, 0x0d, 0x0b, 0x44, 0x60, 0x03, 0xbe, 0xb1, 0x00, 0x00, 0x00, 0x31, 0xc0,
    0xe8, 0xde, 0xe7, 0xd7, 0x00, 0x48, 0x8b, 0x05, 0x9f, 0xf8, 0x21, 0x04, 0x80, 0xb8, 0x78,
    0x15, 0x00, 0x00, 0x00, 0x74, 0x30, 0xc6, 0x80, 0x48, 0x15, 0x00, 0x00, 0x01, 0x83, 0x3d,
    0x48, 0x1c, 0x24, 0x04, 0x00, 0x7e, 0x09, 0x83, 0xb8, 0x90, 0x14, 0x00, 0x00, 0xff, 0x74,
    0x09, 0xc6, 0x80, 0x47, 0x15, 0x00, 0x00, 0x01, 0xeb, 0x07, 0xc6, 0x80, 0x47, 0x15, 0x00,
    0x00, 0x00, 0xc6, 0x80, 0x49, 0x15, 0x00, 0x00, 0x01, 0x5d, 0xc3};

// OnEvent_4000 queues the native death respawn through EventMan's request object.
constexpr u64 DeathRequestRva = 0x1381493;
constexpr std::array<u8, 11> DeathRequestSignature = {0x48, 0x8b, 0x40, 0x10, 0xc7, 0x40,
                                                      0x08, 0x04, 0x00, 0x00, 0x00};

constexpr u64 MainTickRva = 0x193fa34;
constexpr std::array<u8, 19> MainTickSignature = {0x8a, 0x98, 0x47, 0x15, 0x00, 0x00, 0x8a,
                                                  0x90, 0x48, 0x15, 0x00, 0x00, 0x44, 0x8b,
                                                  0xb0, 0x4c, 0x15, 0x00, 0x00};
constexpr u64 TitleTickRva = 0x19e02a0;
constexpr std::array<u8, 17> TitleTickSignature = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                                   0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                   0x53, 0x48, 0x83, 0xec, 0x68};

constexpr std::array<u8, 75> ReturnSignature = {
    0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x1d, 0x0b, 0xf6, 0x1a, 0x04, 0x48, 0x8b,
    0x03, 0x48, 0x85, 0xc0, 0x75, 0x24, 0x48, 0x8d, 0x3d, 0x5d, 0xf8, 0x5a, 0x03, 0x48, 0x8d,
    0x15, 0xaa, 0xf8, 0x5a, 0x03, 0x48, 0x8d, 0x0d, 0x91, 0xf6, 0x5a, 0x03, 0xbe, 0xb1, 0x00,
    0x00, 0x00, 0x31, 0xc0, 0xe8, 0x8a, 0x9a, 0xd2, 0x00, 0x48, 0x8b, 0x03, 0x48, 0x8b, 0x40,
    0x10, 0xc7, 0x40, 0x08, 0x02, 0x00, 0x00, 0x00, 0x48, 0x83, 0xc4, 0x08, 0x5b, 0x5d, 0xc3};

u8* base{};
std::atomic<bool> enabled{};
std::atomic<bool> death_pending{}, lantern_pending{};
std::unique_ptr<Xbyak::CodeGenerator> tick_code, title_code;
std::recursive_mutex io_mutex;
struct Handle {
    bool write;
    int slot;
    bool snapshot_read;
};
std::unordered_map<int, Handle> handles;
std::array<u64, 11> writes{}, reads{}, snapshot_reads{};
SnapshotReadLease read_lease;
u64 snapshot_read_before{};
Clock::time_point last_io{}, epoch{}, phase_since{}, started{}, stable_since{}, title_tick{},
    first{};
std::unique_ptr<Storage> storage;
int slot{-1}, respawn{-1};
bool has_snapshot{}, restored{}, clear_after_load{};
u8* replaced_death_request{};
u8* replaced_dead_player{};
bool test_abort_recovery{};
using ReturnTitleFn = void(PS4_SYSV_ABI*)();
ReturnTitleFn return_title{};
bool recovery_title_requested{}, recovery_ready{};
std::optional<Storage::Position> expected_position;
Barrier barrier;
u64 system_read_before{}, character_read_before{};
std::string message{"No soft save."};
std::string saved_at; // the selected snapshot's time (SavedAt), empty without one
enum class Phase {
    Idle,
    Saving,
    WaitReady,
    WaitTitle,
    Offline,
    WaitSystem,
    Continue,
    WaitPlayer,
    Release,
    RecoverLoad
};
Phase phase{Phase::Idle};
int test_delay{};
bool test_save_sent{}, test_return{}, test_return_sent{}, use_title_reload{};
Clock::time_point test_saved{};

double Seconds() {
    return std::chrono::duration<double>(Clock::now() - epoch).count();
}
u8* Global(u64 rva) {
    return Read<u8*>(base + rva);
}
u8* Player() {
    auto* world = Global(WorldChr);
    return Plausible(world) ? Read<u8*>(world + 0x60) : nullptr;
}
std::optional<Storage::Position> PlayerPosition(u8* player) {
    auto* ctrl = Plausible(player) ? Read<u8*>(player + 0x58) : nullptr;
    if (!Plausible(ctrl) || Read<u8*>(ctrl + 8) != player)
        return {};
    Storage::Position position{Read<float>(ctrl + 0x9c), Read<float>(ctrl + 0xac),
                               Read<float>(ctrl + 0xbc)};
    for (auto value : position)
        if (!std::isfinite(value))
            return {};
    return position;
}
void Position(const char* event, u8* player) {
    if (const auto p = PlayerPosition(player))
        LOG_INFO(Core, "VR soft save probe: POSITION {} t={:.3f} x={} y={} z={}", event, Seconds(),
                 (*p)[0], (*p)[1], (*p)[2]);
}
bool Flag(const char* name) {
    const auto* value = std::getenv(name);
    return value && std::string_view(value) == "1";
}
// Opt-in private-fixture regression. Uses the same AI registry/accessor as melee diagnostics.
std::optional<Storage::Position> enemy_test_anchor;
Clock::time_point enemy_test_resume{};
void EnemyProbe(const char* event, bool kill = false) {
    if (!Flag("VR_TEST_ENEMY_STATE") || !enemy_test_anchor)
        return;
    auto* managers = Global(0x553b0a0);
    if (!Plausible(managers))
        return;
    const auto count = std::clamp(Read<s32>(managers + 0x20), 0, 8);
    auto* array = Read<u8*>(managers + 0x28);
    if (!Plausible(array))
        return;
    int total = 0, nearby = 0, alive = 0, killed = 0;
    for (int m = 0; m < count; ++m) {
        auto* manager = array + m * 0xd8;
        auto* cursor = Read<u8*>(manager + 0x98);
        auto* end = Read<u8*>(manager + 0xa0);
        for (int k = 0; k < 256 && Plausible(cursor) && Plausible(end) && cursor < end;
             ++k, cursor += 8) {
            auto* entry = Read<u8*>(cursor);
            auto* think = Plausible(entry) ? Read<u8*>(entry + 0x8b0) : nullptr;
            auto* ai = Plausible(think) ? Read<u8*>(think) : nullptr;
            auto* accessor = Plausible(ai) ? Read<u8*>(ai) : nullptr;
            auto* vt = Plausible(accessor) ? Read<u8*>(accessor) : nullptr;
            auto* fn = Plausible(vt) ? Read<u8*>(vt + 0x30) : nullptr;
            if (!Plausible(fn) || fn < base || fn >= base + 0x538e2d0)
                continue;
            using GetChr = u8*(PS4_SYSV_ABI*)(void*);
            auto* chr = reinterpret_cast<GetChr>(fn)(accessor);
            if (!Plausible(chr) || Read<u8*>(chr) != base + 0x538ebc0)
                continue;
            const auto position = PlayerPosition(chr);
            auto* modules = Read<u8*>(chr + 0x3b0);
            auto* stats = Plausible(modules) ? Read<u8*>(modules + 0x20) : nullptr;
            if (!position || !Plausible(stats))
                continue;
            ++total;
            float distance = 0;
            for (size_t i = 0; i < 3; ++i) {
                const float delta = (*position)[i] - (*enemy_test_anchor)[i];
                distance += delta * delta;
            }
            if (distance > 25 * 25)
                continue;
            ++nearby;
            LOG_INFO(Core, "VR soft save probe: ENEMY {} npc={} model={} hp={} x={} y={} z={}",
                     event, Read<s32>(chr + 0x70), Read<s32>(chr + 0x74), Read<s32>(stats + 0xf8),
                     (*position)[0], (*position)[1], (*position)[2]);
            if (Read<s32>(stats + 0xf8) <= 0)
                continue;
            ++alive;
            if (kill) {
                const s32 zero = 0;
                std::memcpy(stats + 0xf8, &zero, sizeof(zero));
                ++killed;
            }
        }
    }
    LOG_INFO(Core, "VR soft save probe: ENEMIES {} total={} nearby={} alive={} killed={} t={:.3f}",
             event, total, nearby, alive, killed, Seconds());
}
const fs::path SaveFolder = "1000/savedata/CUSA00207/SPRJ0005";

// Before guest startup, an interrupted restore of the home the game was given is rolled back (its
// journal), so the guest never creates a blank save over it. Whether soft save turns on or not
// (2026-10-31 review: a crash between the swap's two renames leaves no save folder, so the
// launcher has no save to back up and names no backup, and the journal was never read).
void RecoverInterrupted() {
    std::error_code ec;
    const auto home = fs::canonical(EmulatorSettings.GetHomeDir(), ec);
    const auto archive = ec ? fs::path{} : Archive(home);
    if (!archive.empty() && fs::exists(archive / "restore_pending")) {
        try {
            Storage::Recover(home / SaveFolder, archive);
        } catch (const std::exception& e) {
            // Startup stops rather than let the game create a blank save over it (the crash
            // record says why; the pre-load backup stays in the home's archive).
            LOG_CRITICAL(Core, "VR soft save: an interrupted restore could not be recovered: {}",
                         e.what());
            throw;
        }
        LOG_WARNING(Core, "VR soft save: recovered an interrupted restore before game startup");
    }
}

// The portable trial: its run folder's user/home with the harness's prelaunch backup beside it.
bool BackedUpTrial(const fs::path& home) {
    std::error_code ec;
    const auto run = fs::canonical(fs::current_path(), ec);
    if (ec || run.parent_path().filename() != "runs" ||
        run.parent_path().parent_path().filename() != "dual-eye-perf")
        return false;
    if (home != run / "user/home")
        return false;
    fs::path backup = run / "prelaunch_home";
    if (const auto* selected = std::getenv("VR_SOFT_SAVE_BACKUP"))
        backup = fs::path(selected);
    backup = fs::canonical(backup, ec);
    if (ec || backup.parent_path() != run ||
        !backup.filename().string().starts_with("prelaunch_home"))
        return false;
    for (const auto* file : {"userdata0000", "userdata0010"}) {
        const auto rel = SaveFolder / file;
        if (fs::canonical(backup / rel, ec) != backup / rel || ec ||
            !fs::is_regular_file(backup / rel, ec) || ec)
            return false;
        if (fs::canonical(home / rel, ec) != home / rel || ec)
            return false;
    }
    return true;
}

// A launcher launch (2026-10-30, the user: soft save for every player): VR_SOFT_SAVE_LAUNCHER_BACKUP
// names the launcher's backup of this home made at this launch (launcher/core.cpp Prepare). It
// counts when it is a byte-for-byte copy of the save as it is now (soft_save_backup.h); then
// VR_SOFT_SAVE_LAUNCHER_VERIFIED records it, so the same session's graphics restart (which
// inherits this environment, the save changed since) keeps it if the backup still holds the save.
constexpr const char* LauncherBackupEnv = "VR_SOFT_SAVE_LAUNCHER_BACKUP";
constexpr const char* LauncherVerifiedEnv = "VR_SOFT_SAVE_LAUNCHER_VERIFIED";
bool BackedUpByLauncher(const fs::path& home) {
    const char* named = std::getenv(LauncherBackupEnv);
    if (named == nullptr || *named == '\0')
        return false;
    const auto backup = BackupFolder(home, fs::path(named), SaveFolder);
    if (backup.empty())
        return false;
    // The launcher's own words for it (an ASCII-safe path, as VR_INI's), inherited unchanged.
    const std::string token = named;
    if (const char* verified = std::getenv(LauncherVerifiedEnv);
        verified != nullptr && token == verified) {
        return BackupHolds(backup, SaveFolder); // this session's, verified at its first start
    }
    if (!BackupMatches(home, backup, SaveFolder))
        return false;
#ifdef _WIN32
    _putenv_s(LauncherVerifiedEnv, token.c_str()); // this process's getenv
    // The graphics restart's CreateProcessW passes the OS environment block on.
    SetEnvironmentVariableA(LauncherVerifiedEnv, token.c_str());
#else
    setenv(LauncherVerifiedEnv, token.c_str(), 1);
#endif
    return true;
}

// Never scan home directories or select the first save: the save home the game was given, with
// a verified backup of it (the trial's or the launcher's). How, for the log; empty: none.
const char* BackedUp() {
    std::error_code ec;
    const auto home = fs::canonical(EmulatorSettings.GetHomeDir(), ec);
    if (ec || Archive(home).empty())
        return nullptr;
    if (BackedUpTrial(home))
        return "the backed-up private trial";
    if (BackedUpByLauncher(home))
        return "the launcher's backup of this save";
    return nullptr;
}
bool Offline() {
    auto* game = Global(GameMan);
    auto* player = Player();
    return Plausible(game) && Read<u8>(game + 0x1590) == 0 &&
           (!Plausible(player) || Read<s32>(player + 0x78) == 0);
}
const char* Block(u8* player, bool require_alive = true, bool require_offline = true) {
    auto* game = Global(GameMan);
    auto* data = Global(GameData);
    if (!Plausible(game) || !Plausible(data) || !Plausible(Global(LuaEventMan)) ||
        !Plausible(player) || Player() != player)
        return "No active hunter.";
    if (require_offline && !Offline())
        return "Soft save is available offline only.";
    auto* modules = Read<u8*>(player + 0x3b0);
    auto* stats = Plausible(modules) ? Read<u8*>(modules + 0x20) : nullptr;
    if (!Plausible(stats) || (require_alive && Read<s32>(stats + 0xf8) <= 0))
        return "Waiting for the hunter to return.";
    auto* pgd = Read<u8*>(data + 8);
    if (!Plausible(pgd) || pgd != Read<u8*>(player + 0x3c0))
        return "The game is loading.";
    if (!Read<u8>(game + 0x1578) || Read<s32>(game + 0x1490) < 0 || Read<s32>(game + 0x1490) > 9)
        return "Saving is unavailable.";
    return nullptr;
}
void Publish() {
    Mailbox().Publish(Offline() && storage != nullptr, phase != Phase::Idle, has_snapshot, message,
                      saved_at);
}
std::tm LocalTime(std::chrono::system_clock::time_point t) {
    const std::time_t at = std::chrono::system_clock::to_time_t(t);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &at);
#else
    localtime_r(&at, &local);
#endif
    return local;
}
// A snapshot's time (from its name, so a soft save from an earlier session has one too).
std::string TimeOf(const fs::path& snapshot) {
    const auto made = Storage::MadeAt(snapshot);
    return made ? SavedAt(LocalTime(*made), LocalTime(std::chrono::system_clock::now()))
                : std::string{};
}
// has_snapshot and saved_at from the selected snapshot (Current validates it).
void ReadSelected() {
    const auto current = storage->Current();
    has_snapshot = !current.empty();
    saved_at = has_snapshot ? TimeOf(current) : std::string{};
}
// The Save tab's line while a soft save is kept: when it was made (2026-11-01: so you
// can tell whether there is one).
std::string Ready() {
    return saved_at.empty() ? std::string{"Soft save ready."} : "Soft save: " + saved_at;
}
void Next(Phase next, const std::string& text) {
    phase = next;
    phase_since = Clock::now();
    stable_since = {};
    message = text;
    Mailbox().Pad(next != Phase::Idle, next == Phase::Offline || next == Phase::Continue);
    Publish();
    LOG_INFO(Core, "VR soft save: phase={} {} t={:.3f}", static_cast<int>(next), text, Seconds());
}
// If a reload fails before a map/load operation starts, put the original death
// request back. Never overwrite a newer request or touch a replaced player instance.
void ResumeReplacedDeath() {
    auto* game = Global(GameMan);
    auto* event = Global(0x553b108);
    auto* request = Plausible(event) ? Read<u8*>(event + 0x10) : nullptr;
    auto* player = Player();
    if (replaced_death_request && request == replaced_death_request &&
        player == replaced_dead_player && Plausible(player) && Plausible(game) &&
        Read<u8>(game + 8) == 0 && Read<s32>(game + 0x1554) == 0 && Read<s32>(request + 8) == 0) {
        auto* modules = Read<u8*>(player + 0x3b0);
        auto* stats = Plausible(modules) ? Read<u8*>(modules + 0x20) : nullptr;
        const auto pending = Read<s32>(game + 0x154c);
        if (Plausible(stats) && Read<s32>(stats + 0xf8) <= 0 &&
            (pending == slot || pending == -1)) {
            const s32 no_load = -1, death = 4;
            std::memcpy(game + 0x154c, &no_load, sizeof(no_load));
            std::memcpy(request + 8, &death, sizeof(death));
            test_abort_recovery = Flag("VR_TEST_ABORT_RELOAD");
            LOG_INFO(Core, "VR soft save probe: DEATH_RESPAWN_RECOVERY t={:.3f}", Seconds());
        }
    }
    replaced_death_request = replaced_dead_player = nullptr;
}
void Abort(const std::string& reason) {
    std::scoped_lock lock(io_mutex);
    const bool cancelled = read_lease.Cancel();
    if (cancelled)
        ResumeReplacedDeath();
    message = reason;
    phase = cancelled ? Phase::Idle : Phase::RecoverLoad;
    recovery_title_requested = false;
    recovery_ready = false;
    stable_since = {};
    if (!cancelled)
        message += " Waiting for the game's load to finish.";
    Mailbox().Cancel();
    // No more confirm pulses or new loads; Back can dismiss native error dialogs.
    Mailbox().Pad(!cancelled, false, !cancelled);
    Publish();
    LOG_ERROR(Core, "VR soft save: aborted: {} (restored={})", reason, restored);
}
bool Quiet(Clock::time_point now, int milliseconds) {
    // Caller holds io_mutex. Open operations hold the same mutex BEFORE create/truncate.
    return handles.empty() && now - last_io >= std::chrono::milliseconds(milliseconds);
}
void ClearSnapshot() {
    std::scoped_lock lock(io_mutex);
    if (!read_lease.Cancel()) {
        clear_after_load = true;
        Abort("Soft save will be cleared after loading.");
        return;
    }
    ResumeReplacedDeath();
    clear_after_load = false;
    has_snapshot = false;
    saved_at.clear();
    death_pending.store(false, std::memory_order_release);
    Mailbox().Cancel();
    phase = Phase::Idle;
    if (storage) {
        try {
            storage->Clear();
        } catch (const std::exception& e) {
            LOG_ERROR(Core, "VR soft save: invalidation failed: {}", e.what());
            // Never advertise a snapshot that could not be invalidated for the next session.
            enabled.store(false, std::memory_order_release);
            Mailbox().Publish(false, false, false,
                              "Soft save disabled: could not clear its snapshot.");
            return;
        }
    }
    message = "Soft save cleared.";
    Publish();
    LOG_INFO(Core, "VR soft save: cleared snapshot");
}
// Keep host exception handlers in a native Windows ABI frame, outside the guest ABI wrapper.
[[gnu::noinline]] void Tick(bool title_callback) {
    if (!enabled.load(std::memory_order_acquire))
        return;
    const auto now = Clock::now();
    if (title_callback)
        title_tick = now;
    auto* game = Global(GameMan);
    if (!Plausible(game))
        return;
    auto* player = Player();
    const char* blocked = Block(player);
    const int active_slot = Read<s32>(game + 0x1490);
    if (phase == Phase::RecoverLoad) {
        std::scoped_lock lock(io_mutex);
        Mailbox().Pad(true, false, true);
        if (lantern_pending.exchange(false, std::memory_order_acq_rel))
            clear_after_load = true;
        auto* menu = Global(0x5562878);
        const bool title_ready = !Plausible(player) && Plausible(menu) &&
                                 Read<s32>(menu + 0x804) == 3 &&
                                 now - title_tick < std::chrono::milliseconds(100);
        // Recovery also works if going online was what aborted the restore. A healthy
        // old player alone is not completion: the protected character read must finish.
        const bool player_ready = Block(player, true, false) == nullptr &&
                                  (active_slot != slot ||
                                   snapshot_reads[slot] > snapshot_read_before);
        const bool ready = title_ready || player_ready;
        if (ready != recovery_ready) {
            recovery_ready = ready;
            stable_since = {};
        }
        auto* event = Global(0x553b108);
        auto* request = Plausible(event) ? Read<u8*>(event + 0x10) : nullptr;
        if (Read<u8>(game + 8) != 0 ||
            Read<s32>(game + 0x1554) != 0 || Read<s32>(game + 0x154c) != -1 ||
            Read<u8>(game + 0x1547) || Read<u8>(game + 0x1548) ||
            (Plausible(request) && Read<s32>(request + 8) != 0)) {
            stable_since = {};
            return;
        }
        if (stable_since == Clock::time_point{})
            stable_since = now;
        if (now - stable_since < std::chrono::seconds(3) || !Quiet(now, 500))
            return;
        if (!ready) {
            // A failed native read may leave the old hunter/error menu indefinitely.
            // Use the already signature-checked native return-to-title route once IO
            // and native requests settle. Keep the lease until that transition finishes.
            if (!recovery_title_requested && return_title && Plausible(request)) {
                recovery_title_requested = true;
                stable_since = {};
                return_title();
                message = "Soft-save recovery: returning to title. B/Back dismisses errors.";
                Publish();
            }
            return;
        }
        read_lease.Complete();
        replaced_death_request = replaced_dead_player = nullptr;
        phase = Phase::Idle;
        Mailbox().Pad(false);
        if (clear_after_load)
            ClearSnapshot();
        else {
            message = "Soft-save recovery finished. Snapshot retained.";
            Publish();
        }
        LOG_INFO(Core, "VR soft save: aborted load settled; snapshot read protection released");
        return;
    }
    if (phase != Phase::Idle) {
        Mailbox().Pad(true, phase == Phase::Offline || phase == Phase::Continue);
        if (!Offline()) {
            Abort("Soft save stopped: online mode became active.");
            return;
        }
        if (active_slot != slot && active_slot != -1) {
            Abort("Soft save stopped: character slot changed.");
            return;
        }
        const auto limit =
            phase == Phase::Saving ? std::chrono::seconds(20) : std::chrono::seconds(90);
        if (now - started > limit) {
            Abort("Soft save timed out. Recovery backups have been kept.");
            return;
        }
    }
    if (lantern_pending.exchange(false, std::memory_order_acq_rel)) {
        ClearSnapshot();
        return;
    }
    // Discard the old slot binding before any fallible initialization. In particular, a
    // failed constructor must not leave the previous slot's storage paired with the new slot.
    if (phase == Phase::Idle && active_slot >= 0 && active_slot <= 9 && active_slot != slot) {
        storage.reset();
        slot = -1;
        has_snapshot = false;
        saved_at.clear();
        death_pending.store(false, std::memory_order_release);
        Mailbox().Cancel();
        message = "No active soft save for this hunter.";
        Publish();
    }
    if (phase == Phase::Idle && !blocked) {
        if (!storage || active_slot != slot) {
            std::scoped_lock lock(io_mutex);
            if (!Quiet(now, 1000) || Read<s32>(game + 0x1554) != 0)
                return;
            try {
                const auto home = fs::canonical(EmulatorSettings.GetHomeDir());
                auto candidate =
                    std::make_unique<Storage>(home / SaveFolder, Archive(home), active_slot);
                storage = std::move(candidate);
                slot = active_slot;
                ReadSelected();
                message = has_snapshot ? Ready() : "No soft save.";
                respawn = Read<s32>(game + 0x1528);
            } catch (const std::exception& e) {
                Abort(std::string("Soft save unavailable: ") + e.what());
                return;
            }
        }
        const int current_respawn = Read<s32>(game + 0x1528);
        if (respawn != current_respawn) {
            respawn = current_respawn;
            ClearSnapshot();
            return;
        }
    }
    if (phase == Phase::Idle) {
        if (test_abort_recovery && !blocked) {
            Position("RECOVERED_LANTERN", player);
            test_abort_recovery = false;
        }
        if (Flag("VR_TEST_ENEMY_STATE") && !blocked && !enemy_test_anchor &&
            first != Clock::time_point{} && now - first >= std::chrono::seconds(5)) {
            enemy_test_anchor = PlayerPosition(player);
            EnemyProbe("KILL", true);
        }
        if (enemy_test_resume != Clock::time_point{} &&
            now - enemy_test_resume >= std::chrono::seconds(5)) {
            EnemyProbe("SETTLED");
            enemy_test_resume = {};
        }
        // Private, opt-in death regression: exercise native death/respawn instead of a banner.
        static const char* kill_file = std::getenv("VR_TEST_KILL_FILE");
        static Clock::time_point kill_poll{};
        if (kill_file && !blocked && now - kill_poll >= std::chrono::milliseconds(500)) {
            kill_poll = now;
            std::error_code ec;
            if (fs::exists(kill_file, ec) && !ec) {
                fs::remove(kill_file, ec);
                if (!ec) {
                    auto* modules = Read<u8*>(player + 0x3b0);
                    auto* stats = Read<u8*>(modules + 0x20);
                    const s32 zero = 0;
                    std::memcpy(stats + 0xf8, &zero, sizeof(zero));
                    LOG_INFO(Core, "VR soft save probe: REAL_DEATH health set to zero t={:.3f}",
                             Seconds());
                }
            }
        }
        Publish();
        if (death_pending.exchange(false, std::memory_order_acq_rel) && has_snapshot && Offline())
            Mailbox().Death();
        if (test_delay && !test_save_sent && !blocked) {
            if (first == Clock::time_point{})
                first = now;
            if (now - first >= std::chrono::seconds(test_delay)) {
                test_save_sent = true;
                Mailbox().Save();
            }
        } else if (blocked)
            first = {};
        if (test_return && !test_return_sent && has_snapshot && test_saved != Clock::time_point{} &&
            now - test_saved >= std::chrono::seconds(8)) {
            test_return_sent = true;
            Mailbox().Death();
            if (auto offer = Mailbox().TakeOffer())
                Mailbox().Choose(*offer, true);
        }
        const auto command = Mailbox().TakeCommand();
        if (command == Command::None)
            return;
        if (command == Command::Clear) {
            ClearSnapshot();
            return;
        }
        if (command == Command::Save) {
            if (blocked) {
                message = blocked;
                Publish();
                return;
            }
            std::scoped_lock lock(io_mutex);
            if (Read<s32>(game + 0x1554) != 0 || !handles.empty()) {
                message = "The game is saving. Try Soft save again in a moment.";
                Publish();
                return;
            }
            barrier = {writes[slot], writes[10]};
            started = now;
            restored = false;
            Next(Phase::Saving, "Saving...");
            LOG_INFO(Core, "VR soft save probe: REQUEST t={:.3f} thread='{}' slot={}", Seconds(),
                     Common::GetCurrentThreadName(), slot);
            using Fn = void(PS4_SYSV_ABI*)();
            reinterpret_cast<Fn>(base + SaveRequest)();
        } else if (has_snapshot && storage && (active_slot == slot || active_slot == -1) &&
                   Offline()) {
            started = now;
            restored = false;
            LOG_INFO(Core, "VR soft save probe: RESTORE_ACCEPTED t={:.3f}", Seconds());
            Next(Phase::WaitReady, "Returning to soft save...");
        }
        return;
    }
    if (phase == Phase::Saving) {
        if (blocked) {
            Abort(std::string("Soft save cancelled: ") + blocked);
            return;
        }
        std::scoped_lock lock(io_mutex);
        const auto quiet_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_io).count();
        if (!barrier.Ready(writes[slot], writes[10], !handles.empty(),
                           Read<s32>(game + 0x1554) != 0, quiet_ms))
            return;
        try {
            EnemyProbe("SNAPSHOT");
            Position("SNAPSHOT", player);
            const auto saved_position = PlayerPosition(player);
            if (!saved_position)
                throw std::runtime_error("Cannot read the saved position");
            const auto path = storage->Capture(saved_position);
            has_snapshot = true;
            saved_at = TimeOf(path);
            test_saved = now;
            LOG_INFO(Core, "VR soft save probe: SNAPSHOT t={:.3f} path={}", Seconds(),
                     path.string());
            Next(Phase::Release, Ready());
        } catch (const std::exception& e) {
            Abort(std::string("Soft save failed: ") + e.what());
        }
        return;
    }
    if (Flag("VR_TEST_LOAD_TRACE") &&
        (phase == Phase::WaitReady || phase == Phase::WaitPlayer || phase == Phase::Release)) {
        static Clock::time_point traced{};
        if (now - traced >= std::chrono::milliseconds(500)) {
            traced = now;
            auto* modules = Plausible(player) ? Read<u8*>(player + 0x3b0) : nullptr;
            auto* stats = Plausible(modules) ? Read<u8*>(modules + 0x20) : nullptr;
            auto* event = Global(0x553b108);
            auto* request = Plausible(event) ? Read<u8*>(event + 0x10) : nullptr;
            LOG_INFO(
                Core,
                "VR soft save probe: LOAD_STATE t={:.3f} hp={} map={} load={} busy={} event={}",
                Seconds(), Plausible(stats) ? Read<s32>(stats + 0xf8) : -1, Read<u8>(game + 8),
                Read<s32>(game + 0x154c), Read<s32>(game + 0x1554),
                Plausible(request) ? Read<s32>(request + 8) : -1);
            Position("LOADING", player);
        }
    }
    if (phase == Phase::WaitReady) {
        const bool early = !use_title_reload && !Flag("VR_SOFT_SAVE_WAIT_FOR_RESPAWN");
        if (early ? Block(player, false) != nullptr : blocked != nullptr) {
            stable_since = {};
            return;
        }
        if (stable_since == Clock::time_point{})
            stable_since = now;
        std::scoped_lock lock(io_mutex);
        // Do not submit a second transition if a native map request already started.
        if ((early && Read<u8>(game + 8) != 0) ||
            (!early && now - stable_since < std::chrono::seconds(1)) ||
            Read<s32>(game + 0x1554) != 0 || !Quiet(now, 1000))
            return;
        auto* event = Global(0x553b108);
        auto* request = Plausible(event) ? Read<u8*>(event + 0x10) : nullptr;
        if (!Plausible(request))
            return;
        const bool replace_death =
            early && blocked && Read<s32>(request + 8) == 4 && Read<u8>(game + 8) == 0;
        if (Read<s32>(request + 8) != 0 && !replace_death)
            return;
        // Check queued native work before hashing the entire snapshot. A busy request
        // may persist for several callbacks; validating it each time stalls Game:Main.
        if (!use_title_reload &&
            (Read<u8>(game + 0x1547) || Read<u8>(game + 0x1548) || Read<s32>(game + 0x154c) != -1))
            return;
        const auto snapshot = storage->Current();
        if (snapshot.empty()) {
            has_snapshot = false;
            saved_at.clear();
            Abort("The soft save is missing or damaged.");
            return;
        }
        Position("BEFORE_RETURN", player);
        if (!use_title_reload) {
            // Native load request. The main loop consumes +0x154c,
            // reads the character and deserializes it before requesting the map transition.
            try {
                expected_position = Storage::SnapshotPosition(snapshot);
                character_read_before = reads[slot];
                const auto backup = storage->Restore();
                restored = true;
                read_lease.Arm(snapshot, slot);
                snapshot_read_before = snapshot_reads[slot];
                LOG_INFO(Core, "VR soft save probe: RESTORED t={:.3f} backup={}", Seconds(),
                         backup.string());
                if (replace_death) {
                    const s32 idle = 0;
                    std::memcpy(request + 8, &idle, sizeof(idle));
                    replaced_death_request = request;
                    replaced_dead_player = player;
                    LOG_INFO(Core, "VR soft save probe: DEATH_RESPAWN_REPLACED t={:.3f}",
                             Seconds());
                }
                if (replace_death && Flag("VR_TEST_ABORT_RELOAD")) {
                    Abort("Injected failure after replacing the death request.");
                    return;
                }
                if (Flag("VR_TEST_LATE_AUTOSAVE")) {
                    using Fn = void(PS4_SYSV_ABI*)();
                    reinterpret_cast<Fn>(base + SaveRequest)();
                    LOG_INFO(Core, "VR soft save probe: LATE_AUTOSAVE requested after restore");
                }
                std::memcpy(game + 0x154c, &slot, sizeof(slot));
                read_lease.BeginLoad();
                Next(Phase::WaitPlayer, "Loading soft save directly...");
                LOG_INFO(Core, "VR soft save probe: DIRECT_LOAD native request submitted slot={}",
                         slot);
            } catch (const std::exception& e) {
                ReadSelected();
                Abort(std::string("Direct restore failed: ") + e.what());
            }
            return;
        }
        return_title();
        Next(Phase::WaitTitle, "Loading soft save...");
        LOG_INFO(Core, "VR soft save probe: RETURN_TITLE native request submitted");
        return;
    }
    if (phase == Phase::WaitTitle) {
        auto* menu = Global(0x5562878);
        const bool title_ready = !Plausible(player) && Plausible(menu) &&
                                 Read<s32>(menu + 0x804) == 3 &&
                                 now - title_tick < std::chrono::milliseconds(100);
        if (!title_ready || Read<s32>(game + 0x1554) != 0) {
            stable_since = {};
            return;
        }
        if (stable_since == Clock::time_point{})
            stable_since = now;
        std::scoped_lock lock(io_mutex);
        if (now - stable_since < std::chrono::seconds(2) || !Quiet(now, 2000))
            return;
        try {
            system_read_before = reads[10];
            character_read_before = reads[slot];
            const auto snapshot = storage->Current();
            expected_position = Storage::SnapshotPosition(snapshot);
            const auto backup = storage->Restore();
            restored = true;
            read_lease.Arm(snapshot, slot);
            snapshot_read_before = snapshot_reads[slot];
            LOG_INFO(Core, "VR soft save probe: RESTORED t={:.3f} backup={}", Seconds(),
                     backup.string());
            read_lease.BeginLoad(); // Next starts the confirm pulse which can initiate native IO.
            Next(Phase::Offline, "Loading soft save...");
        } catch (const std::exception& e) {
            ReadSelected();
            Abort(std::string("Restore failed: ") + e.what());
        }
        return;
    }
    const auto delivered = Mailbox().Delivered();
    const bool pulse_done =
        delivered != Clock::time_point{} && now - delivered >= std::chrono::milliseconds(200);
    if (phase == Phase::Offline && pulse_done)
        Next(Phase::WaitSystem, "Loading soft save...");
    else if (phase == Phase::WaitSystem) {
        std::scoped_lock lock(io_mutex);
        if (reads[10] > system_read_before && Read<s32>(game + 0x1554) == 0 &&
            now - phase_since >= std::chrono::milliseconds(3500) && Quiet(now, 500))
            Next(Phase::Continue, "Continuing soft save...");
        else if (now - phase_since > std::chrono::seconds(15))
            Abort("Automatic offline selection did not complete.");
    } else if (phase == Phase::Continue && pulse_done)
        Next(Phase::WaitPlayer, "Continuing soft save...");
    else if (phase == Phase::WaitPlayer) {
        std::scoped_lock lock(io_mutex);
        if (blocked || reads[slot] <= character_read_before ||
            snapshot_reads[slot] <= snapshot_read_before || Read<s32>(game + 0x1554) != 0 ||
            Read<u8>(game + 8) != 0 || (!use_title_reload && Read<s32>(game + 0x154c) != -1)) {
            stable_since = {};
            return;
        }
        if (stable_since == Clock::time_point{})
            stable_since = now;
        if (now - stable_since >= std::chrono::seconds(3) && Quiet(now, 500)) {
            const auto actual = PlayerPosition(player);
            if (!actual)
                return;
            if (expected_position) {
                float distance_squared = 0;
                for (size_t i = 0; i < actual->size(); ++i) {
                    const float delta = (*actual)[i] - (*expected_position)[i];
                    distance_squared += delta * delta;
                }
                if (distance_squared > 1.0f) {
                    Position("WRONG_POSITION", player);
                    Abort("Reload did not return to the saved position. Snapshot and backups "
                          "retained.");
                    return;
                }
            }
            EnemyProbe("RESUMED");
            if (Flag("VR_TEST_ENEMY_STATE"))
                enemy_test_resume = now;
            Position("RESUMED", player);
            replaced_death_request = replaced_dead_player = nullptr;
            read_lease.Complete();
            respawn = Read<s32>(game + 0x1528);
            Next(Phase::Release, "Soft save loaded.");
        }
    } else if (phase == Phase::Release && pulse_done) {
        phase = Phase::Idle;
        if (has_snapshot)
            message = Ready(); // "Soft save loaded." done: back to when it was made
        Mailbox().Pad(false);
        Publish();
    }
}
void PS4_SYSV_ABI MainTick(u8*, u8*) {
    Tick(false);
}
void PS4_SYSV_ABI TitleTick(u8*, u8*) {
    Tick(true);
}
} // namespace

void EbootLoaded(VAddr address, u64 size) {
    const char* backed_up = nullptr;
    if (size > 0x5580e40) {
        // Refuse startup on an unrecoverable transaction, before the guest can create a blank save.
        RecoverInterrupted();
        backed_up = BackedUp();
    }
    if (backed_up == nullptr) {
        LOG_INFO(Core, "VR soft save: unavailable: no verified backup of this save home");
        return;
    }
    base = reinterpret_cast<u8*>(address);
    if (std::memcmp(base + SaveRequest, SaveSignature.data(), SaveSignature.size()) ||
        std::memcmp(base + 0x138baf0, ReturnSignature.data(), ReturnSignature.size()) ||
        std::memcmp(base + DeathRequestRva, DeathRequestSignature.data(),
                    DeathRequestSignature.size()) ||
        std::memcmp(base + MainTickRva, MainTickSignature.data(), MainTickSignature.size()) ||
        std::memcmp(base + TitleTickRva, TitleTickSignature.data(), TitleTickSignature.size())) {
        LOG_WARNING(Core, "VR soft save: unsupported executable signatures");
        return;
    }
    if (const char* value = std::getenv("VR_TEST_SAVE_REQUEST")) {
        const auto* end = value + std::strlen(value);
        const auto parsed = std::from_chars(value, end, test_delay);
        if (parsed.ec != std::errc{} || parsed.ptr != end || test_delay < 1 || test_delay > 90) {
            LOG_WARNING(Core, "VR soft save: invalid test delay");
            return;
        }
    }
    test_return = Flag("VR_TEST_RETURN_TITLE");
    // Keep the previously verified title route available for private-trial diagnosis.
    use_title_reload = Flag("VR_SOFT_SAVE_TITLE_RELOAD");
    try {
        if (!Hook::MakeWritable(base + MainTickRva, MainTickSignature.size()) ||
            !Hook::MakeWritable(base + TitleTickRva, TitleTickSignature.size()))
            return;
        tick_code = std::make_unique<Xbyak::CodeGenerator>(1024);
        auto* stub = Hook::EmitHostCallStub(
            *tick_code, reinterpret_cast<void*>(&MainTick), MainTickSignature.data(),
            MainTickSignature.size(),
            reinterpret_cast<u64>(base + MainTickRva + MainTickSignature.size()), Xbyak::util::r13);
        Hook::WriteJump(base + MainTickRva, MainTickSignature.size(), stub);
        title_code = std::make_unique<Xbyak::CodeGenerator>(1024);
        stub = Hook::EmitHostCallStub(
            *title_code, reinterpret_cast<void*>(&TitleTick), TitleTickSignature.data(),
            TitleTickSignature.size(),
            reinterpret_cast<u64>(base + TitleTickRva + TitleTickSignature.size()),
            Xbyak::util::rdi);
        Hook::WriteJump(base + TitleTickRva, TitleTickSignature.size(), stub);
    } catch (const Xbyak::Error& e) {
        LOG_WARNING(Core, "VR soft save: hooks unavailable: {}", e.what());
        return;
    }
    epoch = Clock::now();
    return_title = reinterpret_cast<ReturnTitleFn>(base + 0x138baf0);
    enabled.store(true, std::memory_order_release);
    LOG_INFO(Core, "VR soft save: enabled for {} ({})", backed_up,
             EmulatorSettings.GetHomeDir().string());
}
void Death() {
    if (enabled.load(std::memory_order_acquire))
        death_pending.store(true, std::memory_order_release);
}
void LanternUsed() {
    if (enabled.load(std::memory_order_acquire))
        lantern_pending.store(true, std::memory_order_release);
}
void BossKilled() {
    if (enabled.load(std::memory_order_acquire)) {
        lantern_pending.store(true, std::memory_order_release);
        LOG_INFO(Core, "VR soft save: a boss was slain; the soft save is cleared");
    }
}
std::unique_lock<std::recursive_mutex> LockIo(std::string_view path, std::string_view other) {
    if (!enabled.load(std::memory_order_acquire) ||
        (!path.starts_with("/savedata") && !other.starts_with("/savedata")))
        return {};
    std::unique_lock lock(io_mutex);
    last_io = Clock::now();
    return lock;
}
std::filesystem::path ReadOverride(std::string_view path, bool read_only) {
    if (!enabled.load(std::memory_order_acquire) || !read_only || !path.starts_with("/savedata0/"))
        return {};
    std::scoped_lock lock(io_mutex);
    return read_lease.Resolve(path, read_only);
}
void NoteOpen(int fd, std::string_view path, bool write, bool snapshot_read) {
    if (!enabled.load(std::memory_order_acquire) || !path.starts_with("/savedata0/"))
        return;
    int file_slot = -1;
    if (path.starts_with("/savedata0/userdata") && path.size() == 23) {
        const auto parsed = std::from_chars(path.data() + 19, path.data() + path.size(), file_slot);
        if (parsed.ec != std::errc{} || parsed.ptr != path.data() + path.size() || file_slot < 0 ||
            file_slot > 10)
            file_slot = -1;
    }
    std::scoped_lock lock(io_mutex);
    last_io = Clock::now();
    handles[fd] = {write, file_slot, snapshot_read};
    if (snapshot_read)
        LOG_INFO(Core, "VR soft save probe: READ_SNAPSHOT fd={} path={} t={:.3f}", fd, path,
                 Seconds());
    LOG_INFO(Core, "VR soft save probe: OPEN fd={} write={} path={} t={:.3f} thread='{}'", fd,
             write, path, Seconds(), Common::GetCurrentThreadName());
}
void NoteClose(int fd, std::string_view path) {
    if (!enabled.load(std::memory_order_acquire) || !path.starts_with("/savedata0/"))
        return;
    std::scoped_lock lock(io_mutex);
    const auto it = handles.find(fd);
    const bool write = it != handles.end() && it->second.write;
    if (it != handles.end() && it->second.slot >= 0) {
        ++(write ? writes[it->second.slot] : reads[it->second.slot]);
        if (it->second.snapshot_read && !write)
            ++snapshot_reads[it->second.slot];
    }
    handles.erase(fd);
    last_io = Clock::now();
    LOG_INFO(Core, "VR soft save probe: CLOSE fd={} write={} path={} t={:.3f} thread='{}'", fd,
             write, path, Seconds(), Common::GetCurrentThreadName());
}
} // namespace VR::SoftSave
