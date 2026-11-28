#include "controls_guide.h"
#include "core.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

#include "core/vr/controls_view.h"
#include "core/vr/headset_panels.h"
#include "core/vr/mirror_world.h"
#include "core/vr/monocular.h"
#include "core/vr/vr_page.h"
#include "core/vr/y_menu.h"

namespace ControlsGuide {
namespace {

using VR::ControlsView::Callout;
using VR::ControlsView::Config;
using VR::ControlsView::DrawnPart;
using VR::ControlsView::Family;
using VR::ControlsView::Tab;

// The game's on/off words (vr_session.cpp: "1" or "true"), else `fallback` when the key is absent.
bool Flag(const Lookup &value, const char *key, bool fallback) {
    const auto v = value(key);
    return v ? *v == "1" || *v == "true" : fallback;
}
float Number(const Lookup &value, const char *key, float fallback) {
    const auto v = value(key);
    float out = fallback;
    if (v && !v->empty())
        std::from_chars(v->data(), v->data() + v->size(), out);
    return std::isfinite(out) ? out : fallback;
}

Family FamilyOf(Platform platform) {
    switch (platform) {
    case Platform::Quest:
        break;
    case Platform::Psvr2:
        return Family::Sense;
    case Platform::Frame:
        return Family::Frame;
    case Platform::Index:
        return Family::Index;
    case Platform::Vive:
        return Family::Vive;
    case Platform::Wmr:
        return Family::Wmr;
    }
    return Family::Touch;
}
// Quest plays on Meta's runtime (Link, Air Link); the others on SteamVR (Windows MR too, since
// Windows 11 24H2 dropped Microsoft's own).
bool SteamVr(Platform platform) {
    return platform != Platform::Quest;
}
bool MenuRuntime(Platform platform, std::string_view runtime_name) {
    return SteamVr(platform) || (!runtime_name.empty() && VR::YMenu::RuntimeUsesY(runtime_name));
}
// SteamVR takes the trackpad d-pad maps (the Vive wands', Windows MR's).
std::vector<DrawnPart> PartsFor(Platform platform) {
    const Family family = FamilyOf(platform);
    return VR::ControlsView::PartsOf(family, VR::ControlsView::DefaultProfile(family),
                                     SteamVr(platform));
}
// The same, made once a platform (the settings page words every row each frame).
const std::vector<DrawnPart> &CachedParts(Platform platform) {
    static const std::vector<DrawnPart> all[]{PartsFor(Platform::Quest), PartsFor(Platform::Psvr2),
                                             PartsFor(Platform::Frame), PartsFor(Platform::Index),
                                             PartsFor(Platform::Vive),  PartsFor(Platform::Wmr)};
    static_assert(std::size(all) == PlatformCount);
    return all[static_cast<int>(platform)];
}

// The VR page's help with this platform's button names (vr_page.h RowHelp / RowLabel).
class LookupStore final : public VR::Page::Store {
public:
    LookupStore(const Lookup &value, const std::vector<DrawnPart> &parts)
        : value_{value}, parts_{parts} {}
    std::string ControlName(VR::Thumbs::Control physical) override {
        return VR::ControlsView::NameOf(parts_, physical);
    }
    std::string Get(const std::string &key) override {
        return value_(key).value_or(std::string{});
    }
    void Set(const std::string &, const std::string &) override {}
    void Remove(const std::string &) override {}

private:
    const Lookup &value_;
    const std::vector<DrawnPart> &parts_;
};

Section FromCallouts(std::string title, Tab tab, const Config &c,
                     const std::vector<DrawnPart> &parts, Family family) {
    Section s{std::move(title), VR::ControlsView::Note(tab, c, family), {}};
    for (const Callout &callout : VR::ControlsView::Build(tab, c, parts)) {
        Line line;
        if (callout.part >= 0 && callout.part < static_cast<int>(parts.size()))
            line.control = parts[static_cast<size_t>(callout.part)].name;
        line.title = callout.title;
        line.sub = callout.sub;
        line.help = callout.help;
        s.lines.push_back(std::move(line));
    }
    return s;
}

} // namespace

bool LeftHanded(const Lookup &value) {
    if (!Flag(value, "left_handed", false)) {
        return false; // right-handed: one lookup
    }
    const auto style = value("play_style");
    const auto first = value("first_person");
    const auto stereo = value("stereo");
    const auto mode = value("stereo_mode");
    const auto ui = value("ui_layer");
    return (!style || style->empty() || *style == "motion" || *style == "0") &&
           !(first && VR::Mirror::ThirdPersonWord(*first)) &&
           !(stereo && (*stereo == "0" || *stereo == "false")) &&
           (!mode || mode->empty() || *mode == "2") && (!ui || ui->empty() || *ui == "1");
}

Config ConfigFrom(const Lookup &value, bool menu_runtime) {
    Config c;
    c.layout.left_scheme =
        std::clamp(static_cast<int>(std::lround(Number(value, "left_scheme", 2.0f))), 1, 2);
    c.layout.classic = value("controls") == "classic";
    c.layout.swap = Flag(value, "swap_sticks", false);
    // Left-handed play (core/vr/mirror_world.h): the controllers trade hands.
    c.layout.mirror = LeftHanded(value);
    c.layout.inject = Flag(value, "inject", true);
    // The presenter's YMenuActive: motion scheme 2, on explicitly or auto on SteamVR / VDXR.
    int y_menu = 0;
    if (const auto v = value("y_menu"); v && VR::YMenu::ParseSetting(*v) >= 0)
        y_menu = VR::YMenu::ParseSetting(*v);
    c.layout.y_menu = VR::YMenu::Enabled(y_menu, menu_runtime,
                                       !c.layout.classic && c.layout.left_scheme == 2, true);
    c.layout.menu_runtime = menu_runtime;
    c.quick_arc = Flag(value, "quick_items", true) && Flag(value, "quick_arc", true);
    c.arc_l3 = Flag(value, "quick_arc_l3", true);
    c.belt_vial = Flag(value, "belt_vial", true);
    c.grip_hold = Flag(value, "quick_grip_hold", true);
    c.x_fan = value("x_button") != "vial";
    c.right_grip_draw = value("right_grip") == "draw";
    c.optimized_two_hand = Flag(value, "two_hand_optimized_controls", false);
    c.trigger_hold = Flag(value, "two_hand_trigger_hold", true);
    c.swings = Flag(value, "gestures", true);
    c.melee_physical = value("melee") != "vanilla";
    c.menu_hold_screen = value("menu_hold") == "screen";
    c.physical_evasion = Flag(value, "physical_evasion", true);
    c.lean_dodge = Flag(value, "lean_dodge", false) && !c.physical_evasion;
    c.grip_adjust = Flag(value, "body", true) && Flag(value, "grip_adjust", true);
    c.snap_turn_deg = Number(value, "snap_turn_deg", 30.0f);
    c.seated = Flag(value, "seated", false);
    return c;
}

std::vector<Section> Controls(Platform platform, const Lookup &value, std::string_view runtime_name) {
    const Config c = ConfigFrom(value, MenuRuntime(platform, runtime_name));
    const auto parts = PartsFor(platform);
    const Family family = FamilyOf(platform);
    return {FromCallouts("While playing", Tab::Gameplay, c, parts, family),
            FromCallouts("In menus and VR pages", Tab::Menus, c, parts, family),
            FromCallouts("Gestures", Tab::Gestures, c, parts, family)};
}

std::vector<DiagramButton> Diagram(Platform platform, const Lookup &value, bool menus,
                                   std::string_view runtime_name) {
    using VR::ControlsView::Part;
    const auto& parts = CachedParts(platform);
    const Config config = ConfigFrom(value, MenuRuntime(platform, runtime_name));
    std::vector<DiagramButton> out;
    for (const auto& callout : VR::ControlsView::Build(
             menus ? Tab::Menus : Tab::Gameplay, config, parts)) {
        if (callout.part < 0 || callout.part >= static_cast<int>(parts.size())) continue;
        const auto& part = parts[callout.part];
        // Fractions of the actual embedded artwork, not the old schematic's coordinates.
        // Right-hand anchors are mirrored except Sense, whose two views are cropped separately.
        float x = .5f, y = .5f;
        switch (part.part) {
        case Part::ButtonHigh: x = .65f; y = .37f; break;
        case Part::ButtonLow: x = .54f; y = .33f; break;
        case Part::Stick: x = .68f; y = .22f; break;
        case Part::Pad: x = .76f; y = .237f; break;
        case Part::Menu: x = .50f; y = .26f; break;
        case Part::Trigger: x = .90f; y = .47f; break;
        case Part::Grip: x = .74f; y = .67f; break;
        case Part::FaceLeft:
        case Part::FaceRight: break; // a gamepad's (never a VR controller's)
        }
        if (platform == Platform::Index) {
            switch (part.part) {
            case Part::ButtonHigh: x = .83f; y = .255f; break;
            case Part::ButtonLow: x = .75f; y = .28f; break;
            case Part::Stick: x = .68f; y = .18f; break;
            case Part::Trigger: x = .89f; y = .37f; break;
            case Part::Grip: x = .47f; y = .60f; break;
            default: break;
            }
        } else if (platform == Platform::Frame) {
            switch (part.part) {
            case Part::ButtonHigh: x = part.right ? .21f : .28f; y = part.right ? .15f : .19f; break;
            case Part::ButtonLow: x = part.right ? .19f : .17f; y = part.right ? .24f : .29f; break;
            case Part::Stick: x = .29f; y = .38f; break;
            case Part::Menu: x = .43f; y = .22f; break;
            case Part::Trigger: x = .86f; y = .40f; break;
            case Part::Grip: x = .40f; y = .75f; break;
            default: break;
            }
        } else if (platform == Platform::Psvr2) {
            switch (part.part) {
            case Part::ButtonHigh: x = .76f; y = .077f; break;
            case Part::ButtonLow: x = .71f; y = .128f; break;
            case Part::Stick: x = .63f; y = .059f; break;
            case Part::Trigger: x = .91f; y = .72f; break;
            case Part::Grip: x = .63f; y = .77f; break;
            default: break;
            }
        }
        if (part.right) x = 1 - x;
        out.push_back({part.right, x * 240, y * 440,
                       {part.name, callout.title, callout.sub, callout.help, {}, nullptr}});
    }    return out;
}

std::vector<Section> Interactions(Platform platform, const Lookup &value, std::string_view runtime_name) {
    const auto parts = PartsFor(platform);
    LookupStore store{value, parts};
    const bool swap = VR::Page::SticksSwapped(store);
    // The guide's words with this controller's names: {A} {B} {X} {Y} and {move} {turn} as the
    // layout has them, {button:B} the button itself (thumb_layout.h Expand).
    const bool mirror = VR::Page::PageMirror(store); // left-handed play
    const auto say = [&](const std::string &text) {
        return VR::Thumbs::Expand(
            text, swap,
            [&](VR::Thumbs::Control physical) {
                return VR::ControlsView::NameOf(parts, physical);
            },
            mirror);
    };
    const bool menu_runtime = MenuRuntime(platform, runtime_name);
    const Config c = ConfigFrom(value, menu_runtime);
    // Match the presenter's OptimizedTwoHand and the controller page's EntryWords.
    const bool motion_grip = !c.layout.classic && !c.right_grip_draw;
    const bool optimized = c.optimized_two_hand && motion_grip;
    const bool scheme2 = !c.layout.classic && c.layout.left_scheme == 2;
    const bool x_fan = scheme2 && c.layout.inject && c.x_fan;
    // The game's menu: Menu, or Y where the runtime owns Menu (y_menu); classic and scheme 1 on
    // SteamVR have no button for it (the controller's tab says so).
    const std::string menu_button = c.layout.y_menu    ? "{Y}"
                                    : menu_runtime ? std::string{}
                                                        : "Menu";
    std::vector<Section> out;
    const auto section = [&](const char *title) { out.push_back({title, {}, {}}); };
    // Left-handed play: the guide's words about your hands and body for the other side (its
    // buttons' names come mirrored from say).
    const auto sided = [&](std::string_view text) {
        return mirror ? VR::Mirror::SideWords(text) : std::string{text};
    };
    const auto line = [&](const char *title, const std::string &help, const char *words,
                          const char *row = nullptr) {
        out.back().lines.push_back({{}, sided(title), {}, say(sided(help)), words, row});
    };

    section("Weapons");
    line("Switch weapons",
         "Put your right hand on your right shoulder and tap {B} for your next right-hand weapon. "
         "Put your left hand on your left shoulder and tap {Y} for your next left-hand weapon. "
         "Each tap moves to the next weapon you have equipped in that hand.",
         "weapon switch swap change shoulder cycle next equip right left", "shoulder_switch");
    line("Go unarmed",
         "At a shoulder, hold the button for a moment to put that hand's weapon away. Tap it again "
         "to get the same weapon back. With bare hands you can push and grab dead enemies (see "
         "Bare hands).",
         "unarmed fists bare hands put away holster sheathe empty", "shoulder_switch");
    line("Attack",
         "Just swing the right controller, no button needed. A quick swing is a light attack. "
         "Wind up behind your shoulder (you feel a soft click) and swing through a big arc for a "
         "heavy attack. A dull buzz means you're swinging faster than the weapon can keep up.",
         "attack swing light heavy melee hit combo slash windup");
    line("Charged attack",
         "With most melee weapons, hold the right trigger until a tick makes your next swing "
         "heavy. Keep holding until the triple buzz for a fully charged attack, then swing. Some "
         "transformed weapons use the trigger for a special instead.",
         "charge charged heavy right trigger r2 power");
    line("Weapon weight",
         "Your weapons have weight. Heavy ones take more effort to get moving and keep going "
         "after a swing. Pick Light in VR settings if you want them easier to move.",
         "weight weighted heavy light momentum feel floaty", "weighted_weapons");
    line("Transform",
         optimized ?
         "Press and release {turn_click} to transform your trick weapon. Keep the stick centred. "
         "The bow, Threaded Cane and Beast Cutter transform on the click, with no hold delay." :
         !motion_grip ? "Squeeze the left grip to transform. With the Threaded Cane or Beast "
                        "Cutter, hold it briefly and release it before changing back." :
         "Squeeze the right grip firmly to transform your trick weapon. Only the weapon moves, so "
         "you can keep walking. Squeeze it mid-swing for a transform attack. Threaded Cane and "
         "Beast Cutter: hold the grip for a moment.",
         "transform trick weapon form right grip flick whip cane cutter");
    if (motion_grip && !optimized) {
        line("Transform by hand",
             "Hunter Axe, Kirkhammer, Ludwig's Holy Blade, Burial Blade, Chikage and Rifle Spear: "
             "squeeze the right grip, then grab the handle with your left hand. The weapon stays in "
             "its two-handed form while both hands hold it. Let go with your left hand to change "
             "back.",
             "transform two handed axe kirkhammer ludwig burial blade chikage rifle spear handle");
        line("Other transforms",
             "Bloodletter: hold the right grip and stab it into your chest. Tonitrus: hold the right "
             "grip and raise it over your head to charge it. Beast Claw: hold the right grip for "
             "2 seconds. Whirligig Saw: grab its handle with your left hand.",
             "bloodletter tonitrus beast claw whirligig saw transform special");
    }
    line("Boom Hammer",
         motion_grip && !optimized
             ? "Squeeze and release the right grip repeatedly until the furnace catches. Then "
               "hold the grip, keep pumping, or strike the head with your left hand to light it. "
               "You can also light it early with a strike just after a starter click. The flame "
               "lasts for one damaging hit. Squeeze the grip again to put it out early."
             : "Use your trick weapon transform to light the furnace. The flame lasts for one "
               "damaging hit. With Original motion controls and the right grip set to Transform, "
               "you can also pump the right grip and strike the head to light it.",
         "boom hammer furnace fire ignite starter rev pump strike");
    line("Weapon specials",
         std::string("Reiterpallasch, transformed: pull the right trigger to shoot from the tip. Rifle Spear, "
         "transformed: hold the right trigger until the triple buzz, then let go for the blast. "
         "Stake Driver, transformed: hold the right trigger for a second, then let go. A charged "
         "blast that hits returns it to its compact form. Holy Moonlight Sword, transformed: "
         "after a full charge, the wave flies the moment your blade sweeps across where you're "
         "facing, and hits the enemy in front of you. If a swing never crosses in front of you, "
         "let go of the trigger mid-swing to send it along the blade (an enemy in front of you "
         "still takes it). ") +
         (optimized ? "Whirligig Saw in both hands: hold the right trigger to spin it. "
                      "Bloodletter, and the Kos Parasite with the Milkweed rune, in two-handed "
                      "form: the right trigger uses their special attack."
                    : "Whirligig Saw in both hands: hold a trigger to spin it."),
         "reiterpallasch rifle spear stake driver moonlight sword wave whirligig saw spin shoot "
         "blast special");
    if (!c.layout.classic) {
        line("Bloodletter",
             std::string("With physical combat, transform it into the flail. Tap the ") +
                 (optimized ? "right" : "left") +
                 " trigger for its blood blast. For a physical slam, hold that trigger until you feel "
                 "the heartbeat, then keep holding and bring the flail down against the floor or an enemy.",
             "bloodletter flail blood blast slam heartbeat trigger");
    }
    line("Amygdalan Arm",
         "With physical combat, transform the Arm and swing to crack its tentacle in the "
         "direction of your swing. Keep swinging to keep it out. Press the right trigger with "
         "your hand still to jab straight ahead. Chop down hard from above to slam it into the "
         "ground in front of you.",
         "amygdalan arm whip tentacle lash jab crack slam chop trigger");
    line("Amygdalan Arm hook",
         "Hold the right trigger during a lash or jab to catch an enemy. After the initial "
         "swing, pull back, step back, or swing away to topple an enemy light enough to move. "
         "Swing it around, then release the trigger to throw it. The tentacle can latch onto "
         "heavy enemies and bosses, but it cannot pull them.",
         "amygdalan arm hook tentacle catch yank throw drag ragdoll trigger");
    line("Kos Parasite",
         std::string("With physical combat and the Milkweed rune, swing to lash the parasite's "
                     "tentacle. Transformed, each hand grows three tentacles and lashes with its "
                     "own swing. Pull the left trigger to spit poison where you look. ") +
             (optimized ? "Transformed, tap the right trigger to spit and hold it for the ritual. "
                        : "Transformed, tap the left trigger to spit and hold it for the ritual. ") +
             "Without the rune, swing to punch.",
         "kos parasite tentacle tendril milkweed spit poison ritual punch fist");
    line("Simon's Bowblade",
         std::string(optimized ? "Press {turn_click} to open the bow: "
                     : motion_grip ? "Hold the right grip for half a second to open the bow: "
                                   : "Squeeze the left grip to open the bow: ") +
         "the bow is in your left hand and "
         "an arrow in your right. Bring the arrow to the bow, squeeze the right trigger to nock "
         "it and pull back. Let go of the trigger to shoot; hold full draw until the triple buzz "
         "for a charged shot. Reach over your right shoulder and pull the right trigger for a new "
         "arrow. " + (optimized ? "Press {turn_click} again to close the bow."
                     : motion_grip ? "Squeeze the right grip to close the bow."
                                   : "Squeeze the left grip to close the bow."),
         "bow arrow bowblade simon draw nock shoot quiver archery");
    line("Grip adjust",
         "Stand still and hold {A}+{B} for about a second: the weapon freezes in place. Hold the "
         "controller the way you like and let go to keep it. Pull the right trigger while it's "
         "frozen to reset it.",
         "grip adjust weapon angle hold rotate fit controller reset", "grip_adjust");
    line("Solid weapons",
         "Your weapons stop at walls, floors and each other, with a thud and sparks, so you can't "
         "swing through a wall.",
         "solid weapons walls collision clip through sparks", "weapon_wall");
    line("Blade sounds",
         "Tap your blade against your gun or your other blade to make it ring. Slide one along the "
         "other to sharpen it.",
         "blade sounds ring clang sharpen tap", "blade_sounds");

    section("Fighting");
    line("Shoot and parry",
         "Point the gun in your left hand and pull the left trigger. It fires where the barrel "
         "points, not where you look. Headshots do extra damage. Shoot an enemy just as it "
         "attacks to parry it.",
         "gun pistol blunderbuss shoot fire aim parry headshot");
    line("Aiming line",
         "Turn on Gun laser in VR settings for a thin line from the barrel while you aim.",
         "laser aim line sight pointer gun", "laser");
    line("Reload",
         "After a shot, release the trigger. Snap the muzzle up and lower it slightly, or flick "
         "it down more sharply and let it rebound. A click confirms the reload. With the Cannon "
         "or Church Cannon, let the recoil settle first. You can turn wrist-flick reload off "
         "in VR settings.",
         "reload flick wrist gun bullets click empty", "gun_wrist_reload");
    line("Visceral attack",
         "After a parry, or a charged hit from behind, you feel a heartbeat in your right hand. "
         "Thrust your blade straight into the enemy; a normal swing only hits it. The world holds "
         "still with your blade inside. Pull your hand back out whenever you like to rip it free "
         "and throw the enemy back.",
         "visceral riposte critical backstab parry stab thrust rip heartbeat", "riposte_style");
    line("Deflect bullets",
         "Hold your blade in a bullet's path to block it; it costs a little stamina. Swing at it "
         "on purpose to try to knock it back at the shooter. The better your aim, the better the "
         "chance.",
         "deflect bullet block reflect gunshot swat");
    line("Plunge attack",
         "Swing as you drop off a ledge, or hold your blade point down while you fall, to hit "
         "whatever you land on. A bigger drop hits harder.",
         "plunge falling drop attack jump down ledge stab");
    line("Dodge with your body",
         "Let the blade pass you by. Lean left, right or back, step aside, or bend your knees "
         "to duck under a swing. In standing Motion controls, a quick lean or duck just "
         "before impact can make you briefly invulnerable. No button press needed. "
         "You can still press {A} for a regular dodge.",
         "lean left right backward duck dodge evade sidestep crouch body invulnerable");
    line("Ragdoll kills",
         "Enemies you kill with your blade go limp and fly from the force of your swing. Their "
         "bodies stay physical: you can walk into them or grab them with bare hands.",
         "ragdoll kill death limp physics body", "ragdoll");

    section("Two hands and two weapons");
    if (optimized) {
        line("Two-hand a weapon",
             std::string("First transform with {turn_click}, then bring your left hand to the handle. ") +
             (c.trigger_hold ? "Hold the left trigger to grab; release it to let go. "
                             : "Press the left trigger to grab; press it again to let go. ") +
             "Hold either grip to slide that hand; release the grip to catch the handle again. "
             "Letting go with the left hand keeps the weapon transformed. Swinging a two-handed "
             "weapon with one hand deals half melee damage.",
             "two handed both hands grip handle slide spear polearm left hand", "two_handed");
    } else if (motion_grip) {
        line("Two-hand a weapon",
             "When the weapon is in a two-handed form, put your left hand on the handle and squeeze "
             "the left grip. Both hands now steer the weapon. Let go of both grips to release it. "
             "Hold {button:B} to slide your hands along the handle.",
             "two handed both hands grip handle slide left hand", "two_handed");
        line("Spear grip",
             "On long weapons, hold {button:Y} to loosen your front hand and slide the weapon "
             "through it like a spear.",
             "spear polearm long weapon slide thrust front hand");
    } else {
        line("Two-hand a weapon",
             "Use the left grip to transform. For physical two-handed grabs and sliding, use "
             "Motion controls with the right grip set to Transform.",
             "two handed both hands grip handle slide left hand", "two_handed");
    }
    if (motion_grip) {
        line("Grip guide",
             optimized ? "In a two-handed form, a faint glow shows where your left hand can grab "
                         "the handle; no right-grip hold is needed." :
             "Hold the right grip to see a faint glow where your left hand can grab the weapon.",
             "grip guide glow where grab two hand handle", "two_hand_guide");
        line("Logarius' Wheel",
             optimized ? "Transform with {turn_click}, then use the left trigger to take the "
                         "second handle across the wheel. Use Original two-handed controls for "
                         "the wheel's left-trigger spin buffs." :
             "Squeeze the right grip, then grab the second handle across the wheel with your "
             "left grip. With both hands on the wheel, tap the left trigger to spin it. Each tap "
             "adds one of five buff steps. The right trigger still charges attacks.",
             "logarius wheel spin rev buff second handle two hands");
    }
    line("Dual blades",
         std::string(
             optimized ? "Blade of Mercy and Rakuyo: press and release {turn_click} to split or "
                         "join the blades. When split, each hand swings its own blade." :
             !motion_grip ? "Blade of Mercy and Rakuyo: squeeze the left grip to split or join "
                            "the blades." :
                            "Blade of Mercy and Rakuyo: squeeze the right grip, grab the second "
                            "hilt next to your right hand with your left grip, and pull your hands "
                            "apart. Each hand then swings its own blade. To join them again, hold "
                            "the blades side by side and squeeze the left grip, or squeeze the "
                            "right grip.") +
             " Split, rub the blades together four times quickly to hone them: a ding, and each "
             "blade's next hit lands harder.",
         "dual wield blades mercy rakuyo split two weapons offhand hone sharpen rub");
    line("Left-hand attacks",
         "Swing your left hand to attack with what it holds: a second blade, the Beast Claw, a "
         "torch or your fist. Left-hand swings are light attacks. With a pistol in your left hand, "
         "a punch is a fist attack. With a torch or bare fist, pull the left trigger once first.",
         "left hand attack dual wield offhand torch fist punch claw");
    line("Shields",
         "Hold a Wooden Shield or Loch Shield up in front of you to block.",
         "shield block wooden loch guard");
    line("Torch",
         "Raise the torch to your shoulder or above your head for a bigger flame and more light.",
         "torch fire light flame raise bright dark");

    section("Bare hands");
    line("Your fingers",
         "Go unarmed and your fingers follow yours: the thumb on the buttons, the index finger on "
         "the trigger, the rest with the grip. Squeeze the grip to make a fist.",
         "bare hands fingers unarmed hand pose point fist");
    line("Push corpses",
         "Touch a dead enemy with a bare hand to push it around. Walking into a body pushes it "
         "too.",
         "corpse body ragdoll push shove dead enemy");
    line("Grab and drag corpses",
         "Squeeze the grip on a dead enemy to grab it. Lift it, drag it or throw it, and let go "
         "to drop it. Your Strength stat helps you lift heavier bodies, up to 42 Strength. Use "
         "both hands for a heavy body, or grab a limb and drag it. Walk to drag it along. If you "
         "run off, it slips out of your hand.",
         "corpse body ragdoll grab drag lift carry throw dead enemy strength");

    section("Items");
    std::string fan = "Squeeze the left grip for a moment";
    if (x_fan && !c.layout.y_menu)
        fan += ", press {X}, or tap {Y}";
    else if (x_fan)
        fan += " or press {X}";
    else if (!c.layout.y_menu && scheme2)
        fan += " or tap {Y}";
    fan += " to open your quick items on a fan around your left hand. Move your hand onto an "
           "item until it lights up with its name. ";
    fan += c.grip_hold ? "Squeeze the grip again to take and hold it. Pull the left trigger to "
                         "use it. "
                       : "Squeeze the left trigger to take and hold it. ";
    fan += "Pull your hand back or push it away to close the fan. With nothing in your hand, "
           "the left trigger fires your gun as usual.";
    if (!scheme2)
        fan = "Press {Y} to open your quick items above your hand. Reach an item and squeeze the "
              "grip to take it. Pull the left trigger to use it.";
    if (c.quick_arc && c.grip_hold)
        fan += " Putting an item back, using it or throwing it closes the fan. Open it again "
               "for the next one.";
    line("Quick item fan", fan,
         "quick items fan menu inventory consumables select pick use", "quick_arc_pick");
    line("Throw an item",
         std::string(scheme2 && !c.grip_hold ? "Hold the item with the trigger, swing, and let go "
                                              "of the trigger to throw it. "
                                            : "Hold the item with the grip, swing, and let go "
                                              "of the grip to throw it. ") +
             "A harder throw goes further, and knives get a little aim help. Let go gently to "
             "put it back instead.",
         "throw toss molotov knife pebble urn item");
    line("Pockets",
         "Your left thigh pocket and your chest pocket hold quick items 1 and 2. Squeeze the grip "
         "there to take them without opening the fan.",
         "pocket holster thigh chest quick slot item", "holster_mode");
    if (c.layout.inject) {
        line("Blood Vial",
             std::string("Squeeze the left grip at the vial pouch on your belt, or reach over your "
                         "right shoulder and squeeze") +
                 (x_fan ? "" : ", or press {X}") +
                 ". Then stab it into your wrist, arm, chest, belly or thigh to heal. You feel a "
                 "thump when it works, and a double tick when it can't (full health or no vials "
                 "left). Let go to put it back. It works mid-fight too: a quick fist at the pouch "
                 "grabs a vial without firing your gun.",
             "blood vial heal health potion inject stab wrist pouch belt shoulder", "inject");
    } else {
        line("Blood Vial", "Press {X} to drink a Blood Vial, as in the original game.",
             "blood vial heal health potion drink", "inject");
    }
    line("Hunter tools",
         "A hunter tool you take from the fan stays in your hand for its next use and glows when "
         "it's ready again. No animation plays. Most are aimed and fired with the left trigger "
         "(see the item list). A tool you don't have enough bullets for is dimmed.",
         "hunter tools augur beast roar tonitrus gloves call beyond blacksky");
    line("Monocular",
         std::format("Take it from the fan and bring it to the eye you want to look through. "
                     "Twist your wrist to zoom up to {:.2f}x. Hold the trigger as you twist to "
                     "reach {:.2f}x.", VR::Monocular::FineMaxZoom, VR::Monocular::MaxZoom),
         "monocular zoom telescope spyglass scope look far");
    line("Hand Lantern",
         "Take it from the fan to light your way. It swings as you move. Pull the trigger or "
         "shake it to switch the light. Put it back and it hangs at your hip.",
         "lantern light lamp dark carry hip");
    line("Tiny Music Box",
         "Take it from the fan; your right hand is free while you hold it. Grab its little crank "
         "with your right grip and turn it a full turn to play the tune; keep turning to play it "
         "again. Let go of the left grip to put it away and your weapon comes back.",
         "music box tiny crank tune gascoigne play song");
    line("Bells",
         "Beckoning, Small Resonant and Sinister Bells: give them three quick shakes, or hold the "
         "trigger until the ring fills. They don't work offline.",
         "bell beckoning resonant sinister co-op summon ring shake online");

    section("Every item");
    struct Item {
        const char *name, *how, *words;
    };
    static constexpr Item items[]{
        {"Molotov Cocktail, Rope Molotov", "Fire bombs. Throw them.",
         "molotov rope fire bomb throw"},
        {"Throwing Knife", "Small damage from range. Throw it; knives aim well.", "knife throw"},
        {"Poison Knife", "Poisons enemies. Throw it.", "poison knife throw"},
        {"Pebble", "Gets an enemy's attention. Throw it.", "pebble stone lure throw"},
        {"Oil Urn", "Soaks enemies in oil so fire hurts more. Throw it, then follow up with fire.",
         "oil urn throw fire"},
        {"Pungent Blood Cocktail", "Lures enemies away. Throw it.", "pungent cocktail lure throw"},
        {"Numbing Mist", "Stops enemies from healing. Throw it.", "numbing mist throw heal"},
        {"Shining Coins", "Mark your path. Throw them.", "shining coins mark throw"},
        {"Accursed Brew", "An arcane skull. Hold the trigger, swing, and let go of the trigger "
                          "to throw it.",
         "accursed brew skull arcane throw curse"},
        {"Beast Blood Pellet", "Boosts your attack for a while. Bring it to your mouth.",
         "beast blood pellet eat mouth buff"},
        {"Antidote", "Cures poison. Bring it to your mouth.", "antidote poison eat mouth cure"},
        {"Sedative", "Calms frenzy. Drink it at your mouth.", "sedative frenzy drink mouth"},
        {"Blue Elixir", "Enemies lose sight of you. Drink it at your mouth.",
         "blue elixir stealth hide drink mouth"},
        {"Lead Elixir", "Makes you heavy and tough, but slow. Drink it at your mouth.",
         "lead elixir heavy defense drink mouth"},
        {"Blood of Arianna, Adella, Adeline", "Heals you. Drink it at your mouth.",
         "arianna adella adeline blood drink heal npc"},
        {"Iosefka's Blood Vial", "Heals more than a Blood Vial. Drink it at your mouth.",
         "iosefka blood vial drink heal"},
        {"Fire Paper, Bolt Paper", "Fire or bolt on your weapon for a while. Rub it along the "
                                   "blade.",
         "fire paper bolt paper buff blade rub"},
        {"Empty Phantasm Shell", "Arcane on your weapon for a while. Rub it along the blade.",
         "phantasm shell arcane buff blade rub"},
        {"Bone Marrow Ash", "Your next gunshot hits much harder. Hold the trigger until the ring "
                            "fills.",
         "bone marrow ash gun shot buff"},
        {"Coldblood Dew", "Gives Blood Echoes. Squeeze the trigger hard to crush it.",
         "coldblood dew echoes crush"},
        {"Madman's Knowledge", "Gives Insight. Squeeze the trigger hard to crush it.",
         "madman knowledge insight crush skull"},
        {"Hunter's Mark, Bold Hunter's Mark",
         "Takes you back to the last lamp. Hold the trigger until the ring fills.",
         "hunter mark bold return lamp"},
        {"One Third of Umbilical Cord", "Gives Insight. Hold the trigger until the ring fills.",
         "umbilical cord insight"},
        {"Old Hunter Bone", "Quick, long dodges for a while. Squeeze the trigger hard to crush it.",
         "old hunter bone quickening dodge crush"},
        {"Shaman Bone Blade", "Turns an enemy against its friends. Take it from the fan and "
                              "stab or slash an enemy with it; no trigger needed. Some enemies "
                              "can't be turned.",
         "shaman bone blade ally stab slash contact charm"},
        {"Choir Bell", "Heals you and your allies. Shake it to ring.", "choir bell heal shake"},
        {"Augur of Ebrietas", "Point it and hold the trigger for half a second to crush it: it bursts, "
                              "then the tentacles shoot from your hand where it points. Let go early to "
                              "cancel. Or eat it at your mouth: they burst out of you, where you look.",
         "augur ebrietas tentacles hunter tool"},
        {"Beast Roar", "Knocks enemies back. Hold the trigger: the claw clenches into a fist, then "
                       "bursts open and roars. Let go early to cancel.",
         "beast roar knock back hunter tool"},
        {"Messengers' Gift", "Turns you into a Messenger. Pull the trigger.",
         "messengers gift disguise hunter tool"},
        {"Blacksky Eye", "Fires meteors. Aim it like a gun, or hold it to your eye and fire where "
                         "you look.",
         "blacksky eye meteor hunter tool"},
        {"A Call Beyond", "Hold the trigger to gather the stars, then raise the tool above your "
                          "head to cast. The world loses colour as the stars gather, flashes as "
                          "you lift it, and regains colour once they launch.",
         "call beyond stars hunter tool"},
        {"Tiny Tonitrus", "Lightning along the ground. Hold the trigger until it crackles, then "
                          "strike down: the lightning goes where you look, or at the enemy you're "
                          "locked on to. Slash to the side to send it that way. Lifting it to "
                          "wind up doesn't fire it.",
         "tonitrus lightning hunter tool aim charge"},
        {"Executioner's Gloves", "Hold the trigger until the spirits are fully gathered. Swing "
                                 "and release either the trigger or grip to throw the glove. "
                                 "Release gently to drop it. The spirits rise where it lands "
                                 "and seek an enemy. You can also strike an enemy with the "
                                 "charged glove still in your hand.",
         "executioner gloves spirits hunter tool"},
        {"Madaras Whistle", "Calls a giant snake. Hold it to your mouth and hold the trigger.",
         "madaras whistle snake hunter tool"},
        {"Blood Bullets", "Reach to your left shoulder with your right hand and squeeze the grip "
                         "to draw an empty syringe. Stab your left arm, torso or leg to trade "
                         "30% of your max HP for five blood bullets. You can also hold {turn} up "
                         "for half a second.",
         "blood bullets health ammo quicksilver syringe draw"},
    };
    for (const Item &item : items)
        line(item.name, item.how, item.words);

    section("The world");
    line("Pick up items",
         "Press {B} at an item to pick it up. No crouch: the item and the game's pop-up appear at "
         "once.",
         "pick up pickup loot item collect search");
    line("Doors, chests and lamps",
         "In first person with Immersive VR interactions, press {B} to open doors, gates, chests "
         "and coffins, light a lamp or knock on a resident's door without stopping. Flat plays "
         "the hunter's usual animations. Changes to lamps and knocking apply on the next launch.",
         "door gate chest coffin lamp light open knock resident interaction", "vr_interactions");
    line("Ladder climbs",
         "Start a climb normally. With Scenes on a screen and Skip ladder climbs on, long "
         "climbs fade to black and speed up until you are near the end. A hit or fall returns "
         "control immediately. Skip ladder climbs is on by default.",
         "ladder climb skip fade black comfort", "ladder_skip");
    line("Levers",
         "Grab a lever's handle with either hand and pull it. Pull it more than halfway and it "
         "finishes on its own; let go early and it swings back. On a tall lever your hand slides "
         "down the handle as you pull. A lever too high to reach keeps its button prompt.",
         "lever pull elevator lift gate shortcut handle", "physical_levers");
    line("Lamps",
         "Use a lit lamp to open the lamp menu: rest, travel to another lit lamp, or go to the "
         "Hunter's Dream. Offline only.",
         "lamp rest travel fast travel dream teleport", "lamp_travel");
    line("Summon an NPC",
         "At an NPC's summon sign, press {B} to take out the bell, then shake it to summon them. "
         "{B} again cancels.",
         "summon npc sign bell help co-op", "physical_summon_bell");
    line("Gesture menu",
         scheme2 ? "Hold {Y} and press {B} to open the game's gestures."
                 : "Hold the {move_click} and press {B} to open the game's gestures.",
         "gesture emote menu wave bow");
    line("Body gestures",
         "At the Brain of Mensis, hold both hands high over your head for about 10 seconds (Make "
         "Contact). In front of the Doll you can bow your head, wave, point or raise a hand, and "
         "she answers. Only gestures you've learned work.",
         "gesture make contact brain mensis doll bow wave point emote pose");
    line("The Doll's respec",
         "Choose Rechannel Blood Echoes when speaking to the Doll. Lower attributes as far as "
         "your origin's starting values, then place every freed level elsewhere before "
         "confirming. Redistributing at the same level costs nothing. Back out to keep your "
         "current build.",
         "doll respec rechannel blood echoes stats attributes redistribute level", "doll_respec");
    line("Gehrman's wheelchair",
         "While Gehrman is seated in the Hunter's Dream, grip the chair with a bare hand and "
         "push or steer it.",
         "gehrman wheelchair chair push steer");

    section("Moving");
    line("Move and turn",
         "The {move} moves you and the {turn} turns you. Hold the {turn} down to turn around. "
         "Snap or smooth turning is in VR settings.",
         "move walk turn snap smooth stick 180 around", "turning");
    line("Sprint and leap",
         "Click the {move} to sprint; you keep sprinting until you let go of the stick. Click it "
         "again, or press {A}, to leap.",
         "sprint run leap jump dash");
    line("Dodge and roll",
         "Press {A} to dodge. Press {A} and {B} together to roll.",
         "dodge roll backstep quickstep evade");
    line("Lock on", optimized ?
         "{turn_click} transforms instead of locking on with optimized controls. Choose Original "
         "two-handed controls to use stick-click lock-on." :
         "Click the {turn} to lock on to an enemy. Click it again to let go.",
         "lock on target focus camera");
    line("Next item", "Flick the {turn} down for your next quick item.",
         "next item switch quick item cycle");

    section("The VR menu");
    line("Health and stamina",
         "Look at your left wrist to see your health, stamina and bullets. Look down at your feet "
         "for Blood Echoes and Insight.",
         "health stamina bullets wrist gauge hud echoes insight", "hud_vitals");
    line("VR settings",
         "Open the game's menu" + (menu_button.empty() ? "" : " (" + menu_button + ")") +
             " and pick the VR tile. Its tabs change how all of this works.",
         "vr settings options menu tile page");
    if (!menu_button.empty()) {
        line("Recenter",
             "Hold " + menu_button + " for a moment: forward becomes the way you're facing.",
             "recenter reset view forward center direction");
        line("Skip a cutscene",
             "Tap " + menu_button + " during a cutscene to open the game's skip prompt.",
             "skip cutscene cinematic intro scene menu");
    }
    line("Free camera",
         "VR settings > You > Free camera: fly around while the hunter waits and enemies ignore "
         "him. {Y} brings you back.",
         "free camera fly photo spectator");
    line("Soft save",
         "VR settings > Save > Soft save keeps this moment. After a death you can go back to it or "
         "to the last lamp. Using a lamp clears it.",
         "soft save checkpoint death restore quick save");
    return out;
}

bool Finds(const Line &line, const std::string &needle) {
    std::string text = line.control + " " + line.title + " " + line.sub + " " + line.help + " " +
                       line.words;
    for (auto &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return Launcher::SearchFinds(needle, text);
}

Platform PlatformOf(const std::string &system_name) {
    const VR::Headsets::Headset *headset = VR::Headsets::FromSystemName(system_name);
    if (!headset)
        return Platform::Quest;
    if (headset->id == "psvr2")
        return Platform::Psvr2;
    if (headset->id == "steamframe")
        return Platform::Frame;
    if (headset->id == "index")
        return Platform::Index;
    return Platform::Quest; // the Quest, PICO, Reverb G2, Vive Focus...: Touch's buttons
}

RowText RowWords(const char *id, Platform platform, const Lookup &value) {
    const int at = id ? VR::Page::RowIndex(id) : -1;
    if (at < 0)
        return {};
    LookupStore store{value, CachedParts(platform)};
    const VR::Page::Row &r = VR::Page::Rows()[static_cast<size_t>(at)];
    return {VR::Page::RowLabel(r, store), VR::Page::RowHelp(r, store)};
}

std::string PlatformNote(Platform platform, bool left_handed) {
    switch (platform) {
    case Platform::Quest:
        // (Left-handed play: the game's Y is your right controller's B.)
        if (left_handed) {
            return "Quest Link / Air Link: Menu opens the game's menu. SteamVR or Virtual "
                   "Desktop's VDXR: with the default motion controls, tap B for the game menu or a "
                   "cutscene's skip prompt; hold B for 0.7 seconds to recenter. PICO, Vive Focus "
                   "and HP Reverb G2 controllers have the same buttons.";
        }
        return "Quest Link / Air Link: Menu opens the game's menu. SteamVR or Virtual Desktop's "
               "VDXR: with the default motion controls, tap Y for the game menu or a cutscene's "
               "skip prompt; hold Y for 0.7 seconds to recenter. PICO, Vive Focus and HP "
               "Reverb G2 controllers have the same buttons.";
    case Platform::Psvr2:
        return "PS VR2 plays through SteamVR (the PlayStation VR2 app and its PC adapter), which "
               "gives the game the Sense controllers in the Quest's layout: Cross where A is, "
               "Circle B, Square X, Triangle Y; L1 and R1 are the grips, L2 and R2 the triggers, "
               "L3 and R3 the stick clicks. Create, Options and the PS button stay SteamVR's: with "
               "the default controls " + std::string{left_handed ? "Circle" : "Triangle"} +
               " opens the game's menu. If a button does something "
               "else, SteamVR's controller bindings for the game show where it went. On PC, "
               "Sony's app only gives the Sense controllers basic rumble. The free PSVR2 Toolkit "
               "makes the vibration much richer, and the game uses it: crisp clicks, deep thumps.";
    case Platform::Frame:
        return "Steam Frame plays through SteamVR, with its own controller layout: the left "
               "d-pad does X's and Y's jobs, and the View button is SteamVR's.";
    case Platform::Index:
        return "Valve Index controllers through SteamVR (with an Index, a Bigscreen Beyond or any "
               "headset): each hand has A and B, the left hand's doing X's and Y's jobs; the grips "
               "are squeezed, and the system buttons are SteamVR's.";
    case Platform::Vive:
        return "HTC Vive wands through SteamVR: the trackpads are the sticks, and pressing a pad's "
               "regions presses the buttons (left pad up Y, down X, centre the left stick click; "
               "right pad centre A); the right menu button is B.";
    case Platform::Wmr:
        return "Windows Mixed Reality controllers through SteamVR: thumbsticks as on the Quest, and "
               "the trackpads are the face buttons (up B or Y, the rest A or X); the right menu "
               "button is B too. HP Reverb G2 controllers have the Quest's buttons: see its tab.";
    }
    return {};
}

} // namespace ControlsGuide
