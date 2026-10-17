// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/vr/bloodborne_heal.h"
#include "core/vr/bloodborne_menu.h"
#include "core/vr/lamp_channel.h"
#include "core/vr/lamp_travel.h"
#include "core/vr/bloodborne_vrtile.h"
#include "core/vr/hook_util.h"
#include "core/vr/vr_session.h"
#include "core/vr/ui_layout.h"

namespace VR::Bloodborne {

namespace {

using Hook::Plausible;
using Hook::Read;

// NewMenuSystem: the copy the game's menu-state reader loads (0x176B878) and MenuMan's copy;
// both point at the same object once the menus exist.
constexpr u64 MenuSystemRva = 0x5562870;
constexpr u64 MenuSystemCopyRva = 0x5580E38;
constexpr u64 MenuSystemVtableRva = 0x539C110;
constexpr u64 MenuManRva = 0x5562878;
constexpr u64 MenuManVtableRva = 0x532B490;
constexpr u64 NewSystemRva = 0x5562882; // the reader uses the new menu system only when set
constexpr u32 OffsetModalMenu = 0xA88;  // the open modal menu (0 = none)
constexpr u32 OffsetMenuSerial = 0xB18; // +1 on every open and close
// The UI the game shows without a modal menu (they keep the gameplay controls and the gameplay HUD
// panel), logged with VR_UI_LOG. MenuMan: +0x54 menu request (7 = the Options menu is about to
// open), +0x58 text window (4 = an NPC's subtitle, 1..3 = an event or tutorial text), +0x838 /
// +0x82C an action prompt is shown / its text id. SprjWorldTalkMan: +0x3FF8 an NPC line is playing
// (also published for talk_guard), +0x400C the talk / menu input cooldown (s). ItemGetMenuMan
// +0x08: item pickup pop-ups shown.
constexpr u32 OffsetMenuRequest = 0x54;
constexpr u32 OffsetTextWindow = 0x58;
constexpr u32 OffsetPromptText = 0x82C;
constexpr u32 OffsetPromptShown = 0x838;
constexpr u64 TalkManRva = 0x5540508;
constexpr u32 OffsetTalking = 0x3FF8;
constexpr u32 OffsetTalkCooldown = 0x400C;
constexpr u64 ItemGetRva = 0x553B1C0;
constexpr u64 ItemGetVtableRva = 0x5328240;
constexpr u32 OffsetItemGetList = 0x08;
// The Scaleform menu Open (0x1BB27A0: out, menu system, &job, &on_close): which menu opens (the
// log, and the panel zoom of small dialogs). Its prologue is position independent (push rbp ..
// sub rsp, 0x38); the stub replays it. Most menus are the generic Scaleform job (vtable
// 0x5345EE0, built by 0x1C1CCE0) or built on it, whose +0x30 is the movie name (L"BloodMessage",
// L"LevelUp", ...); other classes are logged by their vtable.
constexpr u64 MenuOpenRva = 0x1BB27A0;
constexpr size_t MenuOpenLen = 17;
constexpr std::array<u8, 28> MenuOpenSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
    0x83, 0xEC, 0x38, 0x49, 0x89, 0xCC, 0x49, 0x89, 0xD7, 0x49, 0x89, 0xF5, 0x49, 0x89,
};
constexpr u64 GenericJobVtableRva = 0x5345EE0;
constexpr u32 OffsetJobName = 0x30;
// The banners in the middle of the screen (the HUD movie game asset's text effects): MenuMan +0x15C8
// is a ring of 17 queued types. SetTextEffect (0x13360B0, the event scripts' SetTextEffect and the
// game's own callers inline the same code) writes the type at the index +0x1610 and moves it on;
// the HUD's update (0x1BF9910) plays the one at +0x160C (the clip of slot type: 1 KillDemon (PREY
// SLAUGHTERED / NIGHTMARE SLAIN), 2 Dead (YOU DIED), 4 SoulGet (the blood echoes regained),
// 5 TargetClear, 7 BlackClear, 15 YouWin, 16 YouLose, 17 Draw; the other types have no clip and
// show nothing), zeroes it and moves +0x160C on.
constexpr u32 OffsetTextEffects = 0x15C8;
constexpr u32 OffsetTextEffectRead = 0x160C;
constexpr u32 OffsetTextEffectWrite = 0x1610;
constexpr s32 TextEffectSlots = 17;
constexpr u64 SetTextEffectRva = 0x13360B0;
constexpr std::array<u8, 6> SetTextEffectSig = {0x8D, 0x46, 0xFF, 0x83, 0xF8, 0x10};
using SetTextEffectFn = void(PS4_SYSV_ABI*)(void* self, s32 type);

const char* TextEffectName(u32 type) {
    switch (type) {
    case 1:
        return "KillDemon (prey slaughtered / nightmare slain)";
    case 2:
        return "Dead (YOU DIED)";
    case 4:
        return "SoulGet (blood echoes regained)";
    case 5:
        return "TargetClear";
    case 7:
        return "BlackClear";
    case 15:
        return "YouWin";
    case 16:
        return "YouLose";
    case 17:
        return "Draw";
    default:
        return "unknown (played between two samples)";
    }
}
std::unique_ptr<Xbyak::CodeGenerator> g_open_code;
// The last named menu: the serial it gets once open (Open adds 1 at 0x1BB293D) and how much
// larger the menu panel shows it. Written by the opening thread, read by the player update.
std::atomic<u64> g_named_serial{0};
std::atomic<float> g_named_zoom{1.0f};
std::atomic<u8> g_named_class{static_cast<u8>(VR::UiClass::Full)};
std::atomic<float> g_named_pitch{0.0f};
// The named menu is the Options grid (IngameTop, not over another menu): its cursor is published
// with it, so the UI layer knows which of its pages opens (VR::Ui::IsGridPageAtOnce).
std::atomic<bool> g_named_box{false};
std::atomic<bool> g_named_grid{false};

// Where the canvas of a menu goes relative to where you look (degrees, + = up): the headstone's
// travel list (WarpMenu) ui_travel_pitch_deg, as its entries sit at the top of a tall, mostly empty
// panel (mock 2026-10-07, the Dream save: 62.8 x 50.5 deg, "Awaken Above Ground / 1st Floor
// Sickroom" 17-20 deg above the view's centre); every other menu centred.
float PitchFor(const std::string& name, bool over_menu) {
    return !over_menu && name == "WarpMenu" ? VR::GetSettings().ui_travel_pitch_deg : 0.0f;
}

// Menus whose content is a small box in the middle of the screen (vr.ini menu_zoom_menus, default
// MessageBox: checked in mock dumps, the headstone's "Awaken at a selected location" and the
// Messengers' "offer you a trick weapon" OK boxes): the panel shows them menu_dialog_zoom times
// larger, unless another menu is open under them (its full-screen image would grow past the
// view). Every other menu keeps the panel size (full-screen menus must fit; the boxes inside
// other menus, such as the weapon list's YES / NO, belong to that menu).
float ZoomFor(const std::string& name, bool over_menu) {
    if (name.empty() || over_menu) {
        return 1.0f;
    }
    const std::string& list = VR::GetSettings().menu_zoom_menus;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        const size_t end = comma == std::string::npos ? list.size() : comma;
        size_t a = start, b = end;
        while (a < b && list[a] == ' ') {
            ++a;
        }
        while (b > a && list[b - 1] == ' ') {
            --b;
        }
        if (list.compare(a, b - a, name) == 0) {
            return std::max(VR::GetSettings().menu_dialog_zoom, 1.0f);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return 1.0f;
}

// How the UI layer shows a menu (VR::UiClass): the small boxes in the middle of the screen (the
// headstone's and the Messengers' OK boxes, YES / NO on its own) as an enlarged crop of the
// middle, the messengers' notes too, everything else (and every box over another menu) whole.
VR::UiClass ClassFor(const std::string& name, bool over_menu) {
    if (name == "BloodMessage") {
        return VR::UiClass::Note;
    }
    // (MessageBox was lost from this list on 2026-10-13, "IngameMessageBox" written twice: every
    // lamp, headstone and Messenger box went whole onto the full-menu canvas with the HUD kept
    // under it.)
    if (!over_menu &&
        (name == "MessageBox" || name == "IngameMessageBox" || name == "TombMessage")) {
        return VR::UiClass::Dialog;
    }
    return VR::UiClass::Full;
}

u8* g_base = nullptr;
u64 g_size = 0;
bool g_enabled = false;
bool g_open = false;
u64 g_serial = 0;
const char* g_refusal = nullptr; // the last reason detection is off (logged on change)
bool g_ui_log = false;
bool g_talking = false; // the last published NPC talk state (logged on change)
u32 g_talk_logs = 0;

void Refuse(const char* why) {
    if (g_refusal != why) {
        g_refusal = why;
        LOG_INFO(Core, "VR menu: detection off ({}); gameplay controls stay", why);
    }
    if (g_open) {
        g_open = false;
        LOG_INFO(Core, "VR menu: closed (detection lost)");
    }
    VR::PublishGameMenu(false);
}

// VR_UI_LOG=1: one line whenever a UI signal changes (which UI is modal, which is not).
void LogUi(u8* man, bool open, u64 serial) {
    struct Ui {
        bool open;
        u64 serial;
        s32 request;
        s32 text_window;
        s32 prompt;
        s32 prompt_text;
        s32 talking;
        bool cooldown;
        bool popup;
        bool operator==(const Ui&) const = default;
    };
    static Ui last{};
    static bool first = true;
    u8* const talk = Read<u8*>(g_base + TalkManRva);
    u8* const item_get = Read<u8*>(g_base + ItemGetRva);
    const bool talk_ok = Plausible(talk);
    const bool item_get_ok =
        Plausible(item_get) && Read<u8*>(item_get) == g_base + ItemGetVtableRva;
    const Ui now{
        .open = open,
        .serial = serial,
        .request = Read<s32>(man + OffsetMenuRequest),
        .text_window = Read<s32>(man + OffsetTextWindow),
        .prompt = Read<s32>(man + OffsetPromptShown),
        .prompt_text = Read<s32>(man + OffsetPromptText),
        .talking = talk_ok ? Read<u8>(talk + OffsetTalking) : -1,
        .cooldown = talk_ok && Read<float>(talk + OffsetTalkCooldown) > 0.0f,
        .popup = item_get_ok && Read<u64>(item_get + OffsetItemGetList) != 0,
    };
    if (!first && now == last) {
        return;
    }
    first = false;
    last = now;
    LOG_INFO(Core,
             "VR ui: menu={} ser={} req54={} text58={} ({}) prompt={}/{} talk={} cooldown={} "
             "pickup={}",
             now.open ? 1 : 0, now.serial, now.request, now.text_window,
             now.text_window == 4                               ? "NPC subtitle"
             : now.text_window >= 1 && now.text_window <= 3     ? "event text"
                                                                : "none",
             now.prompt, now.prompt_text, now.talking, now.cooldown ? 1 : 0, now.popup ? 1 : 0);
}

bool InModule(const u8* address, u64 length) {
    return address >= g_base && address + length <= g_base + g_size;
}

// Menu Open entry (the game's thread that opens it; the job is alive): logs the menu's name and
// remembers its panel zoom for the serial it will have.
void PS4_SYSV_ABI MenuOpenHook(u8* job_slot, u8* /*saved_ymm*/) {
    u8* const system = Read<u8*>(g_base + MenuSystemRva); // what Open is called with (rsi)
    u8* const job = Plausible(job_slot) ? Read<u8*>(job_slot) : nullptr;
    if (!Plausible(job, 16)) {
        return;
    }
    u8* const vtable = Read<u8*>(job);
    // The generic job and the jobs built on it (e.g. 0x5345F10, the Messengers' weapon choice,
    // from 0x1C1DD40) keep the name the generic constructor stored; for any other class the
    // field is used only when it reads as a name in the eboot's constant data.
    std::string name;
    const u8* text = Read<const u8*>(job + OffsetJobName);
    for (u32 i = 0; i < 48 && Plausible(text, 2) && InModule(text, 2 * (i + 1)); ++i) {
        const char16_t c = Read<char16_t>(text + 2 * i);
        if (c == 0) {
            break;
        }
        const bool word = (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z') ||
                          (c >= u'0' && c <= u'9') || c == u'_';
        if (!word) {
            name.clear();
            break;
        }
        name += static_cast<char>(c);
    }
    if (vtable != g_base + GenericJobVtableRva && name.size() < 3) {
        name.clear();
    }
    const bool system_ok = Plausible(system) && Read<u8*>(system) == g_base + MenuSystemVtableRva;
    // Another modal menu still open: this one opens over it.
    const bool over_menu = system_ok && Read<u64>(system + OffsetModalMenu) != 0;
    const float zoom = ZoomFor(name, over_menu);
    const VR::UiClass cls = ClassFor(name, over_menu);
    if (system_ok) {
        g_named_zoom.store(zoom, std::memory_order_relaxed);
        g_named_class.store(static_cast<u8>(cls), std::memory_order_relaxed);
        g_named_pitch.store(PitchFor(name, over_menu), std::memory_order_relaxed);
        g_named_grid.store(!over_menu && name == "IngameTop", std::memory_order_relaxed);
        g_named_box.store(VR::Lamps::IsLampDialogJob(name, over_menu), std::memory_order_relaxed);
        // Maybe the lantern's box: hidden from its first frame until the lamp site has read its
        // message (a few frames; any other box then shows from there, still fading in).
        if (VR::Lamps::IsLampDialogJob(name, over_menu) && VR::Lamps::Enabled() &&
            VR::GetSettings().menu_mode) {
            VR::PublishLampBoxHidden(true);
        }
        g_named_serial.store(Read<u64>(system + OffsetMenuSerial) + 1, std::memory_order_release);
    }
    const float pitch = PitchFor(name, over_menu);
    LOG_INFO(Core, "VR menu: opening {} (job vtable {:#x}){}{}{}, UI layer: {}",
             name.empty() ? std::string{"a menu"} : "'" + name + "'",
             InModule(vtable, 8) ? static_cast<u64>(vtable - g_base) : 0,
             over_menu ? " over an open menu" : "",
             zoom > 1.0f ? fmt::format(", menu panel x{:.2f}", zoom) : std::string{},
             pitch != 0.0f ? fmt::format(", canvas {:+.0f} deg from where you look", pitch)
                           : std::string{},
             VR::UiClassName(cls));
}

// Logs only; a failed check leaves the menu detection as it is.
void InstallMenuOpenHook() {
    u8* const site = g_base + MenuOpenRva;
    if (g_size < MenuOpenRva + MenuOpenSig.size() ||
        std::memcmp(site, MenuOpenSig.data(), MenuOpenSig.size()) != 0) {
        LOG_INFO(Core, "VR menu: menu Open not found ({}); menu names are not logged",
                 g_size < MenuOpenRva + MenuOpenSig.size()
                     ? std::string{"eboot too small"}
                     : Hook::HexBytes(site, MenuOpenSig.size()));
        return;
    }
    if (!Hook::MakeWritable(site, MenuOpenLen)) {
        LOG_WARNING(Core, "VR menu: could not make menu Open writable; menu names are not logged");
        return;
    }
    try {
        g_open_code = std::make_unique<Xbyak::CodeGenerator>(512);
        void* const stub = Hook::EmitHostCallStub(
            *g_open_code, reinterpret_cast<void*>(&MenuOpenHook), MenuOpenSig.data(), MenuOpenLen,
            reinterpret_cast<u64>(site + MenuOpenLen), Xbyak::util::rdx);
        Hook::WriteJump(site, MenuOpenLen, stub);
    } catch (const Xbyak::Error& e) {
        LOG_WARNING(Core, "VR menu: menu Open hook not generated ({})", e.what());
        return;
    }
    LOG_INFO(Core, "VR menu: menu Open hook installed at {:#x} (menu names in the log)",
             MenuOpenRva);
}

// The text effects the HUD played since the last sample (its read index moved on), published as
// banners (VR::PublishBanner) for the UI layer's title. A slot's type is the one seen there at the
// last sample; a type without a clip is not one. The HUD takes a slot whenever any slot holds a
// type, also an empty one when its index is behind the writer's (it then plays nothing and moves
// on, one slot per update, until it reaches the type): an empty slot is a banner only when it was
// written since the last sample (queued and played between two samples: its type is not known).
struct TextEffectWatch {
    bool valid{false};
    s32 read{0};
    s32 write{0};
    std::array<u32, TextEffectSlots> seen{};
};
TextEffectWatch g_effects{};

void WatchTextEffects(u8* man) {
    const s32 read = Read<s32>(man + OffsetTextEffectRead);
    const s32 write = Read<s32>(man + OffsetTextEffectWrite);
    if (read < 0 || read >= TextEffectSlots || write < -1 || write >= TextEffectSlots) {
        g_effects.valid = false;
        return;
    }
    if (g_effects.valid) {
        // The slots written since the last sample: from the old write index up to the new one.
        const auto written = [&](s32 slot) {
            for (s32 i = g_effects.write, n = 0; i != write && n < TextEffectSlots; ++n) {
                if (i == slot) {
                    return true;
                }
                i = i < 0 ? 0 : (i + 1) % TextEffectSlots;
            }
            return false;
        };
        for (s32 i = g_effects.read, n = 0; i != read && n < TextEffectSlots;
             i = (i + 1) % TextEffectSlots, ++n) {
            const u32 type = g_effects.seen[i];
            if (type == 0 ? !written(i) : !Ui::BannerTextEffect(type)) {
                continue;
            }
            // A lantern warp leaves and arrives without the lamp's cutscene: the text effect it
            // queues on arrival is no banner (2026-10-14: an empty 50-degree title for seconds).
            if (type == 0 && (VR::LampWarpDeparting() || VR::LampWarpArrivedWithin(10.0f))) {
                LOG_INFO(Core, "VR ui: an unknown text effect around a lantern warp: no banner");
                continue;
            }
            VR::PublishBanner(type);
            LOG_INFO(Core, "VR ui: the game plays a banner: text effect {} {}", type,
                     TextEffectName(type));
        }
    }
    g_effects.valid = true;
    g_effects.read = read;
    g_effects.write = write;
    for (s32 i = 0; i < TextEffectSlots; ++i) {
        g_effects.seen[i] = Read<u32>(man + OffsetTextEffects + 4 * i);
    }
}

// Test hook (VR_TEXT_EFFECT_FILE=<path>): a file there holding a type makes the game play that
// banner, through its own SetTextEffect (the event scripts' function) on this game thread; the
// file is removed. Checked twice a second.
void TestTextEffect() {
    static const char* const path = std::getenv("VR_TEXT_EFFECT_FILE");
    if (path == nullptr) {
        return;
    }
    static s64 next_ns = 0;
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (now < next_ns) {
        return;
    }
    next_ns = now + 500'000'000;
    s32 type = 0;
    {
        std::ifstream in(path);
        if (!in || !(in >> type)) {
            return;
        }
    }
    std::remove(path);
    u8* const site = g_base + SetTextEffectRva;
    if (std::memcmp(site, SetTextEffectSig.data(), SetTextEffectSig.size()) != 0 || type < 1 ||
        type > 17) {
        LOG_WARNING(Core, "VR ui: test banner {} not played (SetTextEffect {})", type,
                    Hook::HexBytes(site, SetTextEffectSig.size()));
        return;
    }
    LOG_INFO(Core, "VR ui: test banner: SetTextEffect({})", type);
    reinterpret_cast<SetTextEffectFn>(site)(nullptr, type);
}

} // Anonymous namespace

bool MenuIsMessageBox(u64 serial) {
    return serial == g_named_serial.load(std::memory_order_acquire) &&
           g_named_box.load(std::memory_order_relaxed);
}

void MenuEbootLoaded(VAddr base, u64 size) {
    g_base = reinterpret_cast<u8*>(base);
    g_size = size;
    g_enabled = VR::GetSettings().menu_mode && size > MenuSystemCopyRva + 8;
    g_ui_log = std::getenv("VR_UI_LOG") != nullptr;
    // It samples in the player update hook (installed before this): without it, never on.
    const bool hooked = IsPlayerHookInstalled();
    LOG_INFO(Core, "VR menu: {}{}",
             !g_enabled ? "menu_mode=0 or unknown eboot (off)"
             : !hooked  ? "detection OFF (the player update hook is not installed); gameplay "
                          "controls stay in menus"
             : VR::MenuPresentMode() == VR::MenuPresent::Layer && VR::GetSettings().ui_layer == 1
                 ? "detection on (menu controls, menus on the UI layer's canvas)"
             : VR::MenuPresentMode() == VR::MenuPresent::Layer
                 ? "detection on (menu controls and the menu panel: ui_layer=0)"
             : VR::MenuPresentMode() == VR::MenuPresent::Frozen
                 ? "detection on (menu controls and the menu panel)"
                 : "detection on (menu controls, menus on the HUD panel)",
             g_enabled && hooked && g_ui_log ? "; VR_UI_LOG on" : "");
    if (g_enabled) {
        InstallMenuOpenHook();
    }
    // The VR tile in the Options grid (vr_menu=1; its probe follows the cursor in MenuUpdate).
    VrTileEbootLoaded(base, size);
}

void MenuUpdate(u8* /*player*/) {
    if (!g_enabled) {
        return;
    }
    u8* const system = Read<u8*>(g_base + MenuSystemRva);
    if (Read<u8>(g_base + NewSystemRva) == 0) {
        Refuse("the game uses its old menu system");
        return;
    }
    if (!Plausible(system) || system != Read<u8*>(g_base + MenuSystemCopyRva) ||
        Read<u8*>(system) != g_base + MenuSystemVtableRva) {
        Refuse("menu system not found");
        return;
    }
    u8* const man = Read<u8*>(g_base + MenuManRva);
    if (!Plausible(man) || Read<u8*>(man) != g_base + MenuManVtableRva) {
        Refuse("menu manager not found");
        return;
    }
    if (g_refusal != nullptr) {
        g_refusal = nullptr;
        LOG_INFO(Core, "VR menu: detection on");
    }
    const bool open = Read<u64>(system + OffsetModalMenu) != 0;
    const u64 serial = Read<u64>(system + OffsetMenuSerial);
    if (open != g_open || (open && serial != g_serial)) {
        LOG_INFO(Core, "VR menu: {} (serial {})", open ? "open" : "closed", serial);
    }
    g_open = open;
    g_serial = serial;
    TestTextEffect();
    WatchTextEffects(man);
    VrTileMenuUpdate(open);
    // The named menu is still the open one only while the serial is the one its Open gave it
    // (a menu it opened, or closing it, changes the serial).
    const bool named = open && serial == g_named_serial.load(std::memory_order_acquire);
    // The Options grid: the cell its cursor is on (-1: not known), -2 for any other menu.
    const s32 grid_cell =
        named && g_named_grid.load(std::memory_order_relaxed) ? VrTileCursor() : -2;
    VR::PublishGameMenu(
        open, named ? g_named_zoom.load(std::memory_order_relaxed) : 1.0f,
        named ? static_cast<VR::UiClass>(g_named_class.load(std::memory_order_relaxed))
              : VR::UiClass::Full,
        named ? g_named_pitch.load(std::memory_order_relaxed) : 0.0f, grid_cell);
    // An NPC's line is playing (SprjWorldTalkMan +0x3FF8, 1 while it plays; anything but 0 or 1
    // is not trusted): the presenter's talk_guard holds swings back meanwhile. The conversation's
    // UI (an NPC subtitle in MenuMan's text window, the talk input cooldown) keeps it going
    // between lines and as it ends (VR::IsNpcTalking: only after a line, plus a grace).
    u8* const talk_man = Read<u8*>(g_base + TalkManRva);
    const bool talk_ok = Plausible(talk_man);
    const u8 talking = talk_ok ? Read<u8>(talk_man + OffsetTalking) : 0;
    const bool subtitle = Read<s32>(man + OffsetTextWindow) == 4;
    const bool cooldown = talk_ok && Read<float>(talk_man + OffsetTalkCooldown) > 0.0f;
    VR::PublishNpcTalking(talking == 1, subtitle || cooldown);
    VR::PublishNpcSubtitle(subtitle);
    // Only the HUD is up (the UI layer learns the HUD's draws then): no action prompt, no text
    // window, no NPC line or talk cooldown, no item pop-up.
    {
        u8* const item_get = Read<u8*>(g_base + ItemGetRva);
        const bool popup = Plausible(item_get) && Read<u8*>(item_get) == g_base + ItemGetVtableRva &&
                           Read<u64>(item_get + OffsetItemGetList) != 0;
        const bool prompt = !open && Read<s32>(man + OffsetPromptShown) == 1;
        VR::PublishActionPrompt(prompt);
        // The queue empties as soon as Scaleform accepts it; the popup remains
        // until its specific OK prompt (10010110) is dismissed.
        const bool pickup_ok = prompt && Read<s32>(man + OffsetPromptText) == 10010110;
        VR::PublishItemPickup(!open && (popup || pickup_ok));
        VR::PublishPlainHud(!open && !prompt &&
                            Read<s32>(man + OffsetTextWindow) == 0 && talking != 1 && !cooldown &&
                            !popup);
    }
    if ((talking == 1) != g_talking) {
        g_talking = talking == 1;
        if (g_talk_logs < 100) {
            ++g_talk_logs;
            LOG_INFO(Core, "VR menu: an NPC {}", g_talking ? "is talking" : "stopped talking");
        }
    }
    if (g_ui_log) {
        LogUi(man, open, serial);
    }
}

} // namespace VR::Bloodborne
