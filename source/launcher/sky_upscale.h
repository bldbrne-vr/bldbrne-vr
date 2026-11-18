#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// Sharper skies: the game's sky, moon and cloud textures upscaled with Real-ESRGAN (its
// realesrgan-x4plus model, bundled in <launcher folder>/realesrgan), once, on the player's PC,
// from their own copy of the game. The runtime's sky detail (src/video_core/texture_cache/
// sky_detail.cpp) uses them in place of the game's own; without them the game's own stay.
// Nothing made from the game ships: the images exist only in the player's data folder
// (user/sky_detail).
// Sharper weapons (Kind::Weapons, the tester's 2026-11-04 "the option in launcher to also neurally
// upscale (2x) weapon textures as well (both left and right hand weapons)"): the same, for every
// weapon's albedo (dvdroot_ps4/parts/wp_*.partsbnd.dcx, the right hand's and the left's), 2x, in
// user/weapon_detail; the runtime finds them by their names (<texture>.<its bytes' key>.png).
namespace Launcher::Skies {
namespace fs = std::filesystem;

enum class Kind { Skies, Weapons };

// Raised when the way the images are made changes: older ones are offered again.
inline constexpr int Recipe = 1;
constexpr int RecipeFor(Kind kind) {
    return kind == Kind::Weapons ? 2 : Recipe;
}
// The widest image (the runtime looks for images of the same textures): a 2048 x 512 panorama
// becomes 4096 x 1024, a 512 x 512 moon 1024 x 1024.
inline constexpr std::uint32_t MaxSize = 4096;

fs::path ToolFolder(); // <launcher folder>/realesrgan
bool ToolIncluded(const fs::path &tool);
// user/sky_detail (user/weapon_detail), the runtime's folder for them.
fs::path Folder(const fs::path &user, Kind kind = Kind::Skies);

// What the folder holds (its made.json): images made, their bytes, the recipe they were made with.
struct State {
    int images{};
    std::uint64_t bytes{};
    int recipe{};
};
State Look(const fs::path &user, Kind kind = Kind::Skies);

// The texture lists' folders for a game folder (or its eboot.bin): the game's, its update's
// (-UPDATE, else -patch) and its mods' (-mods) dvdroot_ps4/map, as the runtime mounts them.
std::vector<fs::path> MapFolders(const fs::path &game);
// ... and their dvdroot_ps4/parts (the weapons' files).
std::vector<fs::path> PartsFolders(const fs::path &game);

struct Progress {
    std::atomic<bool> cancel{false};
    std::atomic<float> fraction{0};
    void Set(std::string text);
    std::string Text();

private:
    std::mutex mutex;
    std::string text;
};
struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("Cancelled.") {}
};
// Makes the images the folder lacks (all of them after a recipe change) and returns a summary.
// Throws the reason (Cancelled after progress.cancel).
std::string Make(const fs::path &game, const fs::path &user, const fs::path &tool,
                 Progress &progress, Kind kind = Kind::Skies);
// Deletes the images made.json lists, and made.json; nothing else in the folder.
void Remove(const fs::path &user, Kind kind = Kind::Skies);

// The image steps (tests): one channel, values 0..255.
struct Plane {
    int w{}, h{};
    std::vector<float> v;
};
// A Gaussian of `sigma` as three box blurs, edges extended.
void Blur(Plane &plane, float sigma);
// Lanczos-3 resampling (Pillow's LANCZOS: the kernel widened by the reduction).
Plane Resize(const Plane &plane, int w, int h);
// RGB where alpha is under half taken from the covered neighbours (wider blurs until every such
// texel has some), so the upscaler sees the moon's colour past its edge instead of BC1's black.
void FillTransparent(Plane *rgb, const Plane &alpha);
// Alpha to w x h: a 1-bit edge smoothed first (a moon's `hard` edge then a narrow smoothstep: a
// round anti-aliased limb instead of the keyed one's specks).
Plane UpscaleAlpha(Plane alpha, int w, int h, bool hard);
} // namespace Launcher::Skies
