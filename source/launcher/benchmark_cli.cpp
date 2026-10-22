// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "benchmark.h"
#include <iostream>
#include <sstream>
#include <thread>
using namespace Launcher;
int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--analyze") {
        try {
            const fs::path folder = argv[2];
            const auto hardware = Json::parse(Read(folder / "hardware.json"));
            const auto previous = Json::parse(Read(folder / "results.json"));
            Benchmark::Headset headset{hardware.at("headset"), hardware.at("eye_width"),
                                       hardware.at("eye_height"), 8192, 8192};
            SystemInfo system;
            system.nvidia = true;
            system.vram = 16ull << 30;
            const auto candidates = Benchmark::Candidates(system, true, true);
            Json output = Json::array();
            for (const auto &item : previous) {
                const auto id = item.at("id").get<std::string>();
                const auto c = std::find_if(candidates.begin(), candidates.end(),
                                            [&](const auto &c) { return c.id == id; });
                Benchmark::Candidate candidate;
                if (c != candidates.end())
                    candidate = *c;
                else {
                    candidate = {id,
                                 item.at("label"),
                                 item.at("stereo") == "dibr" ? StereoPreset::Dibr
                                                             : StereoPreset::Native,
                                 ParseGraphics(item.at("graphics").get<std::string>()),
                                 item.at("dlss_model"),
                                 item.at("quality_rank")};
                }
                std::vector<Benchmark::Sample> samples;
                unsigned repeat = 0;
                for (const auto &original : item.at("repeats")) {
                    const auto log =
                        Read(folder / (id + "-" + std::to_string(++repeat)) / "measured-log.txt");
                    auto sample = Benchmark::Analyze(log, headset, candidate,
                                                     original.at("background_cores").is_number()
                                                         ? original.at("background_cores").get<double>()
                                                         : std::numeric_limits<double>::quiet_NaN());
                    // Offline analysis cannot erase an observed early exit or runtime failure.
                    if (!original.at("valid").get<bool>()) {
                        sample.valid = false;
                        sample.reason = original.at("reason");
                    }
                    samples.push_back(sample);
                }
                output.push_back(Benchmark::ToJson(
                    Benchmark::Summarize(candidate, samples, hardware.at("target_hz"))));
            }
            std::cout << output.dump(2) << std::endl;
            return 0;
        } catch (const std::exception &e) {
            std::cerr << e.what() << std::endl;
            return 1;
        }
    }
    if (argc < 5) {
        std::cerr << "runtime game fixture output [candidate-id|--cases=a,b] [--size=WxH] [cancel] "
                     "OR --analyze report-folder\n";
        return 2;
    }
    try {
        Benchmark::Request q;
        q.profile.runtime = PathText(argv[1]);
        q.profile.game = PathText(argv[2]);
        q.profile.use_bundled_runtime = true;
        q.paths = {fs::path(argv[4])};
        q.package = argv[3];
        q.headset = Benchmark::ScanHeadset(argv[1]);
        q.system = ReadSystem();
        q.seconds = 150;
        bool cancel_test = false;
        for (int i = 5; i < argc; ++i) {
            const auto option = Utf8(argv[i]);
            if (option == "--matrix")
                q.adaptive = false;
            else if (option == "--thorough")
                q.quick = false;
            else if (option == "cancel")
                cancel_test = true;
            else if (option.starts_with("--display-hz=")) {
                const auto value = option.substr(13);
                size_t consumed{};
                q.display_hz = std::stoul(value, &consumed);
                if (consumed != value.size() || q.display_hz < 30 || q.display_hz > 360)
                    throw std::runtime_error("Invalid display cadence.");
            } else if (option.starts_with("--size=")) {
                std::istringstream input(option.substr(7));
                unsigned w{}, h{};
                char separator{};
                if (!(input >> w >> separator >> h) || separator != 'x' || !input.eof() || w < 64 ||
                    h < 64 || w > q.headset.max_width || h > q.headset.max_height)
                    throw std::runtime_error("Invalid controlled resolution override.");
                q.headset.name += " [research override; scanned " +
                                  std::to_string(q.headset.width[0]) + "x" +
                                  std::to_string(q.headset.height[0]) + "]";
                q.headset.width = {w, w};
                q.headset.height = {h, h};
            } else {
                std::istringstream input(option.starts_with("--cases=") ? option.substr(8)
                                                                        : option);
                std::string id;
                while (std::getline(input, id, ','))
                    q.candidate_ids.push_back(id);
            }
        }
        std::atomic_bool cancel = false;
        std::jthread timer;
        if (cancel_test)
            timer = std::jthread([&](std::stop_token stop) {
                for (int i = 0; i < 400 && !stop.stop_requested(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!stop.stop_requested())
                    cancel = true;
            });
        const auto result =
            Benchmark::Run(q, cancel, [](const std::string &s) { std::cout << s << std::endl; });
        if (timer.joinable())
            timer.request_stop();
        std::cout << "Report: " << PathText(result.folder) << "\nError: " << result.error
                  << "\nCancelled: " << result.cancelled << "\n";
        for (auto &r : result.results)
            std::cout << Benchmark::ToJson(r).dump(2) << std::endl;
        return !result.error.empty() ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
