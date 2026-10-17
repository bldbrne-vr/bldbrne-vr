// SPDX-License-Identifier: GPL-2.0-or-later

// The VR settings page (vr.ini vr_menu=1): the safe vr.ini writer (core/vr/vr_ini.h), the page's
// rows and navigation (vr_page.h), the pointer's ray (vr_page_geom.h), the BC7 decoder of the
// game's menu art (bc7_decode.h) and the CPU renderer's layout (vr_page_render.h).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/bc7_decode.h"
#include "core/vr/game_font.h"
#include "core/vr/vr_ini.h"
#include "core/vr/vr_page.h"
#include "core/vr/vr_page_art.h"
#include "core/vr/vr_page_draw.h"
#include "core/vr/vr_page_geom.h"
#include "core/vr/vr_page_render.h"

namespace {

using namespace VR;

std::string ReadFile(const std::filesystem::path& p) {
    std::string s;
    Ini::ReadText(p, s);
    return s;
}

std::filesystem::path TempDir(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / "vr_page_test" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

// ---- vr.ini edits ---------------------------------------------------------------------------

TEST(VrIni, KeepsCommentsUnknownKeysAndOrder) {
    const std::string before = "# Bloodborne VR settings\n"
                               "stereo=1\n"
                               "\n"
                               "# eye distance\n"
                               "ipd_mm=45\n"
                               "some_future_key = keep me\n"
                               "fp_eye_height_m=1.58\n";
    const std::string after = Ini::ApplyEdits(before, {{"ipd_mm", "47"}});
    EXPECT_EQ(after, "# Bloodborne VR settings\n"
                     "stereo=1\n"
                     "\n"
                     "# eye distance\n"
                     "ipd_mm=47\n"
                     "some_future_key = keep me\n"
                     "fp_eye_height_m=1.58\n");
}

TEST(VrIni, TrimmedKeysMatchAndAreNotDuplicated) {
    const std::string after = Ini::ApplyEdits("  snap_turn_deg =  30  \nlaser=0\n",
                                              {{"snap_turn_deg", "45"}});
    EXPECT_EQ(after, "snap_turn_deg=45\nlaser=0\n");
    EXPECT_EQ(Ini::ValueOf(after, "snap_turn_deg").value_or(""), "45");
    // A commented-out key is not the key.
    const std::string c = Ini::ApplyEdits("# laser=1\n", {{"laser", "1"}});
    EXPECT_EQ(c, "# laser=1\nlaser=1\n");
}

TEST(VrIni, DuplicatesAllGetTheValueAndRemoveTakesEveryLine) {
    const std::string before = "a=1\nb=2\na=3\n";
    EXPECT_EQ(Ini::ApplyEdits(before, {{"a", "9"}}), "a=9\nb=2\na=9\n");
    EXPECT_EQ(Ini::ApplyEdits(before, {{"a", std::nullopt}}), "b=2\n");
    // Removing a key the file does not have changes nothing.
    EXPECT_EQ(Ini::ApplyEdits(before, {{"zzz", std::nullopt}}), before);
}

TEST(VrIni, AppendsNewKeysBeforeTrailingBlankLinesAndKeepsCrlf) {
    EXPECT_EQ(Ini::ApplyEdits("a=1\n\n", {{"b", "2"}}), "a=1\nb=2\n\n");
    EXPECT_EQ(Ini::ApplyEdits("a=1\r\nb=2\r\n", {{"b", "3"}, {"c", "4"}}),
              "a=1\r\nb=3\r\nc=4\r\n");
    EXPECT_EQ(Ini::ApplyEdits("", {{"x", "1"}}), "x=1\n");
    EXPECT_EQ(Ini::ApplyEdits("a=1", {{"b", "2"}}), "a=1\nb=2\n");
    EXPECT_EQ(Ini::ApplyEdits("a=1", {{"a", "5"}}), "a=5"); // no newline added to a replace
    // The last edit of a key wins; a set after a remove keeps the key.
    EXPECT_EQ(Ini::ApplyEdits("a=1\n", {{"a", "2"}, {"a", std::nullopt}}), "");
    EXPECT_EQ(Ini::ApplyEdits("a=1\n", {{"a", std::nullopt}, {"a", "3"}}), "a=3\n");
}

TEST(VrIni, WriteFileIsAtomicAndOnlyTouchesItsKeys) {
    const auto dir = TempDir("write");
    const auto path = dir / "vr.ini";
    {
        std::ofstream out{path, std::ios::binary};
        out << "# mine\nipd_mm=45\nunknown=x\n";
    }
    std::string why;
    bool changed = false;
    ASSERT_TRUE(Ini::WriteFile(path, {{"laser", "1"}, {"ipd_mm", "46"}}, &why, &changed)) << why;
    EXPECT_TRUE(changed);
    EXPECT_EQ(ReadFile(path), "# mine\nipd_mm=46\nunknown=x\nlaser=1\n");
    EXPECT_FALSE(std::filesystem::exists(dir / "vr.ini.tmp"));
    EXPECT_FALSE(std::filesystem::exists(dir / "vr.ini.lock"));
    // Nothing to change: the file is not rewritten.
    const auto stamp = std::filesystem::last_write_time(path);
    ASSERT_TRUE(Ini::WriteFile(path, {{"laser", "1"}}, &why, &changed));
    EXPECT_FALSE(changed);
    EXPECT_EQ(std::filesystem::last_write_time(path), stamp);
    // A missing file is created.
    const auto fresh = dir / "new.ini";
    ASSERT_TRUE(Ini::WriteFile(fresh, {{"vr_menu", "1"}}, &why));
    EXPECT_EQ(ReadFile(fresh), "vr_menu=1\n");
}

TEST(VrIni, AnotherWritersLockIsWaitedForAndAStaleOneTakenOver) {
    const auto dir = TempDir("lock");
    const auto path = dir / "vr.ini";
    {
        std::ofstream out{path, std::ios::binary};
        out << "a=1\n";
    }
    const auto lock = dir / "vr.ini.lock";
    {
        std::ofstream out{lock};
        out << "other\n";
    }
    // A live lock (fresh): released after 300 ms by the other writer, then ours goes through.
    std::thread other([lock] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::error_code ec;
        std::filesystem::remove(lock, ec);
    });
    std::string why;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(Ini::WriteFile(path, {{"b", "2"}}, &why)) << why;
    other.join();
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(250));
    EXPECT_EQ(ReadFile(path), "a=1\nb=2\n");
    // A stale lock (a crashed writer's, older than LockStaleMs) is taken over at once.
    {
        std::ofstream out{lock};
        out << "crashed\n";
    }
    std::filesystem::last_write_time(lock, std::filesystem::file_time_type::clock::now() -
                                               std::chrono::seconds(60));
    EXPECT_TRUE(Ini::WriteFile(path, {{"c", "3"}}, &why)) << why;
    EXPECT_EQ(ReadFile(path), "a=1\nb=2\nc=3\n");
    EXPECT_FALSE(std::filesystem::exists(lock));
}

TEST(VrIni, WriterDebouncesAndFlushes) {
    const auto dir = TempDir("writer");
    const auto path = dir / "vr.ini";
    {
        std::ofstream out{path, std::ios::binary};
        out << "# keep\nhaptic_gain=1\n";
    }
    std::vector<std::string> lines;
    std::mutex m;
    {
        Ini::Writer w([path] { return path; },
                      [&](const std::string& l) {
                          std::scoped_lock lk{m};
                          lines.push_back(l);
                      },
                      200);
        for (int i = 1; i <= 5; ++i) {
            w.Set("haptic_gain", std::to_string(i));
        }
        w.Remove("nothing_here");
        // Not written before the debounce.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(ReadFile(path), "# keep\nhaptic_gain=1\n");
        ASSERT_TRUE(w.WaitIdle());
        EXPECT_EQ(ReadFile(path), "# keep\nhaptic_gain=5\n");
        EXPECT_EQ(w.GetStats().writes, 1u); // five edits, one write
        w.Set("laser", "1");
        w.Flush();
        ASSERT_TRUE(w.WaitIdle(1000));
        EXPECT_EQ(ReadFile(path), "# keep\nhaptic_gain=5\nlaser=1\n");
        w.Set("seated", "1"); // pending at the destructor: written then
    }
    EXPECT_EQ(ReadFile(path), "# keep\nhaptic_gain=5\nlaser=1\nseated=1\n");
    std::scoped_lock lk{m};
    ASSERT_FALSE(lines.empty());
    EXPECT_NE(lines.front().find("haptic_gain=5"), std::string::npos);
}

TEST(VrIni, EveryLineKeepsItsOwnLineEnding) {
    // A file edited by hand with a mix: only the changed line's text changes, every ending stays.
    const std::string before = "# mine\r\nipd_mm=45\nlaser=0\r\nlast=1";
    EXPECT_EQ(Ini::ApplyEdits(before, {{"ipd_mm", "46"}}), "# mine\r\nipd_mm=46\nlaser=0\r\nlast=1");
    // A new key takes the file's first ending; the unterminated last line gets one before it.
    EXPECT_EQ(Ini::ApplyEdits(before, {{"seated", "1"}}),
              "# mine\r\nipd_mm=45\nlaser=0\r\nlast=1\r\nseated=1\r\n");
    EXPECT_EQ(Ini::ApplyEdits(before, {{"laser", std::nullopt}}), "# mine\r\nipd_mm=45\nlast=1");
}

TEST(VrIni, TwoWritersAtOnceKeepEachOthersKeys) {
    // Two emulator instances (two writers on one file, no debounce): each sets its own keys 40
    // times while the other writes; every last value, the comment and the unknown key survive.
    const auto dir = TempDir("two");
    const auto path = dir / "vr.ini";
    {
        std::ofstream out{path, std::ios::binary};
        out << "# keep me\nunknown_key = 7\nipd_mm=45\n";
    }
    std::array<bool, 2> idle{};
    std::array<unsigned, 2> dropped{};
    const auto run = [&](const char* prefix, int n) {
        Ini::Writer w([path] { return path; }, {}, 0);
        for (int i = 0; i < 40; ++i) {
            for (int k = 0; k < 3; ++k) {
                w.Set(std::string{prefix} + std::to_string(k), std::to_string(i));
            }
            w.Flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        idle[n] = w.WaitIdle(20000);
        dropped[n] = w.GetStats().dropped;
    };
    std::thread a(run, "a", 0);
    std::thread b(run, "b", 1);
    a.join();
    b.join();
    EXPECT_TRUE(idle[0] && idle[1]);
    EXPECT_EQ(dropped[0] + dropped[1], 0u);
    const std::string after = ReadFile(path);
    EXPECT_EQ(after.rfind("# keep me\nunknown_key = 7\nipd_mm=45\n", 0), 0u) << after;
    for (const char* prefix : {"a", "b"}) {
        for (int k = 0; k < 3; ++k) {
            const std::string key = std::string{prefix} + std::to_string(k);
            EXPECT_EQ(Ini::ValueOf(after, key).value_or("missing"), "39") << key;
            // Once each: never a key appended twice.
            const auto first = after.find("\n" + key + "=");
            ASSERT_NE(first, std::string::npos);
            EXPECT_EQ(after.find("\n" + key + "=", first + 1), std::string::npos) << key;
        }
    }
    EXPECT_FALSE(std::filesystem::exists(dir / "vr.ini.lock"));
    EXPECT_FALSE(std::filesystem::exists(dir / "vr.ini.tmp"));
}

TEST(VrIni, ACrashedWritersLeftoversNeverReachTheFile) {
    // A writer killed after its .tmp was written but before the rename, holding its lock: the file
    // is still the old one; the next write takes the stale lock over, replaces the leftover .tmp
    // and keeps every line.
    const auto dir = TempDir("crashed");
    const auto path = dir / "vr.ini";
    {
        std::ofstream out{path, std::ios::binary};
        out << "# keep\nipd_mm=45\n";
    }
    {
        std::ofstream tmp{dir / "vr.ini.tmp", std::ios::binary};
        tmp << "ipd_mm=4"; // half written
        std::ofstream lock{dir / "vr.ini.lock"};
        lock << "crashed\n";
    }
    std::filesystem::last_write_time(dir / "vr.ini.lock",
                                     std::filesystem::file_time_type::clock::now() -
                                         std::chrono::seconds(30));
    EXPECT_EQ(ReadFile(path), "# keep\nipd_mm=45\n");
    std::string why;
    ASSERT_TRUE(Ini::WriteFile(path, {{"laser", "1"}}, &why)) << why;
    EXPECT_EQ(ReadFile(path), "# keep\nipd_mm=45\nlaser=1\n");
    EXPECT_FALSE(std::filesystem::exists(dir / "vr.ini.tmp"));
    EXPECT_FALSE(std::filesystem::exists(dir / "vr.ini.lock"));
}

TEST(VrIni, AFailedWriteIsTriedAgainAndKeepsItsOrder) {
    // Another writer holds the lock for longer than LockWaitMs (3 s): the first try fails, the
    // writer tries again RetryMs later, and an edit queued meanwhile still wins.
    const auto dir = TempDir("retry");
    const auto path = dir / "vr.ini";
    {
        std::ofstream out{path, std::ios::binary};
        out << "a=1\n";
    }
    const auto lock = dir / "vr.ini.lock";
    {
        std::ofstream out{lock};
        out << "slow writer\n";
    }
    std::vector<std::string> lines;
    std::mutex m;
    {
        Ini::Writer w([path] { return path; },
                      [&](const std::string& l) {
                          std::scoped_lock lk{m};
                          lines.push_back(l);
                      },
                      0, 200);
        w.Set("a", "2");
        w.Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(Ini::LockWaitMs + 400));
        w.Set("a", "3"); // queued while the first edit waits for its retry
        std::error_code ec;
        std::filesystem::remove(lock, ec); // the other writer is done
        ASSERT_TRUE(w.WaitIdle(Ini::LockWaitMs + 3000));
        EXPECT_EQ(w.GetStats().failures, 1u);
        EXPECT_EQ(w.GetStats().dropped, 0u);
    }
    EXPECT_EQ(ReadFile(path), "a=3\n");
    std::scoped_lock lk{m};
    ASSERT_GE(lines.size(), 2u);
    EXPECT_NE(lines.front().find("NOT written yet"), std::string::npos) << lines.front();
}

TEST(VrIni, AFolderNameOutsideTheCodePageIsWritten) {
    // A user folder with characters outside the ANSI code page (before: path::string() threw in
    // the writer thread, which ends the emulator).
    const auto dir = TempDir("unicode") / std::filesystem::path{u8"Παν 漢 \U0001F600"};
    std::filesystem::create_directories(dir);
    const auto path = dir / "vr.ini";
    {
        std::ofstream out{path, std::ios::binary};
        out << "# mine\nipd_mm=45\n";
    }
    std::string why;
    bool threw = false;
    bool ok = false;
    try {
        ok = Ini::WriteFile(path, {{"laser", "1"}}, &why);
    } catch (const std::exception&) {
        threw = true;
    }
    EXPECT_FALSE(threw);
    EXPECT_TRUE(ok) << why;
    EXPECT_EQ(ReadFile(path), "# mine\nipd_mm=45\nlaser=1\n");
    EXPECT_FALSE(std::filesystem::exists(Ini::WithSuffix(path, ".lock")));
}

#ifdef _WIN32
TEST(VrIni, AFileThatCannotBeReadIsNeverReplaced) {
    // vr.ini exists but this user may not read its data (an ACL, as a sync tool or a file copied
    // from another account may leave it): the write fails and the file keeps every line (before
    // the fix it became just the edited key).
    const auto dir = TempDir("unreadable");
    const auto path = dir / "vr.ini";
    {
        std::ofstream out{path, std::ios::binary};
        out << "# mine\nstereo=1\nipd_mm=45\n";
    }
    const std::string quoted = "\"" + path.string() + "\"";
    std::system(("icacls " + quoted + " /deny %USERNAME%:(RD) >nul 2>nul").c_str());
    std::string probe;
    const bool readable = Ini::ReadText(path, probe);
    std::string why;
    const bool ok = readable || Ini::WriteFile(path, {{"laser", "1"}}, &why);
    std::system(("icacls " + quoted + " /reset >nul 2>nul").c_str());
    if (readable) {
        return; // the ACL could not be set here: nothing to test
    }
    EXPECT_FALSE(ok);
    EXPECT_NE(why.find("cannot read"), std::string::npos) << why;
    EXPECT_EQ(ReadFile(path), "# mine\nstereo=1\nipd_mm=45\n");
}
#endif

// ---- The page's rows and navigation ----------------------------------------------------------

struct MapStore : Page::Store {
    std::map<std::string, std::string> v;
    std::map<std::string, std::string> defaults{{"melee_stamina_pct", "50"},
                                                {"physical_levers", "1"},
                                                {"lamp_travel", "1"},
                                                {"color_contrast", "0"},
                                                {"color_saturation", "0"},
                                                {"color_gamma", "0"},
                                                {"weapon_wall", "0"},
                                                {"blade_sounds", "1"},
                                                {"gun_wrist_reload", "1"},
                                                {"two_hand_guide", "1"},
                                                {"two_hand_swing_hum", "1"},
                                                {"shoulder_switch", "0"},
                                                {"health_color", "0"},
                                                {"stamina_color", "0"},
                                                {"health_color_strength", "0.5"},
                                                {"stamina_color_strength", "0.5"},
                                                {"holster_mode", "0"},
                                                {"holster_right", "1"},
                                                {"holster_left", "2"},
                                                {"holster_chest", "3"},
                                                {"holster_haptics", "1"},
                                                {"holster_height_cm", "0"},
                                                {"holster_width_cm", "0"},
                                                {"holster_forward_cm", "0"},
                                                {"holster_reach_cm", "10"},
                                                {"body_align", "1"},
                                                {"body_posture", "lean"},
                                                {"body_lean_side_deg", "20"},
                                                {"body_align_ms", "50"},
                                                {"body_comfort", "0"},
                                                {"height_base", "hunter"},
                                                {"height_adjust_cm", "0"},
                                                {"seated", "0"},
                                                {"swap_sticks", "0"},
                                                {"snap_turn_deg", "30"},
                                                {"smooth_turn_deg_per_s", "120"},
                                                {"haptic_gain", "1"},
                                                {"controls", "motion"},
                                                {"inject", "1"},
                                                {"laser", "0"},
                                                {"grip_adjust", "1"},
                                                {"cinematic", "1"},
                                                {"vr_grab_attacks", "0"},
                                                {"late_warp_motion", "1"},
                                                {"body_cutaway", "1"},
                                                {"body_mode", "hands_only"},
                                                {"ipd_mm", "0"},
                                                {"hud_vitals", "wrist"},
                                                {"vitals_heartbeat", "1"},
                                                {"vitals_edge", "1"},
                                                {"ui_counters", "look_down"},
                                                {"hit_vignette", "1"},
                                                {"ui_hud_scale_deg", "0.042"},
                                                {"player_hp_bonus_pct", "20"},
                                                {"dodge_iframes_extra", "2"},
                                                {"melee_kill_ragdoll", "1"},
                                                {"melee_ragdoll_impulse", "1"},
                                                {"aa_mode", "smaa"},
                                                {"ui_enemy_hp_bars", "1"},
                                                {"ui_boss_hp_bar", "0"},
                                                {"graphics_resolution", "Headset not connected"},
                                                {"graphics_scale", "0"},
                                                {"graphics_dlss", "0"},
                                                {"graphics_fsr4", "0"},
                                                {"graphics_mode", "1"},
                                                {"graphics_neural", "0"},
                                                {"graphics_neural_mode", "1"},
                                                {"graphics_intensity", "100"},
                                                {"graphics_detail", "85"},
                                                {"graphics_lighting", "60"}};
    std::vector<std::string> removed;
    std::string Get(const std::string& k) override {
        const auto it = v.find(k);
        return it != v.end() ? it->second : defaults[k];
    }
    void Set(const std::string& k, const std::string& value) override {
        v[k] = value;
    }
    void Remove(const std::string& k) override {
        v.erase(k);
        removed.push_back(k);
    }
};

TEST(VrPage, EveryRowShowsItsDefaultAndEveryKeyIsKnown) {
    MapStore store;
    for (const Page::Row& row : Page::Rows()) {
        const std::string text = Page::ValueText(row, store);
        EXPECT_NE(text, "Custom") << row.id;
        for (const char* key : row.keys) {
            if (key != nullptr) {
                EXPECT_TRUE(store.defaults.contains(key)) << key;
            }
        }
    }
    EXPECT_EQ(Page::ValueText(Page::Rows()[Page::RowIndex("height")], store), "As the hunter");
    EXPECT_EQ(Page::ValueText(Page::Rows()[Page::RowIndex("height_adjust")], store), "0 cm");
    EXPECT_EQ(Page::ValueText(Page::Rows()[Page::RowIndex("turning")], store), "Snap 30°");
    EXPECT_EQ(Page::ValueText(Page::Rows()[Page::RowIndex("vibration")], store), "7");
    EXPECT_EQ(Page::ValueText(Page::Rows()[Page::RowIndex("ipd")], store), "Headset");
    EXPECT_EQ(Page::ValueText(Page::Rows()[Page::RowIndex("hp_bonus")], store), "+20 %");
    EXPECT_EQ(Page::ValueText(Page::Rows()[Page::RowIndex("hud_vitals")], store), "Wrist gauge");
    EXPECT_EQ(Page::ValueText(Page::Rows()[Page::RowIndex("aa")], store), "SMAA"); // the default
    EXPECT_EQ(Page::ValueText(Page::Rows()[Page::RowIndex("kill_ragdoll")], store), "On");
    EXPECT_EQ(Page::ValueText(Page::Rows()[Page::RowIndex("ragdoll_push")], store), "Normal");
    // The tabs: General / Controls / Comfort / HUD / Assists / Graphics, at most five rows each.
    ASSERT_EQ(Page::Tabs().size(), 14u);
    EXPECT_STREQ(Page::Tabs()[4].name, "Assists");
    EXPECT_STREQ(Page::Tabs()[5].name, "Display");
    EXPECT_EQ(Page::Tabs()[5].rows[0], Page::RowIndex("aa"));
    for (const Page::Tab& t : Page::Tabs()) {
        for (const int r : t.rows) {
            EXPECT_LT(r, static_cast<int>(Page::Rows().size()));
        }
        EXPECT_NE(t.right.label, nullptr); // Defaults on every page
    }
}

// Comfort > Hunter's body (vr.ini body_mode): Full (no_head) / Arms only / Hands only (the default
// since 2026-10-08), wrapping, live; the tab's Defaults takes the line out. Body near your eyes
// says it is for the full body only (its arms were never faded).
TEST(VrPage, AttackStaminaChoicesAndDefault) {
    MapStore store;
    const auto& row = Page::Rows()[Page::RowIndex("attack_stamina")];
    ASSERT_EQ(row.options.size(), 4u);
    EXPECT_EQ(Page::ValueText(row, store), "50%");
    EXPECT_EQ(Page::Step(row, store, 1).text, "10%");
    EXPECT_EQ(store.Get("melee_stamina_pct"), "10");
    EXPECT_EQ(Page::Step(row, store, 1).text, "Original");
    EXPECT_EQ(store.Get("melee_stamina_pct"), "100");
    EXPECT_EQ(Page::Step(row, store, 1).text, "80%");
    EXPECT_EQ(store.Get("melee_stamina_pct"), "80");
    EXPECT_EQ(Page::Step(row, store, 1).text, "50%");
    EXPECT_EQ(store.Get("melee_stamina_pct"), "50");
    Page::Step(row, store, -1);
    store.Set("holster_mode", "2");
    Page::Defaults(Page::Tabs()[4], store);
    EXPECT_EQ(Page::ValueText(row, store), "50%");
    EXPECT_EQ(store.Get("holster_mode"), "2");
}

TEST(VrPage, HuntersBodyRowStepsThroughTheModes) {
    MapStore store;
    const Page::Row& body = Page::Rows()[Page::RowIndex("body_mode")];
    EXPECT_STREQ(body.label, "Hunter's body");
    EXPECT_EQ(Page::ValueText(body, store), "Hands only"); // the default
    auto c = Page::Step(body, store, 1);
    EXPECT_EQ(c.text, "Full");
    EXPECT_EQ(store.v["body_mode"], "no_head");
    c = Page::Step(body, store, 1);
    EXPECT_EQ(c.text, "Arms only");
    EXPECT_EQ(store.v["body_mode"], "arms_only");
    c = Page::Step(body, store, 1);
    EXPECT_EQ(c.text, "Hands only");
    EXPECT_EQ(store.v["body_mode"], "hands_only");
    c = Page::Step(body, store, -1);
    EXPECT_EQ(c.text, "Arms only");
    EXPECT_EQ(store.v["body_mode"], "arms_only");
    const Page::Row& cut = Page::Rows()[Page::RowIndex("cutaway")];
    EXPECT_STREQ(cut.help, "Full body only: fade his coat where it comes close to your eyes.");
    const Page::Tab& comfort = Page::Tabs()[2];
    EXPECT_STREQ(comfort.name, "Comfort");
    const auto d = Page::Defaults(comfort, store);
    EXPECT_NE(std::find(d.removed.begin(), d.removed.end(), "body_mode"), d.removed.end());
    EXPECT_EQ(Page::ValueText(body, store), "Hands only");
}

// Spatial AA is a saved preference, independent of the accepted temporal provider.
TEST(VrPage, AntiAliasingRowStepsThroughTheModes) {
    MapStore store;
    const auto& aa = Page::Rows()[Page::RowIndex("aa")];
    ASSERT_EQ(aa.options.size(), 3u);
    EXPECT_EQ(Page::ValueText(aa, store), "SMAA");
    EXPECT_EQ(Page::Step(aa, store, 1).text, "None");
    EXPECT_EQ(store.v["aa_mode"], "off");
    EXPECT_EQ(Page::Step(aa, store, 1).text, "Game FXAA");
    EXPECT_EQ(Page::Step(aa, store, 1).text, "SMAA");
    EXPECT_EQ(Page::Step(aa, store, -1).text, "Game FXAA");
    EXPECT_EQ(Page::Step(aa, store, -1).text, "None");
    EXPECT_EQ(Page::Step(aa, store, -1).text, "SMAA");
    for (const auto& option : aa.options) {
        EXPECT_NE(std::string_view(option.text), "TAA");
    }
    const auto d = Page::Defaults(Page::Tabs()[5], store);
    ASSERT_EQ(d.removed.size(), 3u);
    EXPECT_EQ(d.removed[0], "aa_mode");
    EXPECT_EQ(Page::ValueText(aa, store), "SMAA");
}
TEST(VrGraphics, AcceptedUpscalersDisableAaWithoutChangingItsPreference) {
    MapStore base;
    base.v["aa_mode"] = "smaa";
    Page::GraphicsDraftStore draft(base);
    const auto& aa = Page::Rows()[Page::RowIndex("aa")];
    for (const auto provider : {Graphics::Upscaler::Dlss, Graphics::Upscaler::Fsr4}) {
        Graphics::Config config;
        config.upscaler = provider;
        // Pending changes do not disable currently applied AA.
        draft.Set(provider == Graphics::Upscaler::Dlss ? "graphics_dlss" : "graphics_fsr4", "1");
        EXPECT_TRUE(Page::Enabled(aa, draft));
        ASSERT_TRUE(draft.Accept());
        EXPECT_FALSE(Page::Enabled(aa, draft));
        Page::Step(aa, draft, 1);
        Page::Defaults(Page::Tabs()[5], draft);
        EXPECT_EQ(base.v["aa_mode"], "smaa");
        EXPECT_EQ(Page::ValueText(aa, draft), "SMAA");
        // Back/discard must not turn an accepted provider off.
        draft.Set(provider == Graphics::Upscaler::Dlss ? "graphics_dlss" : "graphics_fsr4", "0");
        draft.Discard();
        EXPECT_FALSE(Page::Enabled(aa, draft));
        draft.Set(provider == Graphics::Upscaler::Dlss ? "graphics_dlss" : "graphics_fsr4", "0");
        ASSERT_TRUE(draft.Accept());
        EXPECT_TRUE(Page::Enabled(aa, draft));
        EXPECT_EQ(base.v["aa_mode"], "smaa");
    }
}

// Assists > Blade kills (vr.ini melee_kill_ragdoll / melee_ragdoll_impulse, core/vr/
// bloodborne_ragdoll.h, 2026-10-08): Instant ragdoll On (the default) / Off and Ragdoll push
// None / Light / Normal (the default) / Strong / Strongest = 0 / 0.5 / 1 / 2 / 3, both live, saved
// like every row, the tab's Defaults returning them to On / Normal.
TEST(VrPage, BladeKillRowsSwitchTheRagdollLive) {
    MapStore store;
    const Page::Tab& assists = Page::Tabs()[4];
    EXPECT_STREQ(assists.headers[0], "Combat");
    EXPECT_STREQ(assists.headers[1], "Attacks"); // above slot 2
    EXPECT_EQ(assists.rows[2], Page::RowIndex("kill_ragdoll"));
    EXPECT_EQ(assists.rows[3], Page::RowIndex("ragdoll_push"));
    EXPECT_EQ(assists.rows[4], Page::RowIndex("attack_stamina"));

    const Page::Row& rag = Page::Rows()[Page::RowIndex("kill_ragdoll")];
    EXPECT_STREQ(rag.label, "Instant ragdoll");
    EXPECT_STREQ(rag.help,
                 "An enemy your blade kills goes limp at once. Off: its death animation.");
    EXPECT_LE(std::string{rag.help}.size(), 70u); // 69 characters measured 844 of 900 stage px
    EXPECT_EQ(rag.widget, Page::Widget::Choice);
    EXPECT_STREQ(rag.keys[0], "melee_kill_ragdoll");
    EXPECT_TRUE(rag.keys[1] == nullptr);
    EXPECT_FALSE(rag.restart); // live: read at every hit
    EXPECT_FALSE(rag.placeholder);
    EXPECT_EQ(Page::ValueText(rag, store), "On"); // the default
    auto c = Page::Step(rag, store, 1);
    EXPECT_EQ(c.text, "Off");
    EXPECT_EQ(store.v["melee_kill_ragdoll"], "0");
    ASSERT_EQ(c.set.size(), 1u);
    EXPECT_EQ(c.set[0].first, "melee_kill_ragdoll");
    EXPECT_FALSE(c.restart);
    c = Page::Step(rag, store, 1); // wraps
    EXPECT_EQ(c.text, "On");
    EXPECT_EQ(store.v["melee_kill_ragdoll"], "1");
    c = Page::Step(rag, store, -1);
    EXPECT_EQ(c.text, "Off");
    // vr.ini's true / false read as On / Off.
    store.v["melee_kill_ragdoll"] = "true";
    EXPECT_EQ(Page::ValueText(rag, store), "On");
    store.v["melee_kill_ragdoll"] = "false";
    EXPECT_EQ(Page::ValueText(rag, store), "Off");

    const Page::Row& push = Page::Rows()[Page::RowIndex("ragdoll_push")];
    EXPECT_STREQ(push.label, "Ragdoll push");
    EXPECT_STREQ(push.help, "How hard your swing throws the body. None: it just falls.");
    EXPECT_STREQ(push.keys[0], "melee_ragdoll_impulse");
    EXPECT_FALSE(push.restart); // live: read at the push
    EXPECT_FALSE(push.placeholder);
    EXPECT_EQ(Page::ValueText(push, store), "Normal"); // the default
    const std::vector<std::pair<std::string, std::string>> forward{
        {"Strong", "2"}, {"Strongest", "3"}, {"None", "0"}, {"Light", "0.5"}, {"Normal", "1"}};
    for (const auto& [text, value] : forward) {
        c = Page::Step(push, store, 1);
        EXPECT_EQ(c.text, text);
        EXPECT_EQ(store.v["melee_ragdoll_impulse"], value);
    }
    c = Page::Step(push, store, -1);
    EXPECT_EQ(c.text, "Light");
    // The emulator's store reads the live float back (FormatIni): 0.5 and 1.0 match.
    store.v["melee_ragdoll_impulse"] = "1.0";
    EXPECT_EQ(Page::ValueText(push, store), "Normal");
    // A strength the page does not offer (vr.ini 1.5) reads Custom; ► goes to None, ◄ to
    // Strongest.
    store.v["melee_ragdoll_impulse"] = "1.5";
    EXPECT_EQ(Page::ValueText(push, store), "Custom");
    c = Page::Step(push, store, 1); // (the mini shim's EXPECT evaluates its arguments twice)
    EXPECT_EQ(c.text, "None");
    store.v["melee_ragdoll_impulse"] = "1.5";
    c = Page::Step(push, store, -1);
    EXPECT_EQ(c.text, "Strongest");

    // On the page: the grips to Assists, down to Instant ragdoll, the right trigger: Off, live
    // (the log says so), then the tab's Defaults: every Assists key back to its default.
    MapStore s2;
    std::vector<std::string> lines;
    Page::Model m{s2, [&](const std::string& l) { lines.push_back(l); }};
    m.Reset();
    m.TabStep(4); // General -> Assists
    ASSERT_EQ(m.GetState().tab, 4);
    m.Move(Page::Dir::Down);
    m.Move(Page::Dir::Down);
    EXPECT_EQ(m.GetState().focus, 2);
    m.Value(1);
    EXPECT_EQ(s2.v["melee_kill_ragdoll"], "0");
    m.Move(Page::Dir::Down);
    EXPECT_EQ(m.GetState().focus, 3);
    m.Value(-1); // Normal -> Light
    EXPECT_EQ(s2.v["melee_ragdoll_impulse"], "0.5");
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "VR page: kill_ragdoll = Off (melee_kill_ragdoll=0) [live]");
    EXPECT_EQ(lines[1], "VR page: ragdoll_push = Light (melee_ragdoll_impulse=0.5) [live]");
    const auto changes = m.TakeChanges();
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].row, "kill_ragdoll");
    EXPECT_FALSE(changes[0].restart);
    const auto d = Page::Defaults(assists, s2);
    const std::vector<std::string> expected{"player_hp_bonus_pct", "dodge_iframes_extra",
                                            "melee_kill_ragdoll", "melee_ragdoll_impulse",
                                            "melee_stamina_pct"};
    EXPECT_TRUE(d.removed == expected);
    EXPECT_EQ(Page::ValueText(rag, s2), "On");
    EXPECT_EQ(Page::ValueText(push, s2), "Normal");
}

// The two rows' round trip through vr.ini's text (Ini::ApplyEdits, what VR::SaveSetting /
// RemoveSetting write): the page's values are written as their keys' lines, a fresh read shows
// them again, and Defaults takes the lines out (the other keys stay).
TEST(VrPage, BladeKillRowsRoundTripThroughVrIni) {
    struct IniStore : Page::Store {
        std::string text;
        MapStore fallback; // the code defaults
        std::string Get(const std::string& k) override {
            return Ini::ValueOf(text, k).value_or(fallback.defaults[k]);
        }
        void Set(const std::string& k, const std::string& v) override {
            text = Ini::ApplyEdits(text, {{k, v}});
        }
        void Remove(const std::string& k) override {
            text = Ini::ApplyEdits(text, {{k, std::nullopt}});
        }
    };
    IniStore a;
    a.text = "aa_mode=smaa\r\nipd_mm=0\r\n"; // the user's vr.ini's style (CRLF)
    const Page::Row& rag = Page::Rows()[Page::RowIndex("kill_ragdoll")];
    const Page::Row& push = Page::Rows()[Page::RowIndex("ragdoll_push")];
    Page::Step(rag, a, 1);      // Off
    Page::Step(push, a, 1);     // Strong
    EXPECT_EQ(a.text, "aa_mode=smaa\r\nipd_mm=0\r\nmelee_kill_ragdoll=0\r\n"
                      "melee_ragdoll_impulse=2\r\n");
    IniStore b; // the next start reads the file
    b.text = a.text;
    EXPECT_EQ(Page::ValueText(rag, b), "Off");
    EXPECT_EQ(Page::ValueText(push, b), "Strong");
    Page::Step(rag, b, 1); // On again: the line changes in place
    EXPECT_EQ(b.text, "aa_mode=smaa\r\nipd_mm=0\r\nmelee_kill_ragdoll=1\r\n"
                      "melee_ragdoll_impulse=2\r\n");
    Page::Defaults(Page::Tabs()[4], b);
    EXPECT_EQ(b.text, "aa_mode=smaa\r\nipd_mm=0\r\n");
    EXPECT_EQ(Page::ValueText(rag, b), "On");
    EXPECT_EQ(Page::ValueText(push, b), "Normal");
}

// Graphics > Display > Show boss health bar (vr.ini ui_boss_hp_bar, 2026-10-15): Off (the default:
// hidden) / On (near the top of the view), live, below the enemy bars.
TEST(VrPage, BossHealthBarRowDefaultsOffAndSwitchesLive) {
    MapStore store;
    const Page::Row& boss = Page::Rows()[Page::RowIndex("boss_bar")];
    EXPECT_STREQ(boss.label, "Show boss health bar");
    EXPECT_LE(std::string{boss.help}.size(), 70u);
    EXPECT_STREQ(boss.keys[0], "ui_boss_hp_bar");
    EXPECT_FALSE(boss.restart);
    EXPECT_EQ(Page::ValueText(boss, store), "Off"); // the default
    auto c = Page::Step(boss, store, 1);
    EXPECT_EQ(c.text, "On");
    EXPECT_EQ(store.v["ui_boss_hp_bar"], "1");
    c = Page::Step(boss, store, 1); // wraps
    EXPECT_EQ(c.text, "Off");
    EXPECT_EQ(store.v["ui_boss_hp_bar"], "0");
}

// Graphics > Display > Show enemy health bars (vr.ini ui_enemy_hp_bars, core/vr/
// bloodborne_enemy_bars.h, 2026-10-08): On (the default) / Off, live (the game's enemy gauge reads
// it at every update), saved like every row, the tab's Defaults returning it to On. The HUD tab's
// five slots are taken: it is the Graphics tab's second group.
TEST(VrPage, EnemyHealthBarsRowSwitchesLive) {
    MapStore store;
    const Page::Tab& graphics = Page::Tabs()[5];
    EXPECT_STREQ(graphics.name, "Display");
    EXPECT_STREQ(graphics.headers[0], "Image");
    EXPECT_STREQ(graphics.headers[1], "Display"); // above slot 2
    EXPECT_EQ(graphics.rows[0], Page::RowIndex("aa"));
    EXPECT_EQ(graphics.rows[1], -1);
    EXPECT_EQ(graphics.rows[2], Page::RowIndex("enemy_bars"));
    EXPECT_EQ(graphics.rows[3], Page::RowIndex("boss_bar"));
    EXPECT_EQ(graphics.rows[4], -1);

    const Page::Row& bars = Page::Rows()[Page::RowIndex("enemy_bars")];
    EXPECT_STREQ(bars.label, "Show enemy health bars");
    EXPECT_STREQ(bars.help, "The bars over enemies you hit or lock on to.");
    EXPECT_LE(std::string{bars.help}.size(), 70u); // 69 characters measured 844 of 900 stage px
    EXPECT_EQ(bars.widget, Page::Widget::Choice);
    EXPECT_STREQ(bars.keys[0], "ui_enemy_hp_bars");
    EXPECT_TRUE(bars.keys[1] == nullptr);
    EXPECT_FALSE(bars.restart); // live: read at every gauge update
    EXPECT_FALSE(bars.placeholder);
    EXPECT_EQ(Page::ValueText(bars, store), "On"); // the default
    auto c = Page::Step(bars, store, 1);
    EXPECT_EQ(c.text, "Off");
    EXPECT_EQ(store.v["ui_enemy_hp_bars"], "0");
    ASSERT_EQ(c.set.size(), 1u);
    EXPECT_EQ(c.set[0].first, "ui_enemy_hp_bars");
    EXPECT_FALSE(c.restart);
    c = Page::Step(bars, store, 1); // wraps
    EXPECT_EQ(c.text, "On");
    EXPECT_EQ(store.v["ui_enemy_hp_bars"], "1");
    c = Page::Step(bars, store, -1);
    EXPECT_EQ(c.text, "Off");
    EXPECT_EQ(store.v["ui_enemy_hp_bars"], "0");
    // vr.ini's true / false read as On / Off.
    store.v["ui_enemy_hp_bars"] = "true";
    EXPECT_EQ(Page::ValueText(bars, store), "On");
    store.v["ui_enemy_hp_bars"] = "false";
    EXPECT_EQ(Page::ValueText(bars, store), "Off");

    // On the page: the left grip to Graphics, down past the empty slot to the row, the right
    // trigger: Off, live (the log says so); the left trigger: On again; the tab's Defaults: On.
    MapStore s2;
    std::vector<std::string> lines;
    Page::Model m{s2, [&](const std::string& l) { lines.push_back(l); }};
    m.Reset();
    m.SetTab(5); // Display
    ASSERT_EQ(m.GetState().tab, 5);
    EXPECT_EQ(m.GetState().focus, 0); // Anti-aliasing
    m.Move(Page::Dir::Down);
    EXPECT_EQ(m.GetState().focus, 2); // slot 1 is empty
    m.Value(1);
    EXPECT_EQ(s2.v["ui_enemy_hp_bars"], "0");
    m.Value(-1);
    EXPECT_EQ(s2.v["ui_enemy_hp_bars"], "1");
    m.Value(-1);
    EXPECT_EQ(s2.v["ui_enemy_hp_bars"], "0");
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "VR page: enemy_bars = Off (ui_enemy_hp_bars=0) [live]");
    EXPECT_EQ(lines[1], "VR page: enemy_bars = On (ui_enemy_hp_bars=1) [live]");
    const auto changes = m.TakeChanges();
    ASSERT_EQ(changes.size(), 3u);
    EXPECT_EQ(changes[0].row, "enemy_bars");
    EXPECT_FALSE(changes[0].restart);
    EXPECT_FALSE(s2.v.contains("aa_mode")); // Anti-aliasing untouched
    const auto d = Page::Defaults(graphics, s2);
    const std::vector<std::string> expected{"aa_mode", "ui_enemy_hp_bars", "ui_boss_hp_bar"};
    EXPECT_TRUE(d.removed == expected);
    EXPECT_EQ(Page::ValueText(bars, s2), "On");
    // Down from the row: the boss bar's, then the Defaults button; up: back to Anti-aliasing.
    m.Move(Page::Dir::Down);
    EXPECT_EQ(m.GetState().focus, 3);
    m.Move(Page::Dir::Down);
    EXPECT_EQ(m.GetState().focus, Page::FocusRightButton);
    m.Move(Page::Dir::Up);
    EXPECT_EQ(m.GetState().focus, 3);
    m.Move(Page::Dir::Up);
    EXPECT_EQ(m.GetState().focus, 2);
    m.Move(Page::Dir::Up);
    EXPECT_EQ(m.GetState().focus, 0);
}

// The row's round trip through vr.ini's text (Ini::ApplyEdits, what VR::SaveSetting /
// RemoveSetting write): Off is written as ui_enemy_hp_bars=0, a fresh read shows Off, On changes
// the line in place, and Defaults takes it out (the other keys stay).
TEST(VrPage, EnemyHealthBarsRowRoundTripsThroughVrIni) {
    struct IniStore : Page::Store {
        std::string text;
        MapStore fallback; // the code defaults
        std::string Get(const std::string& k) override {
            return Ini::ValueOf(text, k).value_or(fallback.defaults[k]);
        }
        void Set(const std::string& k, const std::string& v) override {
            text = Ini::ApplyEdits(text, {{k, v}});
        }
        void Remove(const std::string& k) override {
            text = Ini::ApplyEdits(text, {{k, std::nullopt}});
        }
    };
    IniStore a;
    a.text = "ipd_mm=0\r\nmelee_kill_ragdoll=1\r\n"; // the user's vr.ini's style (CRLF)
    const Page::Row& bars = Page::Rows()[Page::RowIndex("enemy_bars")];
    EXPECT_EQ(Page::ValueText(bars, a), "On"); // not in the file: the code default
    Page::Step(bars, a, 1);                    // Off
    EXPECT_EQ(a.text, "ipd_mm=0\r\nmelee_kill_ragdoll=1\r\nui_enemy_hp_bars=0\r\n");
    IniStore b; // the next start reads the file
    b.text = a.text;
    EXPECT_EQ(Page::ValueText(bars, b), "Off");
    Page::Step(bars, b, 1); // On again: the line changes in place
    EXPECT_EQ(b.text, "ipd_mm=0\r\nmelee_kill_ragdoll=1\r\nui_enemy_hp_bars=1\r\n");
    Page::Step(bars, b, 1); // Off
    Page::Defaults(Page::Tabs()[5], b);
    EXPECT_EQ(b.text, "ipd_mm=0\r\nmelee_kill_ragdoll=1\r\n");
    EXPECT_EQ(Page::ValueText(bars, b), "On");
}

TEST(VrPage, StepsChoicesSlidersAndNumbers) {
    MapStore store;
    const auto& rows = Page::Rows();
    const Page::Row& turning = rows[Page::RowIndex("turning")];
    auto c = Page::Step(turning, store, 1);
    EXPECT_EQ(c.text, "Snap 45°");
    EXPECT_EQ(store.v["snap_turn_deg"], "45");
    Page::Step(turning, store, 1); // 90
    c = Page::Step(turning, store, 1);
    EXPECT_EQ(c.text, "Smooth, slow");
    EXPECT_EQ(store.v["snap_turn_deg"], "0");
    EXPECT_EQ(store.v["smooth_turn_deg_per_s"], "90");
    EXPECT_EQ(Page::ValueText(turning, store), "Smooth, slow");
    // A value the page does not offer reads "Custom"; a step goes to the first / last option.
    store.v["snap_turn_deg"] = "20";
    EXPECT_EQ(Page::ValueText(turning, store), "Custom");
    const auto r1 = Page::Step(turning, store, 1);
    EXPECT_EQ(r1.text, "Snap 15°");
    // Choices wrap like the game's combo boxes.
    const Page::Row& stance = rows[Page::RowIndex("stance")];
    const auto r2 = Page::Step(stance, store, -1);
    EXPECT_EQ(r2.text, "Seated");
    const auto r3 = Page::Step(stance, store, 1);
    EXPECT_EQ(r3.text, "Standing");
    // The slider: 0..10, the gain = value / 7, clamped.
    const Page::Row& vib = rows[Page::RowIndex("vibration")];
    const auto r4 = Page::Step(vib, store, -1);
    EXPECT_EQ(r4.text, "6");
    EXPECT_EQ(store.v["haptic_gain"], "0.8571");
    for (int i = 0; i < 20; ++i) {
        Page::Step(vib, store, 1);
    }
    EXPECT_EQ(Page::CurrentNumber(vib, store), 10);
    const auto r5 = Page::SetFraction(vib, store, 0.0f);
    EXPECT_EQ(r5.text, "0");
    EXPECT_EQ(store.v["haptic_gain"], "0");
    const auto r6 = Page::SetFraction(vib, store, 0.72f);
    EXPECT_EQ(r6.text, "7");
    EXPECT_EQ(store.v["haptic_gain"], "1");
    // Numbers: Adjust height from 0 (a value like any other: no zero text) 1 cm a step, signed,
    // clamped to -10..+10; Height toggles its two choices.
    const Page::Row& adjust = rows[Page::RowIndex("height_adjust")];
    const auto r7 = Page::Step(adjust, store, 1);
    EXPECT_EQ(r7.text, "+1 cm");
    EXPECT_EQ(store.v["height_adjust_cm"], "1");
    Page::Step(adjust, store, -1);
    const auto r8 = Page::Step(adjust, store, -1);
    EXPECT_EQ(r8.text, "-1 cm");
    EXPECT_EQ(store.v["height_adjust_cm"], "-1");
    store.v["height_adjust_cm"] = "10";
    const auto r9 = Page::Step(adjust, store, 1);
    EXPECT_EQ(r9.text, "+10 cm");
    store.v["height_adjust_cm"] = "-10";
    const auto r9b = Page::Step(adjust, store, -1);
    EXPECT_EQ(r9b.text, "-10 cm");
    const Page::Row& height = rows[Page::RowIndex("height")];
    const auto r9c = Page::Step(height, store, 1);
    EXPECT_EQ(r9c.text, "As the player");
    EXPECT_EQ(store.v["height_base"], "player");
    const auto r9d = Page::Step(height, store, 1);
    EXPECT_EQ(r9d.text, "As the hunter");
    // Two keys at once (the low-health warning).
    const Page::Row& low = rows[Page::RowIndex("low_hp")];
    c = Page::Step(low, store, 1);
    EXPECT_EQ(c.text, "Off");
    EXPECT_EQ(store.v["vitals_heartbeat"], "0");
    EXPECT_EQ(store.v["vitals_edge"], "0");
    EXPECT_EQ(c.set.size(), 2u);
}

TEST(VrPage, NavigatesLikeTheGamesSettingsPages) {
    MapStore store;
    Page::Model m{store};
    m.Reset();
    EXPECT_EQ(m.GetState().tab, 0);
    EXPECT_EQ(m.GetState().focus, 0); // Height
    m.Move(Page::Dir::Up);            // wraps to the button row's first button
    EXPECT_EQ(m.GetState().focus, Page::FocusLeftButton);
    m.Move(Page::Dir::Right);
    EXPECT_EQ(m.GetState().focus, Page::FocusRightButton);
    m.Move(Page::Dir::Down); // wraps to the first row
    EXPECT_EQ(m.GetState().focus, 0);
    for (int i = 0; i < 4; ++i) {
        m.Move(Page::Dir::Down);
    }
    EXPECT_EQ(m.GetState().focus, 4); // Vibration
    m.Move(Page::Dir::Left);
    EXPECT_EQ(store.v["haptic_gain"], "0.8571");
    const auto r15 = m.TakeChanges();
    ASSERT_EQ(r15.size(), 1u);
    // Tabs: the grips (LG / RG) wrap; a new tab opens on its first row.
    m.TabStep(-1);
    EXPECT_EQ(m.GetState().tab, static_cast<int>(Page::Tabs().size()) - 1); // wraps to the last tab
    m.SetTab(5);
    EXPECT_EQ(m.GetState().tab, 5);
    EXPECT_EQ(m.GetState().focus, 0);
    // Graphics: Anti-aliasing, Show enemy health bars (slot 2, past the empty slot 1), Show boss
    // health bar, then the Defaults button.
    m.Move(Page::Dir::Down);
    EXPECT_EQ(m.GetState().focus, 2);
    m.Move(Page::Dir::Down);
    EXPECT_EQ(m.GetState().focus, 3);
    m.Move(Page::Dir::Down);
    EXPECT_EQ(m.GetState().focus, Page::FocusRightButton);
    m.Move(Page::Dir::Down); // wraps to the first row
    EXPECT_EQ(m.GetState().focus, 0);
    m.TabStep(-1);
    EXPECT_EQ(m.GetState().tab, 4);
    EXPECT_EQ(m.GetState().focus, 0);
    // Assists: five rows (Combat: two, Attacks: three), then the Defaults button (no left
    // button there).
    for (int i = 0; i < 5; ++i) {
        m.Move(Page::Dir::Down);
    }
    EXPECT_EQ(m.GetState().focus, Page::FocusRightButton);
    m.Move(Page::Dir::Left); // nothing on the left
    EXPECT_EQ(m.GetState().focus, Page::FocusRightButton);
    // B closes the page.
    const auto r10 = m.Back();
    EXPECT_EQ(r10, Page::Action::Close);
}

TEST(VrPage, DefaultsAsksFirstAndRemovesThePagesKeys) {
    MapStore store;
    Page::Model m{store};
    m.Reset();
    m.TabStep(3); // HUD
    m.Move(Page::Dir::Right);       // Health & stamina -> Classic
    EXPECT_EQ(store.v["hud_vitals"], "classic");
    for (int i = 0; i < 5; ++i) {
        m.Move(Page::Dir::Down);
    }
    EXPECT_EQ(m.GetState().focus, Page::FocusRightButton);
    const auto r11 = m.Confirm();
    EXPECT_EQ(r11, Page::Action::None);
    EXPECT_TRUE(m.GetState().confirm);
    EXPECT_EQ(m.GetState().confirm_yes, 0); // NO preselected, as in the game
    // B or NO: nothing changes.
    m.Back();
    EXPECT_FALSE(m.GetState().confirm);
    EXPECT_EQ(store.v["hud_vitals"], "classic");
    m.Confirm();
    m.Move(Page::Dir::Left); // YES
    EXPECT_EQ(m.GetState().confirm_yes, 1);
    m.Confirm();
    EXPECT_FALSE(m.GetState().confirm);
    EXPECT_FALSE(store.v.contains("hud_vitals"));
    // Every key of the HUD page's rows, and only those.
    const std::vector<std::string> expected{"hud_vitals",  "vitals_heartbeat", "vitals_edge",
                                            "ui_counters", "hit_vignette",     "ui_hud_scale_deg"};
    EXPECT_TRUE(store.removed == expected);
}

TEST(VrPage, PointerHoversAndClicks) {
    MapStore store;
    Page::Model m{store};
    m.Reset();
    m.SetHover({Page::Target::RowRight, 3}); // Turning's ►
    EXPECT_EQ(m.GetState().focus, 3);        // the selection follows the pointer
    m.Click();
    EXPECT_EQ(store.v["snap_turn_deg"], "45");
    m.SetHover({Page::Target::RowLeft, 3});
    m.Click();
    EXPECT_EQ(store.v["snap_turn_deg"], "30");
    m.SetHover({Page::Target::SliderBar, 4});
    m.Click(0.3f);
    EXPECT_EQ(store.v["haptic_gain"], "0.4286"); // 3 / 7
    m.Drag(1.0f);
    EXPECT_EQ(Page::CurrentNumber(Page::Rows()[Page::RowIndex("vibration")], store), 10);
    m.Release();
    m.Drag(0.0f); // no longer dragging
    EXPECT_EQ(Page::CurrentNumber(Page::Rows()[Page::RowIndex("vibration")], store), 10);
    m.SetHover({Page::Target::Tab, 2});
    m.Click();
    EXPECT_EQ(m.GetState().tab, 2);
    m.SetHover({Page::Target::TabNext, -1});
    m.Click();
    EXPECT_EQ(m.GetState().tab, 3);
    m.SetHover({Page::Target::Row, 0}); // a choice row's value: the next option
    m.Click();
    EXPECT_EQ(store.v["hud_vitals"], "classic");
    m.SetHover({Page::Target::ButtonRight, -1});
    const auto r12 = m.Click();
    EXPECT_EQ(r12, Page::Action::None);
    EXPECT_TRUE(m.GetState().confirm);
}

// The triggers change the highlighted value exactly as the stick pushed right / left (the right
// trigger: next, the left: previous); the grips switch tabs (right: next, left: previous).
TEST(VrPage, TriggersChangeTheValueGripsTheTab) {
    MapStore store;
    MapStore stick_store;
    Page::Model m{store};
    Page::Model stick{stick_store};
    m.Reset();
    stick.Reset();
    // Every row of every tab: the right trigger = the stick right, the left trigger = left.
    for (int tab = 0; tab < static_cast<int>(Page::Tabs().size()); ++tab) {
        for (int slot = 0; slot < Page::Slots; ++slot) {
            if (m.RowAt(slot) == nullptr) {
                continue;
            }
            m.SetHover({Page::Target::Row, slot});
            stick.SetHover({Page::Target::Row, slot});
            for (const int delta : {1, 1, -1, 1, -1, -1, -1}) {
                m.Value(delta);
                stick.Move(delta > 0 ? Page::Dir::Right : Page::Dir::Left);
                EXPECT_TRUE(store.v == stick_store.v) << tab << " " << slot << " " << delta;
            }
            m.SetHover({});
            stick.SetHover({});
        }
        m.TabStep(1); // the right grip
        stick.TabStep(1);
        EXPECT_EQ(m.GetState().tab, (tab + 1) % static_cast<int>(Page::Tabs().size()));
    }
    EXPECT_FALSE(store.v.empty());
    // Graphics > Anti-aliasing: RT from SMAA (the default) to None, LT back.
    m.SetTab(5); // Display anti-aliasing
    EXPECT_EQ(m.GetState().tab, 5);
    store.v.erase("aa_mode");
    m.Value(1);
    EXPECT_EQ(store.v["aa_mode"], "off");
    m.Value(-1);
    EXPECT_EQ(store.v["aa_mode"], "smaa");
    EXPECT_EQ(m.Value(0), Page::Action::None);
    // On the button row the triggers pick a button, in the Defaults question YES / NO (the stick).
    m.Move(Page::Dir::Down); // Show enemy health bars
    m.Move(Page::Dir::Down); // Show boss health bar
    m.Move(Page::Dir::Down);
    EXPECT_EQ(m.GetState().focus, Page::FocusRightButton);
    m.Confirm();
    ASSERT_TRUE(m.GetState().confirm);
    m.Value(-1);
    EXPECT_EQ(m.GetState().confirm_yes, 1);
    m.Value(1);
    EXPECT_EQ(m.GetState().confirm_yes, 0);
    m.Back();
}

// The triggers' and grips' pace: the stick's (a step at the press, then held a repeat after 0.4 s
// about 8 a second); the grips take the press only; a press the pointer used (a click) never
// steps, even when the ray leaves the page while it is held.
TEST(VrPage, ButtonsKeepTheSticksPace) {
    Page::PressRepeat p;
    EXPECT_FALSE(p.Step(false, 0.011f));
    EXPECT_TRUE(p.Step(true, 0.011f)); // the press
    int steps = 0;
    float t = 0.0f;
    for (; t < 0.395f; t += 0.011f) {
        steps += p.Step(true, 0.011f) ? 1 : 0;
    }
    EXPECT_EQ(steps, 0); // nothing before 0.4 s
    for (; t < 1.0f; t += 0.011f) {
        steps += p.Step(true, 0.011f) ? 1 : 0;
    }
    EXPECT_GE(steps, 5); // then about 8 a second: 0.6 s held -> 5 or 6
    EXPECT_LE(steps, 6);
    EXPECT_FALSE(p.Step(false, 0.011f)); // released
    EXPECT_TRUE(p.Step(true, 0.011f));   // a new press steps at once
    // Press only (the grips): held for a second, one step.
    Page::PressRepeat g;
    int grip_steps = 0;
    for (int i = 0; i < 90; ++i) {
        grip_steps += g.Step(true, 0.011f, false) ? 1 : 0;
    }
    EXPECT_EQ(grip_steps, 1);
    // A trigger that clicked (the ray on the page) steps nothing until it is released.
    Page::PressRepeat k;
    k.Block(true);
    int blocked_steps = 0;
    for (int i = 0; i < 90; ++i) {
        blocked_steps += k.Step(true, 0.011f) ? 1 : 0;
    }
    EXPECT_EQ(blocked_steps, 0);
    EXPECT_FALSE(k.Step(false, 0.011f));
    EXPECT_TRUE(k.Step(true, 0.011f));
}

// The tab badges are the grips' (LG / RG), the key guide's Change shows the D-pad and the
// triggers (LT RT).
TEST(VrPage, BadgesAndKeyGuideNameTheGripsAndTriggers) {
    const Page::Glyphs g{};
    EXPECT_EQ(g.prev, "LG");
    EXPECT_EQ(g.next, "RG");
    EXPECT_EQ(g.value_prev, "LT");
    EXPECT_EQ(g.value_next, "RT");
    MapStore store;
    Page::Model m{store};
    m.Reset();
    // Height (a choice): Change, Select, Return; Adjust height (a number) the same.
    for (int slot = 0; slot < 2; ++slot) {
        const Page::View v = Page::MakeView(m, g);
        ASSERT_EQ(v.keys.size(), 3u) << slot;
        EXPECT_EQ(v.keys[0].glyph, Page::KeyGlyph::DpadLRValues);
        EXPECT_EQ(v.keys[0].text, "Change");
        EXPECT_EQ(v.keys[1].glyph, Page::KeyGlyph::DpadUD);
        EXPECT_EQ(v.keys[2].glyph, Page::KeyGlyph::Back);
        m.Move(Page::Dir::Down);
    }
}

TEST(VrPage, ShowControlsIsAskedOfTheOwner) {
    MapStore store;
    Page::Model m{store};
    m.Reset();
    const auto r13 = m.Confirm();
    EXPECT_EQ(r13, Page::Action::None); // A on Height: nothing (left / right change it)
    m.Move(Page::Dir::Up);                          // the left button: Show VR controls
    const auto r14 = m.Confirm();
    EXPECT_EQ(r14, Page::Action::ShowControls);
}

// ---- The pointer's ray ---------------------------------------------------------------------

TEST(VrPageGeom, CylinderAndQuadHits) {
    Page::Surface s{};
    s.pos[1] = 1.6f;
    s.curved = true;
    s.radius = 1.8f;
    s.central_angle = 1.0f;
    s.aspect = 2.0f;
    float u = 0, v = 0;
    const float o[3]{0.0f, 1.6f, 0.0f};
    const float ahead[3]{0.0f, 0.0f, -1.0f};
    ASSERT_TRUE(Page::RayHit(s, o, ahead, u, v));
    EXPECT_NEAR(u, 0.5f, 1e-5f);
    EXPECT_NEAR(v, 0.5f, 1e-5f);
    // 0.25 rad to the right of the centre: a quarter of the way to the right edge.
    const float right[3]{std::sin(0.25f), 0.0f, -std::cos(0.25f)};
    ASSERT_TRUE(Page::RayHit(s, o, right, u, v));
    EXPECT_NEAR(u, 0.75f, 1e-4f);
    // Up: height = 1.8 * 1 / 2 = 0.9 m; 0.225 m up at 1.8 m is a quarter.
    const float up[3]{0.0f, 0.225f, -1.8f};
    ASSERT_TRUE(Page::RayHit(s, o, up, u, v));
    EXPECT_NEAR(v, 0.25f, 1e-4f);
    // Away from the page: no hit.
    const float back[3]{0.0f, 0.0f, 1.0f};
    EXPECT_FALSE(Page::RayHit(s, o, back, u, v));
    // A yawed cylinder (90 degrees left: facing -X) seen from a hand off its centre.
    const float h = std::sqrt(0.5f);
    s.rot[1] = h;
    s.rot[3] = h;
    const float hand[3]{0.0f, 1.6f, 0.3f};
    const float left[3]{-1.0f, 0.0f, 0.0f};
    ASSERT_TRUE(Page::RayHit(s, hand, left, u, v));
    EXPECT_NEAR(u, 0.5f - std::asin(0.3f / 1.8f), 1e-4f); // 0.3 m left of its axis
    // A quad 2 m ahead, 2 x 1 m.
    Page::Surface q{};
    q.curved = false;
    q.pos[2] = -2.0f;
    q.quad_w = 2.0f;
    q.quad_h = 1.0f;
    const float o0[3]{0.0f, 0.0f, 0.0f};
    const float to[3]{0.5f, 0.25f, -2.0f};
    ASSERT_TRUE(Page::RayHit(q, o0, to, u, v));
    EXPECT_NEAR(u, 0.75f, 1e-5f);
    EXPECT_NEAR(v, 0.25f, 1e-5f);
}

// The page always opens fully in view (the play test of 2026-10-09: after Auto height measures
// the canvas sat 12-15 deg up and the page's top ~36 deg up): its centre's pitch is kept within
// -15..+5 deg, and when it opens its centre is brought within 12 deg of where you look (the
// shorter way round, across +-180 too); within that it hangs exactly on the canvas.
TEST(VrPageGeom, PageOpensFullyInView) {
    constexpr float D = 3.14159265f / 180.0f;
    EXPECT_NEAR(Page::ClampPagePitch(1.0f * D), 1.0f * D, 1e-6f);
    EXPECT_NEAR(Page::ClampPagePitch(15.0f * D), 5.0f * D, 1e-6f);
    EXPECT_NEAR(Page::ClampPagePitch(11.9f * D), 5.0f * D, 1e-6f);
    EXPECT_NEAR(Page::ClampPagePitch(-30.0f * D), -15.0f * D, 1e-6f);
    // The page is 42.8 deg tall: at the limits its edges stay within 36.5 deg of the horizon.
    EXPECT_LT(Page::PageMaxPitchDeg + 21.4f, 36.5f);
    EXPECT_GT(Page::PageMinPitchDeg - 21.4f, -36.5f);
    // Yaw: within 12 deg of the head no change; beyond, brought to 12 deg.
    EXPECT_NEAR(Page::PageOpenYawOffset(10.0f * D, 0.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(Page::PageOpenYawOffset(-11.0f * D, 0.0f), 0.0f, 1e-6f);
    EXPECT_NEAR(10.0f * D + Page::PageOpenYawOffset(10.0f * D, -20.0f * D), -8.0f * D, 1e-5f);
    EXPECT_NEAR(40.0f * D + Page::PageOpenYawOffset(40.0f * D, 0.0f), 12.0f * D, 1e-5f);
    EXPECT_NEAR(-40.0f * D + Page::PageOpenYawOffset(-40.0f * D, 0.0f), -12.0f * D, 1e-5f);
    // Across the +-180 seam: the anchor at 175, the head at -175 (10 deg apart): no change; the
    // head at -160 (the anchor 25 deg to its right, the shorter way): turned 13 deg left, to -172.
    EXPECT_NEAR(Page::PageOpenYawOffset(175.0f * D, -175.0f * D), 0.0f, 1e-5f);
    EXPECT_NEAR(Page::PageOpenYawOffset(175.0f * D, -160.0f * D), 13.0f * D, 1e-5f);
}

// Opened looking far up or down, the page's centre comes into the view (the Quest 3 over Link:
// 44 deg up, 55 down; the page 42.8 deg tall). With the canvas where you look (-30..+15),
// nothing changes from -30 to +30 deg; at +45 a page at +5 would lose its bottom 6.4 deg, so it
// opens at +11.4 (its bottom on the view's lower edge); at -45 at -22.4 (its top on the upper
// edge). Every case, wherever the canvas is: the whole page inside the view it opened in.
TEST(VrPageGeom, PageOpensInTheViewLookingFarUpOrDown) {
    constexpr float D = 3.14159265f / 180.0f;
    const float up = 44.0f * D, down = 55.0f * D, half = 21.4f * D;
    for (const float head : {-60.0f, -45.0f, -30.0f, -10.0f, 0.0f, 10.0f, 30.0f, 38.0f, 45.0f,
                             60.0f}) {
        const auto view = Page::PageViewPitchRange(head * D, up, down, half);
        for (const float canvas : {-30.0f, -15.0f, 0.0f, 5.0f, 15.0f}) {
            const float p = Page::PagePitch(canvas * D, view);
            EXPECT_LE(p + half, head * D + up + 1e-5f) << head << " " << canvas;
            EXPECT_GE(p - half, head * D - down - 1e-5f) << head << " " << canvas;
        }
        if (head >= -30.0f && head <= 30.0f) {
            const float canvas = std::clamp(head, -30.0f, 15.0f) * D; // placed where you look
            EXPECT_NEAR(Page::PagePitch(canvas, view), Page::ClampPagePitch(canvas), 1e-6f)
                << head;
        }
    }
    EXPECT_NEAR(Page::PagePitch(15.0f * D, Page::PageViewPitchRange(45.0f * D, up, down, half)),
                11.4f * D, 1e-5f);
    EXPECT_NEAR(Page::PagePitch(-30.0f * D, Page::PageViewPitchRange(-45.0f * D, up, down, half)),
                -22.4f * D, 1e-5f);
    // No range (no head yet): only the -15..+5 limits.
    EXPECT_NEAR(Page::PagePitch(15.0f * D, Page::PitchRange{}), 5.0f * D, 1e-6f);
    // A view shorter than the page: centred in it.
    const auto narrow = Page::PageViewPitchRange(10.0f * D, 15.0f * D, 25.0f * D, half);
    EXPECT_NEAR(narrow.lo, narrow.hi, 1e-6f);
    EXPECT_NEAR(Page::PagePitch(0.0f, narrow), 5.0f * D, 1e-5f);
}

// The dim under the panel in VR (PanelDim): the game's 60% over the soft edges, full where the
// panel is solid; rising, never above 1, never below the game's.
TEST(VrPageDraw, PanelDimOpaqueCoreGameEdges) {
    using Page::Draw::PanelDim;
    const float dim = PageArt::ScreenDimAlpha;
    EXPECT_FLOAT_EQ(PanelDim(1.0f, dim), 1.0f);
    EXPECT_FLOAT_EQ(PanelDim(0.0f, dim), 0.0f);
    float last = 0.0f;
    for (int i = 0; i <= 100; ++i) {
        const float t = static_cast<float>(i) / 100.0f;
        const float v = PanelDim(t, dim);
        EXPECT_GE(v, last - 1e-7f) << t;
        EXPECT_LE(v, 1.0f) << t;
        EXPECT_GE(v, dim * t - 1e-7f) << t;
        if (t <= 0.6f) {
            EXPECT_NEAR(v, dim * t, 0.007f) << t;
        }
        last = v;
    }
    // With the panel's own 70% over it, its middle is fully opaque.
    EXPECT_FLOAT_EQ(1.0f - (1.0f - PanelDim(1.0f, dim)) * (1.0f - 0.6992f), 1.0f);
}

// ---- BC7 (blocks of our own, against Pillow's decoder) ----------------------------------------

TEST(VrPageBc7, DecodesHandBuiltBlocks) {
    // Each block packed by hand per the BC7 spec from endpoints and indices of our own choosing
    // (mode 3: two subsets, partition 0, p-bits; mode 4: rotation 1, the 3-bit index set for
    // colour; mode 5: separate 2-bit colour and alpha indices; mode 6: RGBA with 4-bit indices),
    // and all 16 texels as Pillow's BC7 decoder gives them (Image.frombytes(..., "bcn", 7)).
    struct Case {
        std::array<u8, 16> block;
        std::array<u8, 64> px; // RGBA, texels 0..15
    };
    // clang-format off
    const std::array<Case, 4> cases{{
        // mode 3
        {{0x08, 0x28, 0xc8, 0x7f, 0x00, 0x45, 0x0b, 0xf8, 0x7b, 0xa0, 0x40, 0x50, 0xc9, 0x1b, 0x69, 0x73},
         {21, 41, 61, 255, 80, 87, 93, 255, 84, 171, 85, 255, 0, 254, 64, 255, 80, 87, 93, 255, 200, 180, 160, 255, 255, 1, 129, 255, 84, 171, 85, 255, 21, 41, 61, 255, 80, 87, 93, 255, 0, 254, 64, 255, 84, 171, 85, 255, 80, 87, 93, 255, 141, 134, 128, 255, 0, 254, 64, 255, 255, 1, 129, 255}},
        // mode 4
        {{0xb0, 0x83, 0x47, 0xd3, 0x59, 0xc1, 0xcb, 0x37, 0xb0, 0x1b, 0x3b, 0x56, 0x99, 0xf8, 0xa2, 0x9a},
         {20, 127, 219, 53, 93, 49, 99, 231, 170, 140, 239, 24, 243, 102, 180, 111, 243, 75, 138, 173, 170, 114, 200, 82, 93, 62, 119, 202, 20, 87, 158, 144, 20, 140, 239, 24, 170, 49, 99, 231, 93, 102, 180, 111, 243, 127, 219, 53, 93, 114, 200, 82, 243, 75, 138, 173, 20, 62, 119, 202, 170, 87, 158, 144}},
        // mode 5
        {{0x20, 0x14, 0x37, 0x10, 0x85, 0x3f, 0x40, 0xc0, 0x77, 0x6c, 0xca, 0xd9, 0x9c, 0x36, 0xc9, 0x63},
         {99, 113, 167, 16, 162, 96, 88, 240, 221, 80, 14, 90, 40, 129, 241, 167, 162, 96, 88, 167, 99, 113, 167, 90, 221, 80, 14, 240, 40, 129, 241, 16, 99, 113, 167, 90, 99, 113, 167, 167, 162, 96, 88, 16, 221, 80, 14, 240, 40, 129, 241, 240, 221, 80, 14, 16, 162, 96, 88, 167, 99, 113, 167, 90}},
        // mode 6
        {{0xc0, 0x82, 0xde, 0x18, 0xc2, 0x27, 0xfe, 0xa8, 0xf6, 0x70, 0xc9, 0x51, 0x2e, 0x6b, 0x48, 0xad},
         {58, 126, 196, 219, 244, 66, 18, 80, 11, 141, 241, 255, 120, 106, 136, 173, 149, 96, 109, 151, 197, 81, 63, 116, 26, 136, 227, 244, 87, 116, 168, 198, 229, 71, 32, 91, 44, 130, 210, 230, 182, 86, 77, 126, 106, 111, 150, 184, 135, 101, 123, 162, 73, 121, 182, 209, 211, 77, 49, 105, 168, 91, 91, 137}},
    }};
    // clang-format on
    for (const Case& c : cases) {
        u8 out[64];
        Bc7::DecodeBlock(c.block.data(), out);
        for (int i = 0; i < 64; ++i) {
            EXPECT_EQ(out[i], c.px[static_cast<size_t>(i)])
                << "mode block " << int(c.block[0]) << " texel " << i / 4 << " ch " << i % 4;
        }
    }
    // A reserved block (no mode bit) is transparent black.
    std::array<u8, 16> zero{};
    u8 out[64];
    Bc7::DecodeBlock(zero.data(), out);
    for (u8 b : out) {
        EXPECT_EQ(b, 0);
    }
}

// ---- The renderer (synthetic art: every piece drawn, sizes, the glow only on the selection) ---

TEST(VrPageRender, DrawsThePageAndTheGlowLayer) {
    // Opaque mid-grey atlases stand in for the game's.
    std::vector<u8> grey(64 * 64 * 4, 128);
    const Page::Texture t{64, 64, grey.data()};
    // A font with one empty glyph per letter: text takes no pixels (the layout is still run).
    GameText::GameFont font;
    font.tag = {0x00, 0x00};
    font.table = 0;
    font.ascent = 18020;
    font.descent = 2460;
    for (u32 c = 32; c < 127; ++c) {
        font.glyph[c] = 0;
    }
    font.offsets = {0};
    font.advance = {10240};
    Page::Assets assets{t, t, t, t, &font};
    MapStore store;
    Page::Model m{store};
    m.Reset();
    Page::View view = Page::MakeView(m, Page::Glyphs{});
    ASSERT_EQ(view.tabs.size(), Page::Tabs().size());
    EXPECT_TRUE(view.rows[0].selected);
    EXPECT_EQ(view.rows[0].value, "As the hunter");
    EXPECT_EQ(view.rows[1].value, "0 cm");
    EXPECT_EQ(view.help, "As the hunter: his eye height. As the player: your real height (from the "
                         "headset's floor).");
    Page::Image page, glow;
    ASSERT_TRUE(Page::Render(assets, view, page, glow));
    EXPECT_EQ(page.width, Page::PageWidth);
    EXPECT_EQ(page.height, Page::PageHeight);
    ASSERT_EQ(glow.rgba.size(), page.rgba.size());
    const auto at = [](const Page::Image& im, float sx, float sy) {
        const int x = static_cast<int>((sx - Page::PageX0) * Page::PageScale);
        const int y = static_cast<int>((sy - Page::PageY0) * Page::PageScale);
        return &im.rgba[(static_cast<size_t>(y) * im.width + x) * 4];
    };
    // The panel is opaque in its middle (the world never shows through the text's backing: the
    // play test of 2026-10-09, "as if it was rendered weirdly on each eye"); its soft edges stay
    // soft. VR_PAGE_SEE_THROUGH=1: the game's 88% (12% of the world through).
    if (std::getenv("VR_PAGE_SEE_THROUGH") == nullptr) {
        EXPECT_EQ(at(page, 954.0f, 600.0f)[3], 255);
        EXPECT_EQ(at(page, 600.0f, 500.0f)[3], 255);
    }
    EXPECT_GT(at(page, 954.0f, 600.0f)[3], 150);
    // The glow: on the selected row (row 0), nothing on row 3.
    EXPECT_GT(at(glow, 900.0f, Page::RowTop(0) + 30.0f)[0], 0);
    EXPECT_EQ(at(glow, 900.0f, Page::RowTop(3) + 30.0f)[0], 0);
    // Hit tests: the right arrows of row 1 (Adjust height) and row 0 (Height).
    const auto boxes = Page::HitBoxes(view, &font);
    const float ax = VR::PageArt::RowX + 0.5f * (VR::PageArt::ComboArrowRight[0].x0 +
                                                 VR::PageArt::ComboArrowRight[0].x1);
    const auto hit = Page::HitTest(boxes, ax, Page::RowTop(1) + 31.0f);
    EXPECT_EQ(hit.target, Page::Target::RowRight);
    EXPECT_EQ(hit.index, 1);
    const auto hit0 = Page::HitTest(boxes, ax, Page::RowTop(0) + 31.0f);
    EXPECT_EQ(hit0.target, Page::Target::RowRight);
    EXPECT_EQ(hit0.index, 0);
    EXPECT_EQ(Page::HitTest(boxes, 100.0f, 100.0f).target, Page::Target::None);
    // No assets: nothing drawn.
    EXPECT_FALSE(Page::Render(Page::Assets{}, view, page, glow));
}

// The group headers (2026-10-09, the user: "show enemy health bars" still very close to its
// header above): each header sits the same distance above its first row's label on both groups
// (so on every tab), half a line more than the movie's 30.15 baseline to baseline, so a header's
// descenders ("Display") clear a long label's ascenders by about half a line (12.5..13.25 px on the
// real glyphs, 4.5..5.25 before); both headers sit the same distance under their divider and
// clear of it. The room comes from the rows, each group's moved together with its pitch kept, and
// every row's art and hit boxes still fit between their dividers.
TEST(VrPageRender, GroupHeadersSitHalfALineAboveTheirFirstRows) {
    constexpr float ascent = VR::Page::Draw::Ascent24;    // the menu font at 24 px: 18020 / 20480
    constexpr float descent = 24.0f * 2460.0f / 20480.0f; // ... 2460 / 20480 em
    const float gap0 = Page::LabelBaseline(0) - Page::HeaderBaseline(0);
    const float gap1 = Page::LabelBaseline(2) - Page::HeaderBaseline(1);
    EXPECT_NEAR(gap1, gap0, 0.01f);
    EXPECT_NEAR(gap0, 30.15f + 8.0f, 0.01f);
    // Between a header's descent and its label's ascent (metrics): over 13 px (was 6).
    for (int h = 0; h < 2; ++h) {
        const float room =
            (Page::LabelBaseline(h * 2) - ascent) - (Page::HeaderBaseline(h) + descent);
        EXPECT_GT(room, 13.0f) << h;
        EXPECT_LT(Page::HeaderBaseline(h), Page::LabelBaseline(h * 2)) << h;
    }
    // The dividers (the frame's rules: stage y 254.7..260.7 and 404.55..410.55) and the bottom
    // rule over the buttons (614.5): both headers the same distance under their rule's top, the
    // second where it was (435.97), their ascent clear of the rule.
    constexpr std::array<float, 2> rule{254.7f, 404.55f};
    constexpr float bottom_rule = 614.5f;
    EXPECT_NEAR(Page::HeaderBaseline(0) - rule[0], Page::HeaderBaseline(1) - rule[1], 0.01f);
    EXPECT_NEAR(Page::HeaderBaseline(1), 435.97f, 0.01f);
    for (int h = 0; h < 2; ++h) {
        EXPECT_GT(Page::HeaderBaseline(h) - ascent, rule[static_cast<size_t>(h)] + 6.0f) << h;
    }
    // Every row placed from RowTop (the label too), each group moved as one (the movie's pitch),
    // never up from the movie's place.
    for (int slot = 0; slot < Page::Slots; ++slot) {
        EXPECT_NEAR(Page::LabelBaseline(slot), Page::RowTop(slot) + 19.1f + ascent, 0.01f) << slot;
        EXPECT_GE(Page::RowTop(slot), VR::PageArt::RowY[static_cast<size_t>(slot)]) << slot;
    }
    EXPECT_NEAR(Page::RowTop(1) - Page::RowTop(0), VR::PageArt::RowY[1] - VR::PageArt::RowY[0],
                0.01f);
    EXPECT_NEAR(Page::RowTop(3) - Page::RowTop(2), VR::PageArt::RowY[3] - VR::PageArt::RowY[2],
                0.01f);
    EXPECT_NEAR(Page::RowTop(4) - Page::RowTop(3), VR::PageArt::RowY[4] - VR::PageArt::RowY[3],
                0.01f);
    // A row's lowest art (the combo's ornament, the slider's box) stays 6 px above the next rule:
    // the first group's last row above the second divider, the second's above the bottom rule.
    const float art_bottom =
        std::max(VR::PageArt::ComboOrnament[0].y1, VR::PageArt::SliderBox[3].y1);
    EXPECT_LT(Page::RowTop(1) + art_bottom, rule[1] - 6.0f);
    EXPECT_LT(Page::RowTop(4) + art_bottom, bottom_rule - 6.0f);
    // The pointer's boxes follow the rows (a slider and a choice in each group) and the last
    // row's end above the bottom rule, clear of the buttons' boxes.
    Page::View view;
    for (int slot = 0; slot < Page::Slots; ++slot) {
        auto& r = view.rows[static_cast<size_t>(slot)];
        r.present = true;
        r.widget = slot % 2 == 0 ? Page::Widget::Slider : Page::Widget::Choice;
    }
    view.buttons[0] = {true, "Show VR controls", false};
    view.buttons[1] = {true, "Defaults", false};
    float button_top = 1e9f;
    for (const auto& b : Page::HitBoxes(view, nullptr)) {
        if (b.hover.target == Page::Target::ButtonLeft ||
            b.hover.target == Page::Target::ButtonRight) {
            button_top = std::min(button_top, b.y0);
        }
    }
    for (const auto& b : Page::HitBoxes(view, nullptr)) {
        if (b.hover.index < 0 || b.hover.target == Page::Target::Tab) {
            continue;
        }
        EXPECT_GE(b.y0, Page::RowTop(b.hover.index)) << b.hover.index;
        EXPECT_LE(b.y1, Page::RowTop(b.hover.index) + 54.0f) << b.hover.index;
        if (b.hover.index == 4) {
            EXPECT_LT(b.y1, bottom_rule);
            EXPECT_LT(b.y1, button_top);
        }
    }
    const float x = VR::PageArt::RowX + 0.5f * (VR::PageArt::ComboArrowRight[0].x0 +
                                                VR::PageArt::ComboArrowRight[0].x1);
    EXPECT_EQ(Page::HitTest(Page::HitBoxes(view, nullptr), x, Page::RowTop(3) + 31.0f).index, 3);
}

// The page as the compositor blends it (linear light): over a black and over a white world the
// result is the game's (its encoded-domain blend), black dims let as much through as they do in
// the game, and additive light, empty space and opaque pixels keep their bytes.
TEST(VrPageRender, CompositorAlphaKeepsTheGamesLookOverBlackAndWhite) {
    const auto decode = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    const auto encode = [](float l) {
        l = std::clamp(l, 0.0f, 1.0f);
        return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
    };
    Page::Image im;
    // the panel (88% black), a glow-tinted dim, grey text's edge, white text, additive light,
    // empty, opaque
    const std::vector<std::array<u8, 4>> px{{0, 0, 0, 224},     {40, 30, 10, 200},
                                            {100, 100, 100, 128}, {255, 252, 228, 255},
                                            {90, 70, 30, 0},     {0, 0, 0, 0},
                                            {12, 34, 56, 255}};
    im.width = static_cast<int>(px.size());
    im.height = 1;
    for (const auto& p : px) {
        im.rgba.insert(im.rgba.end(), p.begin(), p.end());
    }
    Page::Image out = im;
    Page::ToCompositorAlpha(out);
    for (size_t i = 0; i < px.size(); ++i) {
        const u8* a = &im.rgba[i * 4];
        const u8* b = &out.rgba[i * 4];
        for (int c = 0; c < 3; ++c) {
            EXPECT_EQ(a[c], b[c]) << i; // the colour bytes stay
        }
        if (a[3] == 0 || a[3] == 255) {
            EXPECT_EQ(a[3], b[3]) << i;
            continue;
        }
        const float alpha = a[3] / 255.0f, fitted = b[3] / 255.0f;
        // Over white: the game min(k + 1 - a, 1) (encoded), the compositor G(k) + (1 - a').
        float game = 0.0f, comp = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float k = a[c] / 255.0f;
            const float w = std::array{0.2126f, 0.7152f, 0.0722f}[static_cast<size_t>(c)];
            game += w * decode(std::min(k + 1.0f - alpha, 1.0f));
            comp += w * (decode(k) + 1.0f - fitted);
        }
        EXPECT_NEAR(comp, game, 0.004f) << i;
    }
    // The panel: the game lets 12% of the world through (encoded), so does the compositor now;
    // with the game's alpha it let 39% through.
    const float world = 0.8f; // an encoded world value behind it
    const float before = encode((1.0f - im.rgba[3] / 255.0f) * decode(world));
    const float after = encode((1.0f - out.rgba[3] / 255.0f) * decode(world));
    EXPECT_NEAR(after, (1.0f - 224.0f / 255.0f) * world, 0.03f); // (8-bit alpha near 1)
    EXPECT_GT(before, 0.3f);
}

TEST(VrPage, BodyInventoryEditsPersistAndDefaultsResetOnlyThatPage) {
    MapStore store;
    Page::Model m{store};
    m.Reset();
    m.SetTab(9); // Inventory, after Graphics and DLSS 5
    m.Value(1);
    EXPECT_EQ(store.v["holster_mode"], "1");
    m.Move(Page::Dir::Down);
    m.Value(1);
    EXPECT_EQ(store.v["holster_haptics"], "0");
    m.Move(Page::Dir::Down);
    store.Set("body_comfort", "1");
    EXPECT_EQ(Page::RowIndex("holster_right"), -1);
    EXPECT_EQ(Page::RowIndex("holster_left"), -1);
    EXPECT_EQ(Page::RowIndex("holster_chest"), -1);
    // Pouch fit (holster_height_cm / _width_cm / _forward_cm / holster_reach_cm) is vr.ini only:
    // its page showed 0 for every value and its Defaults left the live values alone.
    for (const char* id : {"holster_height", "holster_width", "holster_forward", "holster_radius"})
        EXPECT_EQ(Page::RowIndex(id), -1) << id;
    for (const Page::Tab& t : Page::Tabs())
        EXPECT_STRNE(t.name, "Pouch fit");
    EXPECT_EQ(m.GetState().focus, Page::FocusRightButton);
    m.Confirm();
    m.Move(Page::Dir::Left);
    m.Confirm();
    EXPECT_EQ(store.Get("holster_mode"), "0");
    EXPECT_EQ(store.Get("holster_right"), "1");
    EXPECT_EQ(store.Get("body_comfort"), "1");
}
TEST(VrPageRender, AddedTabsKeepReadableWindowAndMatchingHitTargets) {
    GameText::GameFont font;
    font.tag = {0, 0}; font.offsets = {0}; font.advance = {10240};
    font.ascent = 18020; font.descent = 2460;
    for (u32 c = 32; c < 127; ++c) font.glyph[c] = 0;
    MapStore store;
    Page::Model m{store};
    m.Reset();
    for (int page = 0; page < static_cast<int>(Page::Tabs().size()); ++page) {
        auto view = Page::MakeView(m, {});
        const auto layout = Page::Draw::LayoutTabs(view.tabs, view.glyphs, &font, view.tab);
        EXPECT_GT(layout.width[page], 0);
        int count = 0;
        for (float w : layout.width)
            if (w > 0)
                ++count;
        EXPECT_EQ(count, 5);
        const auto boxes = Page::HitBoxes(view, &font);
        for (int j = 0; j < static_cast<int>(view.tabs.size()); ++j)
            if (layout.width[j] > 0) {
                const auto hit = Page::HitTest(boxes, layout.x[j] + layout.width[j] * .5f,
                                               Page::Draw::TitleBaseline - 8);
                EXPECT_EQ(hit.target, Page::Target::Tab);
                EXPECT_EQ(hit.index, j);
            }
        m.TabStep(1);
    }
}
TEST(VrPage, ShoulderAndVitalFeedbackAreIndependentAndResetLocally) {
    MapStore store;
    const auto& rows = Page::Rows();
    for (const char* key : {"shoulder_switch", "health_color", "stamina_color"}) {
        EXPECT_EQ(store.Get(key), "0");
        Page::Step(rows[Page::RowIndex(key)], store, 1);
        EXPECT_EQ(store.Get(key), "1");
    }
    Page::Step(rows[Page::RowIndex("health_color")], store, 1);
    EXPECT_EQ(store.Get("health_color"), "0");
    EXPECT_EQ(store.Get("stamina_color"), "1");
    EXPECT_EQ(store.Get("shoulder_switch"), "1");
    Page::SetFraction(rows[Page::RowIndex("health_color_strength")], store, .7f);
    EXPECT_NEAR(std::stof(store.Get("health_color_strength")), .7f, .0001f);
    EXPECT_NEAR(std::stof(store.Get("stamina_color_strength")), .5f, .0001f);
    store.Set("holster_mode", "2");
    ASSERT_STREQ(Page::Tabs()[10].name, "Feedback");
    Page::Defaults(Page::Tabs()[10], store);
    EXPECT_EQ(store.Get("shoulder_switch"), "0");
    EXPECT_EQ(store.Get("stamina_color"), "0");
    EXPECT_NEAR(std::stof(store.Get("health_color_strength")), .5f, .0001f);
    EXPECT_EQ(store.Get("holster_mode"), "2");
}
// Weapons > Solid weapons (vr.ini weapon_wall, 2026-10-12: the user could not find it on the page;
// off by default).
TEST(VrPage, SolidWeaponsSwitchesOnAndDefaultsBackOff) {
    MapStore store;
    const Page::Tab& weapons = Page::Tabs()[11];
    EXPECT_STREQ(weapons.name, "Weapons");
    ASSERT_EQ(weapons.rows[0], Page::RowIndex("weapon_wall"));
    const auto& row = Page::Rows()[Page::RowIndex("weapon_wall")];
    EXPECT_LE(std::string{row.help}.size(), 70u);
    EXPECT_EQ(Page::ValueText(row, store), "Off");
    EXPECT_EQ(Page::Step(row, store, 1).text, "On");
    EXPECT_EQ(store.Get("weapon_wall"), "1");
    // Weapons only (2026-10-13): solid against each other, through the walls.
    EXPECT_EQ(Page::Step(row, store, 1).text, "Weapons only");
    EXPECT_EQ(store.Get("weapon_wall"), "2");
    Page::Defaults(weapons, store);
    EXPECT_EQ(Page::ValueText(row, store), "Off");
}
} // namespace

TEST(VrGraphics, DraftIsIsolatedExclusiveAndAcceptedAsOneRecord) {
    MapStore base;
    Page::Model m(base);
    m.SetTab(6);
    EXPECT_EQ(m.GetState().focus, 1); // Resolution is informational.
    m.Value(-1);
    EXPECT_EQ(m.GetStore().Get("graphics_scale"), "-10");
    m.SetHover({Page::Target::RowRight, 2});
    m.Click();
    EXPECT_EQ(m.GetStore().Get("graphics_dlss"), "1");
    EXPECT_EQ(m.GetStore().Get("graphics_mode"), "1"); // Balanced default.
    m.SetHover({Page::Target::RowLeft, 4});
    m.Click(); // Balanced to Performance.
    EXPECT_EQ(m.GetStore().Get("graphics_mode"), "0");
    m.SetHover({Page::Target::RowRight, 3});
    m.Click();
    EXPECT_EQ(m.GetStore().Get("graphics_fsr4"), "1");
    EXPECT_EQ(m.GetStore().Get("graphics_dlss"), "0");
    EXPECT_EQ(m.GetStore().Get("graphics_mode"), "1"); // Independent Balanced FSR default.
    m.SetHover({Page::Target::RowRight, 2});
    m.Click();
    EXPECT_EQ(m.GetStore().Get("graphics_mode"), "0"); // DLSS remembers Performance.
    EXPECT_TRUE(base.v.empty());
    m.SetTab(7);
    m.Value(1);
    m.SetHover({Page::Target::RowRight, 1});
    m.Click();
    EXPECT_EQ(m.GetStore().Get("graphics_neural_mode"), "0");
    EXPECT_EQ(m.GetStore().Get("graphics_intensity"), "100"); // DLSS 5 Enhancement default
    EXPECT_TRUE(base.v.empty());
    m.SetHover({Page::Target::ButtonRight, -1});
    m.Click();
    ASSERT_EQ(base.v.size(), 1u);
    const auto c = Graphics::Parse(base.v.at("graphics_config"));
    ASSERT_TRUE(c);
    EXPECT_EQ(c->scale_percent, -10);
    EXPECT_EQ(c->upscaler, Graphics::Upscaler::Dlss);
    EXPECT_EQ(c->dlss, Graphics::Profile::Performance);
    EXPECT_TRUE(c->dlss5);
    EXPECT_FALSE(m.GraphicsPending());
    Page::Model reopened(base);
    reopened.SetTab(6);
    EXPECT_EQ(reopened.GetStore().Get("graphics_scale"), "-10");
    EXPECT_EQ(reopened.GetStore().Get("graphics_mode"), "0"); // Saved DLSS choice survives reopen.
    EXPECT_EQ(Graphics::Parse(base.v.at("graphics_config"))->fsr4, Graphics::Profile::Balanced);
    reopened.Value(1);
    reopened.Back();
    EXPECT_EQ(base.v.at("graphics_config"), Graphics::Serialize(*c));
    reopened.Reset();
    EXPECT_EQ(reopened.GetStore().Get("graphics_scale"), "-10");
}
TEST(VrGraphics, DisabledRowsRejectPointerAndHaveNoHitTargets) {
    MapStore base;
    Page::Model m(base);
    m.SetTab(6);
    EXPECT_FALSE(m.Focusable(0));
    EXPECT_FALSE(m.Focusable(4));
    for (auto target : {Page::Target::Row, Page::Target::RowLeft, Page::Target::RowRight}) {
        m.SetHover({target, 4});
        m.Click();
    }
    EXPECT_FALSE(m.GraphicsPending());
    EXPECT_TRUE(base.v.empty());
    auto v = Page::MakeView(m, {});
    EXPECT_FALSE(v.rows[4].enabled);
    for (const auto& h : Page::HitBoxes(v, nullptr)) {
        if (h.hover.target == Page::Target::Row || h.hover.target == Page::Target::RowLeft ||
            h.hover.target == Page::Target::RowRight)
            EXPECT_NE(h.hover.index, 4);
    }
    m.SetTab(7);
    EXPECT_FALSE(m.Focusable(1));
    EXPECT_FALSE(m.Focusable(4));
    m.Value(1);
    EXPECT_TRUE(m.Focusable(1));
    EXPECT_TRUE(m.Focusable(4));
    m.Back();
    EXPECT_FALSE(m.GraphicsPending());
}
TEST(VrGraphics, ScaleHasElevenStepsAndDefaultsStayPending) {
    MapStore base;
    Page::Model m(base);
    m.SetTab(6);
    for (int i = 0; i < 10; ++i)
        m.Value(-1);
    EXPECT_EQ(m.GetStore().Get("graphics_scale"), "-50");
    for (int expected = -40; expected <= 50; expected += 10) {
        m.Value(1);
        EXPECT_EQ(m.GetStore().Get("graphics_scale"), std::to_string(expected));
    }
    m.Value(1);
    EXPECT_EQ(m.GetStore().Get("graphics_scale"), "50");
    Page::Defaults(Page::Tabs()[6], m.GetStore());
    EXPECT_EQ(m.GetStore().Get("graphics_scale"), "0");
    EXPECT_TRUE(base.v.empty());
    m.SetHover({Page::Target::ButtonRight, -1});
    m.Click();
    // Returning to the already-applied defaults is not an unsaved change.
    EXPECT_FALSE(m.GraphicsPending());
    EXPECT_TRUE(base.v.empty());
}
TEST(VrGraphics, SerializationRejectsMalformedAndPreservesEveryProfile) {
    for (int provider = 0; provider < 3; ++provider)
        for (int p = 0; p < 3; ++p)
            for (int scale = -50; scale <= 50; scale += 10) {
                Graphics::Config c;
                c.scale_percent = scale;
                c.upscaler = Graphics::Upscaler(provider);
                c.dlss = c.fsr4 = Graphics::Profile(p);
                c.dlss5 = true;
                Graphics::SetNeuralProfile(c, Graphics::Profile(p));
                EXPECT_EQ(Graphics::Parse(Graphics::Serialize(c)), c);
                EXPECT_EQ(Graphics::Unpack(Graphics::Pack(c)), c);
            }
    for (auto bad :
         {"", "2,0,0,2,2,0,2,75,85,70", "1,51,0,2,2,0,2,75,85,70", "1,5,0,2,2,0,2,75,85,70",
          "1,0,3,2,2,0,2,75,85,70", "1,0,0,2,2,0,2,75,85,70,0", "1,0,0,2,2,0,2,75,85,70junk"}) {
        const auto before = Graphics::Accepted();
        EXPECT_FALSE(Graphics::Publish(bad));
        EXPECT_EQ(Graphics::Accepted(), before);
    }
}
TEST(VrGraphics, HeadsetResolutionReportsEachEyeAndDisconnect) {
    Graphics::SetHeadset(0, 2064, 2208);
    Graphics::SetHeadset(1, 2064, 2208);
    EXPECT_EQ(Graphics::ResolutionText(), "2064 x 2208 / eye");
    Graphics::SetHeadset(1, 2048, 2192);
    EXPECT_EQ(Graphics::ResolutionText(), "L 2064 x 2208; R 2048 x 2192");
    Graphics::SetHeadset(0, 0, 0);
    Graphics::SetHeadset(1, 0, 0);
    EXPECT_EQ(Graphics::ResolutionText(), "Headset not connected");
}

TEST(VrGraphics, AcceptHasVisibleBorderAndSelectionHighlight) {
    std::vector<u8> atlas(64 * 64 * 4, 0);
    const Page::Texture t{64, 64, atlas.data()};
    GameText::GameFont font;
    font.tag = {0, 0};
    font.table = 0;
    font.ascent = 18020;
    font.descent = 2460;
    for (u32 c = 32; c < 127; ++c)
        font.glyph[c] = 0;
    font.offsets = {0};
    font.advance = {10240};
    Page::Assets assets{t, t, t, t, &font};
    MapStore base;
    Page::Model m(base);
    m.SetTab(6);
    Page::Image idle, idle_glow, selected, selected_glow;
    const auto v = Page::MakeView(m, {});
    ASSERT_EQ(v.buttons[1].label, "ACCEPT");
    ASSERT_TRUE(Page::Render(assets, v, idle, idle_glow));
    m.SetHover({Page::Target::ButtonRight, -1});
    ASSERT_TRUE(Page::MakeView(m, {}).buttons[1].selected);
    ASSERT_TRUE(Page::Render(assets, Page::MakeView(m, {}), selected, selected_glow));
    // Sample the middle of the upper border, away from text and ornaments.
    const int x = int((PageArt::ButtonX + 137.0f - Page::PageX0) * Page::PageScale);
    const int y = int((PageArt::ButtonY + 12.0f - 36.0f - Page::PageY0) * Page::PageScale);
    const auto pixel = [&](const Page::Image& im) {
        return im.rgba[(size_t(y) * im.width + x) * 4];
    };
    EXPECT_GT(pixel(idle), 0);
    EXPECT_GT(pixel(selected), pixel(idle));
    EXPECT_GT(pixel(selected_glow), pixel(idle_glow));
    const auto hit = Page::HitTest(Page::HitBoxes(v, nullptr), PageArt::ButtonX + 137.0f,
                                   PageArt::ButtonY - 10.0f);
    EXPECT_EQ(hit.target, Page::Target::ButtonRight);
}
TEST(VrGraphics, ForcedCloseAndBackRestoreLastAcceptedAcrossBothTabs) {
    MapStore base;
    Graphics::Config accepted;
    accepted.scale_percent = 20;
    accepted.upscaler = Graphics::Upscaler::Fsr4;
    accepted.dlss5 = true;
    base.v["graphics_config"] = Graphics::Serialize(accepted);
    Page::Model m(base);
    m.SetTab(6);
    m.Value(-1);
    m.SetTab(7);
    m.Value(-1);
    EXPECT_TRUE(m.GraphicsPending());
    m.DiscardGraphics();
    EXPECT_FALSE(m.GraphicsPending());
    EXPECT_EQ(m.GetStore().Get("graphics_scale"), "20");
    EXPECT_EQ(m.GetStore().Get("graphics_neural"), "0"); // Legacy FSR+neural is not active DLSS 5.
    m.SetTab(6);
    m.Value(1);
    EXPECT_EQ(m.Back(), Page::Action::Close);
    EXPECT_EQ(m.GetStore().Get("graphics_scale"), "20");
    EXPECT_EQ(base.v["graphics_config"], Graphics::Serialize(accepted));
}

TEST(VrGraphics, GraphicsTabsShareScrolledDrawingAndHitCoordinates) {
    GameText::GameFont font;
    font.advance = {14000};
    for (u32 c = 32; c < 127; ++c)
        font.glyph[c] = 0;
    MapStore base;
    Page::Model m(base);
    m.SetTab(6);
    const auto v = Page::MakeView(m, {});
    const auto layout = Page::Draw::LayoutTabs(v.tabs, v.glyphs, &font, v.tab);
    EXPECT_GE(layout.prev_cx - layout.prev_w * 0.5f, PageArt::FrameX0);
    EXPECT_LE(layout.next_cx + layout.next_w * 0.5f, PageArt::FrameX1);
    const auto boxes = Page::HitBoxes(v, &font);
    for (size_t i = 0; i < v.tabs.size(); ++i) {
        if (layout.width[i] == 0.0f) continue; // off-window tabs have no hit target
        const auto hit =
            Page::HitTest(boxes, layout.x[i] + layout.width[i] * 0.5f, Page::Draw::TitleBaseline);
        EXPECT_EQ(hit.target, Page::Target::Tab);
        EXPECT_EQ(hit.index, int(i));
    }
}

TEST(VrGraphics, NewBodyTabsKeepTheirOwnHelpWhileGraphicsArePending) {
    MapStore store;
    Page::Model model(store);
    model.SetTab(6);
    model.Value(-1);
    ASSERT_TRUE(model.GraphicsPending());
    for (int tab = 8; tab < static_cast<int>(Page::Tabs().size()); ++tab) {
        model.SetTab(tab);
        const auto view = Page::MakeView(model, {});
        const auto* row = model.RowAt(model.GetState().focus);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(view.help, Page::RowHelp(*row, model.GetStore()));
        EXPECT_EQ(view.help.find("Graphics"), std::string::npos);
    }
}

TEST(VrGraphics, RestartRequiresExplicitYesAndSavesTheDraft) {
    MapStore base;
    Page::Model model(base);
    model.SetTab(6);
    model.Value(1);
    model.SetHover({Page::Target::ButtonLeft, -1});
    EXPECT_EQ(model.Click(), Page::Action::None);
    EXPECT_TRUE(model.GetState().confirm_restart);
    EXPECT_EQ(model.GetState().confirm_yes, 0);
    EXPECT_EQ(model.Confirm(), Page::Action::None);
    EXPECT_TRUE(base.v.empty());
    EXPECT_TRUE(model.GraphicsPending());
    model.SetHover({Page::Target::ButtonLeft, -1});
    model.Click(); model.Back();
    EXPECT_TRUE(base.v.empty());
    model.SetHover({Page::Target::ButtonLeft, -1});
    model.Click(); model.Value(-1);
    EXPECT_EQ(model.Confirm(), Page::Action::RestartGraphics);
    ASSERT_TRUE(base.v.contains("graphics_config"));
    EXPECT_EQ(Graphics::Parse(base.v["graphics_config"])->scale_percent, 10);
    EXPECT_FALSE(model.GraphicsPending());
}

TEST(VrGraphics, RuntimeReadoutDoesNotReplaceHelpAndRevertedEditsAreClean) {
    MapStore base;
    base.v["graphics_status"] = "DLSS active | 950x1046 -> 1424x1568 / eye";
    base.v["graphics_config"] = Graphics::Serialize({});
    Page::Model m(base);
    m.SetTab(6);
    m.SetHover({Page::Target::Row, 2});
    auto view = Page::MakeView(m, {});
    EXPECT_EQ(view.graphics_status, base.v["graphics_status"]);
    EXPECT_NE(view.help.find("reconstruct"), std::string::npos);
    m.GetStore().Set("graphics_scale", "10");
    EXPECT_TRUE(m.GraphicsPending());
    EXPECT_NE(Page::MakeView(m, {}).graphics_status.find("Unsaved"), std::string::npos);
    m.GetStore().Set("graphics_scale", "0");
    EXPECT_FALSE(m.GraphicsPending());
    EXPECT_EQ(Page::MakeView(m, {}).graphics_status, base.v["graphics_status"]);
    m.SetTab(0);
    EXPECT_TRUE(Page::MakeView(m, {}).graphics_status.empty());
}
TEST(VrGraphics, RestartButtonEdgesAreClickableAndConfirmationStaysDistinct) {
    MapStore base;
    Page::Model m(base);
    m.SetTab(6);
    auto v = Page::MakeView(m, {});
    auto boxes = Page::HitBoxes(v, nullptr);
    const auto button = std::find_if(boxes.begin(), boxes.end(), [](auto& b) {
        return b.hover.target == Page::Target::ButtonLeft;
    });
    ASSERT_NE(button, boxes.end());
    EXPECT_FLOAT_EQ(button->x1 - button->x0, 460.0f);
    EXPECT_EQ(Page::HitTest(boxes, button->x0 + 1, button->y0 + 10).target, Page::Target::ButtonLeft);
    EXPECT_EQ(Page::HitTest(boxes, button->x1 - 1, button->y0 + 10).target, Page::Target::ButtonLeft);
    m.SetHover({Page::Target::ButtonLeft, -1});
    m.Confirm();
    v = Page::MakeView(m, {});
    EXPECT_TRUE(v.confirm);
    EXPECT_TRUE(v.graphics_status.empty());
    EXPECT_TRUE(v.buttons[1].selected); // safe default remains NO
}

TEST(VrGraphics, ThreeUpscalersAreExclusiveAndNeuralOwnsItsReconstruction) {
    MapStore base;
    Page::GraphicsDraftStore draft(base);
    for (const auto* selected : {"graphics_dlss", "graphics_fsr4", "graphics_neural",
                                 "graphics_dlss", "graphics_neural", "graphics_fsr4"}) {
        draft.Set(selected, "1");
        for (const auto* choice : {"graphics_dlss", "graphics_fsr4", "graphics_neural"})
            EXPECT_EQ(draft.Get(choice), std::string_view(selected) == choice ? "1" : "0");
        EXPECT_TRUE(base.v.empty());
    }
    draft.Set("graphics_neural", "1");
    EXPECT_FALSE(Page::Enabled(Page::Rows()[Page::RowIndex("graphics_mode")], draft));
    ASSERT_TRUE(draft.Accept());
    auto saved = Graphics::Parse(base.v["graphics_config"]);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->upscaler, Graphics::Upscaler::Dlss);
    EXPECT_TRUE(saved->dlss5);
    draft.Set("graphics_neural", "0");
    EXPECT_EQ(draft.Get("graphics_dlss"), "0");
    draft.Discard();
    EXPECT_EQ(draft.Get("graphics_neural"), "1");
}
TEST(VrGraphics, NeuralTabCanSaveAndRestartWithoutNavigatingAway) {
    MapStore base;
    Page::Model m(base);
    m.SetTab(7);
    m.Value(1);
    EXPECT_EQ(m.GetStore().Get("graphics_neural"), "1");
    m.SetHover({Page::Target::ButtonLeft, -1});
    m.Click();
    ASSERT_TRUE(m.GetState().confirm_restart);
    EXPECT_TRUE(base.v.empty());
    m.Value(-1);
    EXPECT_EQ(m.Confirm(), Page::Action::RestartGraphics);
    const auto c = Graphics::Parse(base.v["graphics_config"]);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->upscaler, Graphics::Upscaler::Dlss);
    EXPECT_TRUE(c->dlss5);
}

TEST(VrGraphics, NeuralMenuHasNoQualityAndResetReturnsBalanced) {
    const auto& row=Page::Rows()[Page::RowIndex("graphics_neural_mode")];
    ASSERT_EQ(row.options.size(),2u);
    MapStore base; Page::GraphicsDraftStore draft(base);
    draft.Set("graphics_neural","1"); draft.Set("graphics_neural_mode","2");
    EXPECT_EQ(draft.Get("graphics_neural_mode"),"1");
    draft.Set("graphics_neural_mode","0"); draft.Remove("graphics_neural_mode");
    EXPECT_EQ(draft.Get("graphics_neural_mode"),"1");
    ASSERT_TRUE(draft.Accept());
    EXPECT_EQ(Graphics::EffectiveProfile(*Graphics::Parse(base.v["graphics_config"])),Graphics::Profile::Balanced);
}

TEST(VrPage, PhysicalLeversToggleAndDefaults) {
    MapStore store;
    const auto& row = Page::Rows()[Page::RowIndex("physical_levers")];
    EXPECT_EQ(Page::ValueText(row, store), "On");
    EXPECT_EQ(Page::Step(row, store, 1).text, "Off");
    EXPECT_EQ(store.Get("physical_levers"), "0");
    const auto tab = std::find_if(Page::Tabs().begin(), Page::Tabs().end(),
        [](const auto& t) { return std::string_view(t.name) == "Interactions"; });
    ASSERT_NE(tab, Page::Tabs().end());
    Page::Defaults(*tab, store);
    EXPECT_EQ(store.Get("physical_levers"), "1");
}

TEST(VrPage, WeaponsHasPersistentGripGuideChoice) {
    MapStore store;
    store.Set("two_hand_guide", "1");
    const auto& row = Page::Rows()[Page::RowIndex("two_hand_guide")];
    EXPECT_EQ(Page::ValueText(row, store), "On");
    EXPECT_EQ(Page::Step(row, store, 1).text, "Off");
    EXPECT_EQ(store.Get("two_hand_guide"), "0");
    EXPECT_EQ(Page::Step(row, store, 1).text, "On");
    bool found = false;
    for (const auto& tab : Page::Tabs()) {
        if (std::string(tab.name) == "Weapons") {
            found = std::find(tab.rows.begin(), tab.rows.end(), Page::RowIndex("two_hand_guide")) != tab.rows.end();
        }
    }
    EXPECT_TRUE(found);
}


TEST(VrPage, WeaponsSwingFeelCanBeReducedDisabledAndRestored) {
    MapStore store;
    const auto& tab = Page::Tabs()[11];
    ASSERT_STREQ(tab.name, "Weapons");
    const int index = Page::RowIndex("two_hand_swing_hum");
    ASSERT_GE(index, 0);
    EXPECT_NE(std::find(tab.rows.begin(), tab.rows.end(), index), tab.rows.end());
    const auto& row = Page::Rows()[index];
    EXPECT_EQ(Page::ValueText(row, store), "100");
    EXPECT_EQ(Page::Step(row, store, -1).text, "90");
    for (int i = 0; i < 12; ++i) Page::Step(row, store, -1);
    EXPECT_EQ(Page::ValueText(row, store), "0");
    EXPECT_EQ(std::stof(store.Get("two_hand_swing_hum")), 0);
    Page::Defaults(tab, store);
    EXPECT_EQ(Page::ValueText(row, store), "100");
}

// Lamp actions use the page renderer but never expose settings widgets or Defaults/Apply.
#include "core/vr/lamp_page.h"
TEST(VrLampPage, ActionsAndLongDestinationNamesHaveFullWidthPointerTargets) {
    using namespace VR;
    Lamps::Model lamp{{91, 2412951, Lamps::TravelPages([](int id) -> std::optional<bool> {
                           return id < 70000000;
                       })}};
    auto view = Lamps::MakePage(lamp);
    EXPECT_EQ(view.tabs, (std::vector<std::string>{"Lamp"}));
    EXPECT_EQ(view.headers[0], "Central Yharnam");
    EXPECT_FALSE(view.buttons[0].present);
    EXPECT_FALSE(view.buttons[1].present);
    EXPECT_EQ(view.rows[0].label, "Rest");
    EXPECT_EQ(view.rows[3].label, "Leave");
    EXPECT_FALSE(view.rows[4].present);
    lamp.focus = 1;
    lamp.Confirm();
    for (size_t page = 0; page < lamp.offer.pages.size(); ++page) {
        auto travel = Lamps::MakePage(lamp);
        auto boxes = Page::HitBoxes(travel, nullptr);
        for (int row = 0; row < lamp.Count(); ++row) {
            EXPECT_EQ(travel.rows[row].widget, Page::Widget::Action);
            auto found = std::find_if(boxes.begin(), boxes.end(), [&](const Page::HitBox& b) {
                return b.hover.target == Page::Target::Row && b.hover.index == row;
            });
            ASSERT_NE(found, boxes.end());
            EXPECT_GT(found->x1 - found->x0, 800.f);
            EXPECT_EQ(Page::HitTest(boxes, (found->x0 + found->x1) / 2, (found->y0 + found->y1) / 2)
                          .index,
                      row);
        }
        EXPECT_TRUE(std::none_of(boxes.begin(), boxes.end(), [](const Page::HitBox& b) {
            return b.hover.target == Page::Target::RowLeft ||
                   b.hover.target == Page::Target::RowRight;
        }));
        lamp.Tab(1);
    }
}

TEST(VrPage, WristReloadIsLiveOnWeaponsAndDefaultsOn) {
    MapStore store;
    const auto& tab = Page::Tabs()[11];
    ASSERT_STREQ(tab.name, "Weapons");
    ASSERT_EQ(tab.rows[3], Page::RowIndex("gun_wrist_reload"));
    const auto& row = Page::Rows()[tab.rows[3]];
    EXPECT_FALSE(row.restart);
    EXPECT_EQ(Page::ValueText(row, store), "On");
    EXPECT_EQ(Page::Step(row, store, 1).text, "Off");
    EXPECT_EQ(store.Get("gun_wrist_reload"), "0");
    Page::Defaults(tab, store);
    EXPECT_EQ(Page::ValueText(row, store), "On");
}

TEST(VrPage, ColorAdjustmentsAreLiveBoundedAndResetTogether) {
    MapStore store;
    Page::Model model{store};
    model.Reset();
    const auto it = std::find_if(Page::Tabs().begin(), Page::Tabs().end(),
                                [](const auto& tab) { return std::string_view(tab.name) == "Color"; });
    ASSERT_NE(it, Page::Tabs().end());
    model.TabStep(static_cast<int>(it - Page::Tabs().begin()));
    store.Set("aa_mode", "off");
    const std::array<const char*, 3> keys{"color_contrast", "color_saturation", "color_gamma"};
    for (int i = 0; i < 3; ++i) {
        const auto& row = Page::Rows()[Page::RowIndex(keys[i])];
        EXPECT_FALSE(row.restart);
        EXPECT_FALSE(row.placeholder);
        EXPECT_EQ(Page::ValueText(row, store), "0%");
        model.Move(Page::Dir::Right);
        EXPECT_EQ(store.Get(keys[i]), "5");
        EXPECT_EQ(Page::ValueText(row, store), "+5%");
        for (int n = 0; n < 50; ++n) Page::Step(row, store, 1);
        EXPECT_EQ(Page::CurrentNumber(row, store), row.max);
        for (int n = 0; n < 50; ++n) Page::Step(row, store, -1);
        EXPECT_EQ(Page::CurrentNumber(row, store), row.min);
        model.Move(Page::Dir::Down);
    }
    EXPECT_EQ(model.GetState().focus, Page::FocusRightButton);
    model.Confirm();
    model.Move(Page::Dir::Left);
    model.Confirm();
    EXPECT_EQ(store.removed, (std::vector<std::string>{keys.begin(), keys.end()}));
    for (const auto* key : keys) EXPECT_EQ(store.Get(key), "0");
    EXPECT_EQ(store.Get("aa_mode"), "off");
}


TEST(VrPage, GrabAttacksIsDefaultOffAndShownInInteractions) {
    const auto index = VR::Page::RowIndex("vr_grab_attacks");
    ASSERT_GE(index, 0);
    const auto& row = VR::Page::Rows()[index];
    EXPECT_STREQ(row.label, "VR Grab attacks");
    EXPECT_STREQ(row.keys[0], "vr_grab_attacks");
    ASSERT_EQ(row.options.size(), 2u);
    EXPECT_STREQ(row.options[0].text, "Off");
    EXPECT_STREQ(row.options[0].values[0], "0");
    EXPECT_STREQ(row.options[1].values[0], "1");
    bool visible = false;
    for (const auto& tab : VR::Page::Tabs()) {
        if (std::string(tab.name) == "Interactions")
            visible = std::find(tab.rows.begin(), tab.rows.end(), index) != tab.rows.end();
    }
    EXPECT_TRUE(visible);
}

TEST(VrPage, GrabAttackToggleAndDefaultsPreserveOtherCinematicSettings) {
    MapStore store;
    store.Set("physical_levers", "0");
    const auto& row = Page::Rows()[Page::RowIndex("vr_grab_attacks")];
    EXPECT_EQ(Page::ValueText(row, store), "Off");
    Page::Step(row, store, 1);
    EXPECT_EQ(store.Get("vr_grab_attacks"), "1");
    EXPECT_EQ(store.Get("physical_levers"), "0");
    Page::Step(row, store, -1);
    EXPECT_EQ(store.Get("vr_grab_attacks"), "0");
    EXPECT_EQ(store.Get("physical_levers"), "0");
    EXPECT_EQ(store.Get("cinematic"), "1");
    for (const auto& tab : Page::Tabs()) {
        if (std::string(tab.name) == "Interactions") Page::Defaults(tab, store);
    }
    EXPECT_EQ(Page::ValueText(row, store), "Off");
    EXPECT_EQ(store.Get("cinematic"), "1");
}


#include "core/vr/menu_exit_input.h"

TEST(VrMenuExit, BackBlocksMenuItemButtonsUntilRelease) {
    // A, B, X, Y, left grip (Square), right grip (Triangle).
    std::array<bool, 6> raw{false, true, false, false, true, true};
    auto in = raw;
    std::array<bool, 6> blocked{};
    VR::MenuInput::GuardExit(raw, in, blocked, true, 1, 3);
    EXPECT_TRUE(in[1]);
    EXPECT_FALSE(in[4]);
    EXPECT_FALSE(in[5]);
    EXPECT_TRUE(blocked[4]);
    EXPECT_TRUE(blocked[5]);
    raw[1] = false; // back released first: squeezed grips remain blocked
    for (size_t i = 0; i < raw.size(); ++i) {
        if (!raw[i]) blocked[i] = false;
        in[i] = raw[i] && !blocked[i];
    }
    VR::MenuInput::GuardExit(raw, in, blocked, true, 1, 3);
    EXPECT_FALSE(in[4]);
    EXPECT_FALSE(in[5]);
}

TEST(VrMenuExit, MenuTailBlocksNewPressesEvenWithoutBackHeld) {
    std::array<bool, 6> raw{true, false, false, false, true, true};
    auto in = raw;
    std::array<bool, 6> blocked{};
    VR::MenuInput::GuardExit(raw, in, blocked, false, 1, 3);
    EXPECT_TRUE(std::none_of(in.begin(), in.end(), [](bool v) { return v; }));
    EXPECT_TRUE(blocked[0]);
    EXPECT_TRUE(blocked[4]);
    EXPECT_TRUE(blocked[5]);
}

TEST(VrMenuExit, OrdinaryMenuActionsAndBothBackButtonsStillWork) {
    for (const size_t back : {1u, 3u}) {
        std::array<bool, 6> raw{};
        raw[back] = true;
        auto in = raw;
        std::array<bool, 6> blocked{};
        VR::MenuInput::GuardExit(raw, in, blocked, true, 1, 3);
        EXPECT_TRUE(in[back]);
        EXPECT_FALSE(blocked[back]);
    }
    std::array<bool, 6> raw{true, false, true, false, true, true};
    auto in = raw;
    std::array<bool, 6> blocked{};
    VR::MenuInput::GuardExit(raw, in, blocked, true, 1, 3);
    EXPECT_EQ(in, raw);
    EXPECT_TRUE(std::none_of(blocked.begin(), blocked.end(), [](bool v) { return v; }));
}
