#pragma once
// The launcher's Controls page (2026-10-30, the user): every VR control for each controller the
// game knows (Quest, PS VR2, Steam Frame, Index, Vive, Windows MR; 2026-11-03, the user: "properly
// reflect buttons for each controller"), written by the in-game Show VR controls' own tables
// (core/vr/controls_view.h) so the two never differ, with the player's vr.ini settings; then how
// to play, from the VR page's own help (core/vr/vr_page.h) and the few things only the hands do.
// The VR interactions tab (2026-11-03, the user: "a short guide for people on how to use things
// properly", searchable): weapons, fighting, two hands, bare hands, every item, the tools and the
// world, in the button names of the controller tab picked last.
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace VR::ControlsView {
struct Config;
}

namespace ControlsGuide {

enum class Platform { Quest, Psvr2, Frame, Index, Vive, Wmr };
inline constexpr const char *PlatformNames[]{"Quest",    "PS VR2", "Steam Frame",
                                             "Index",    "Vive",   "Windows MR"};
inline constexpr int PlatformCount = 6;
// The tab for the headset the game last ran on (its XrSystemProperties::systemName); Quest when
// the name does not say (SteamVR names an Index, a Vive and a Beyond alike "lighthouse").
Platform PlatformOf(const std::string &system_name);

// vr.ini's value of a key (the launcher's pending edits first); empty: the game's default.
using Lookup = std::function<std::optional<std::string>(const std::string &)>;

struct Line {
    std::string control; // "left grip"; empty for a gesture
    std::string title;   // "Item arc"
    std::string sub;     // "at your hip: Blood Vial"
    std::string help;    // the one-line help
    std::string words;   // more words the search finds it by (the VR interactions guide)
    const char *row{nullptr}; // the VR page row (vr_page.h id) that changes it, if any
};
struct Section {
    std::string title;
    std::string note;
    std::vector<Line> lines;
};

// The game's controls Config (core/vr/controls_session.cpp CurrentConfig) from vr.ini, with the
// game's defaults for a key the file lacks, on a runtime that owns the left Menu button
// (SteamVR / VDXR) or not (Meta's).
VR::ControlsView::Config ConfigFrom(const Lookup &value, bool menu_runtime);
// The platform's controls with these settings: while playing, in menus, and the gestures.
// Pass the active runtime name when known; otherwise the controller tab supplies the default.
std::vector<Section> Controls(Platform platform, const Lookup &value,
                              std::string_view runtime_name = {});
// Button locations normalized into a 240 x 440 drawing area.
struct DiagramButton {
    bool right{};
    float x{}, y{};
    Line line;
};
std::vector<DiagramButton> Diagram(Platform platform, const Lookup &value, bool menus,
                                   std::string_view runtime_name = {});
// The VR interactions guide: what the hands do beyond the buttons, in the platform's button
// names, with these settings.
std::vector<Section> Interactions(Platform platform, const Lookup &value,
                                  std::string_view runtime_name = {});
// Whether a line has every word of `needle` (lower case) at the start of one of its words.
bool Finds(const Line &line, const std::string &needle);
// What the player should know about the platform first (its runtime, its button names).
std::string PlatformNote(Platform platform);
// A VR page row's label and help line as the game words them (vr_page.h RowLabel / RowHelp): the
// platform's button names and the controls these settings bind; empty for an unknown id.
struct RowText {
    std::string label, help;
};
RowText RowWords(const char *id, Platform platform, const Lookup &value);

} // namespace ControlsGuide
