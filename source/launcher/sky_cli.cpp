#include "sky_upscale.h"
#include <future>
#include <iostream>
using namespace Launcher;
namespace fs = std::filesystem;
// sky_cli [--weapons] <game folder> <user folder> [upscaler folder]: makes the sharper skies (the
// sharper weapons) as the launcher's button does, printing its progress. sky_cli [--weapons]
// --remove <user folder> deletes them.
int wmain(int argc, wchar_t **argv) {
    try {
        Skies::Kind kind = Skies::Kind::Skies;
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--weapons") {
            kind = Skies::Kind::Weapons;
            ++argv;
            --argc;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--remove") {
            Skies::Remove(argv[2], kind);
            std::cout << "removed\n";
            return 0;
        }
        if (argc != 3 && argc != 4) {
            std::cerr << "usage: sky_cli [--weapons] <game folder> <user folder> [upscaler folder]\n"
                         "       sky_cli [--weapons] --remove <user folder>\n";
            return 2;
        }
        const fs::path game = argv[1], user = argv[2];
        const fs::path tool = argc == 4 ? fs::path(argv[3]) : Skies::ToolFolder();
        Skies::Progress progress;
        auto task = std::async(std::launch::async,
                               [&] { return Skies::Make(game, user, tool, progress, kind); });
        std::string last;
        while (task.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready) {
            const auto text = progress.Text();
            if (text != last)
                std::cout << int(progress.fraction * 100) << "% " << text << "\n" << std::flush;
            last = text;
        }
        std::cout << task.get() << "\n";
        const auto state = Skies::Look(user, kind);
        std::cout << state.images << " images, " << state.bytes / (1 << 20) << " MiB, recipe "
                  << state.recipe << "\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
