// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <string>
#include <fmt/format.h>

#include "core/vr/transform_settings.h"

namespace VR::Transform {

namespace {

bool Flag(const std::string& value) {
    return value == "1" || value == "true";
}

} // Anonymous namespace

TransformSettings& GetTransformSettings() {
    static TransformSettings settings;
    return settings;
}

bool ParseTransformSetting(const std::string& key, const std::string& value) {
    auto& s = GetTransformSettings();
    const auto f = [&value] { return std::stof(value); };
    if (key == "transform") {
        if (value != "native" && value != "vanilla") {
            throw std::invalid_argument{"native or vanilla expected"};
        }
        s.native = value == "native";
    } else if (key == "transform_untested") {
        s.untested = Flag(value);
    } else if (key == "transform_speed") {
        s.speed = std::clamp(f(), 0.5f, 2.0f);
    } else if (key == "transform_skip_s") {
        s.skip_s = value == "auto" ? -1.0f : std::clamp(f(), -1.0f, 0.6f);
    } else if (key == "transform_retract_speed") {
        s.retract_speed = std::clamp(f(), 1.0f, 3.0f);
    } else if (key == "transform_cues") {
        s.cues = Flag(value);
    } else if (key == "transform_haptics") {
        s.haptics = Flag(value);
    } else if (key == "transform_buffer_ms") {
        s.buffer_ms = std::clamp(f(), 0.0f, 1500.0f);
    } else if (key == "transform_flick") {
        s.flick = Flag(value);
    } else if (key == "transform_flick_speed") {
        s.flick_speed = std::clamp(f(), 1.0f, 4.0f);
    } else if (key == "transform_flick_lead_ms") {
        s.flick_lead_ms = std::clamp(f(), 30.0f, 600.0f);
    } else if (key == "transform_attack") {
        s.attack = Flag(value);
    } else {
        return false;
    }
    return true;
}

void WriteTransformDefaults(std::ostream& out) {
    const TransformSettings d{};
    out << "# ---- Transform (the right grip) ----\n"
        << "# native = only the weapon transforms (it unfolds in your hand with its own sound; no "
           "body animation,\n# you keep walking); vanilla = the right grip is L1 (the game's "
           "transform animation)\n"
        << "transform=" << (d.native ? "native" : "vanilla") << "\n"
        << "# trick weapons whose native transform was not checked yet transform natively too (0 "
           "= they keep L1)\n"
        << "transform_untested=" << (d.untested ? 1 : 0) << "\n"
        << "# the transform's speed (1 = the game's timing) and seconds of its wind-up skipped "
           "(auto = the part\n# where the weapon itself does not move yet, so it moves at the "
           "squeeze; 0 = the game's timing)\n"
        << "transform_speed=" << d.speed << "\n"
        << "transform_skip_s="
        << (d.skip_s < 0.0f ? std::string{"auto"} : fmt::format("{}", d.skip_s)) << "\n"
        << "# the retract (form B -> A): its start, up to the weapon's switch, this much faster "
           "(the whole clip\n# still plays, its sounds in order; 1 = the game's timing)\n"
        << "transform_retract_speed=" << d.retract_speed << "\n"
        << "# the weapon's own transform sounds and effects; the latch, detent and lock haptics\n"
        << "transform_cues=" << (d.cues ? 1 : 0) << "\n"
        << "transform_haptics=" << (d.haptics ? 1 : 0) << "\n"
        << "# a squeeze while the game has the arm (a hit, an item) or the transform before "
           "is still locking\n# waits this long (ms)\n"
        << "transform_buffer_ms=" << d.buffer_ms << "\n"
        << "# squeeze while swinging or flicking the weapon: it snaps into its new form "
           "mid-stroke (0 = such a\n# squeeze is ignored); the flick's speed (the clip's, at "
           "least) and the longest wait for its\n# form switch (ms); the stroke becomes the "
           "game's transform attack (0 = the new form's own swing)\n"
        << "transform_flick=" << (d.flick ? 1 : 0) << "\n"
        << "transform_flick_speed=" << d.flick_speed << "\n"
        << "transform_flick_lead_ms=" << d.flick_lead_ms << "\n"
        << "transform_attack=" << (d.attack ? 1 : 0) << "\n";
}

std::string TransformSettingsText() {
    const auto& s = GetTransformSettings();
    return fmt::format("transform={} transform_untested={} transform_speed={:.2f} "
                       "transform_skip_s={} transform_retract_speed={:.2f} transform_cues={} "
                       "transform_haptics={} "
                       "transform_buffer_ms={:.0f} transform_flick={} transform_flick_speed={:.2f} "
                       "transform_flick_lead_ms={:.0f} transform_attack={}",
                       s.native ? "native" : "vanilla", s.untested ? 1 : 0, s.speed,
                       s.skip_s < 0.0f ? std::string{"auto"} : fmt::format("{:.2f}", s.skip_s),
                       s.retract_speed, s.cues ? 1 : 0, s.haptics ? 1 : 0, s.buffer_ms, s.flick ? 1 : 0,
                       s.flick_speed, s.flick_lead_ms, s.attack ? 1 : 0);
}

} // namespace VR::Transform
