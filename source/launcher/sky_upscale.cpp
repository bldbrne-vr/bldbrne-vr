#include "sky_upscale.h"
#include "core.h"
#include "core/vr/sky_textures.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <thread>
#include <unordered_set>
#include <windows.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace Launcher::Skies {
namespace {
namespace Sky = VR::SkyTextures;
using Bytes = std::vector<std::uint8_t>;

constexpr const wchar_t *ToolExe = L"realesrgan-ncnn-vulkan.exe";
constexpr const char *Model = "realesrgan-x4plus";
// Columns copied from the other end on both sides of a panorama before upscaling, then cut off:
// the upscaler sees the strip wrap around the horizon (without them its two ends no longer met:
// a seam twice as strong as the original's, 2026-10-30).
constexpr int WrapColumns = 32;

std::string Hex(std::uint64_t key) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(key));
    return text;
}

Bytes ReadFile(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read " + path.string());
    return Bytes(std::istreambuf_iterator<char>(file), {});
}

std::optional<Bytes> ReadRange(std::ifstream &file, std::uint64_t offset, std::uint64_t size) {
    if (!size || size > (64u << 20))
        return std::nullopt;
    Bytes out(size);
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size)))
        return std::nullopt;
    return out;
}

// A .tpf.dcx unpacked (DCX: DFLT, a zlib stream after "DCA\0" and its size).
std::optional<Bytes> Inflate(const Bytes &file) {
    if (file.size() < 0x50 || std::memcmp(file.data(), "DCX\0", 4) != 0)
        return std::nullopt;
    size_t dcs = 0;
    while (dcs + 12 < file.size() && std::memcmp(file.data() + dcs, "DCS\0", 4) != 0)
        ++dcs;
    size_t dca = dcs;
    while (dca + 8 < file.size() && std::memcmp(file.data() + dca, "DCA\0", 4) != 0)
        ++dca;
    if (dcs + 12 >= file.size() || dca + 8 >= file.size())
        return std::nullopt;
    const auto big = [&](size_t at) {
        return std::uint32_t(file[at]) << 24 | std::uint32_t(file[at + 1]) << 16 |
               std::uint32_t(file[at + 2]) << 8 | file[at + 3];
    };
    const std::uint32_t size = big(dcs + 4);
    if (!size || size > (128u << 20))
        return std::nullopt;
    Bytes out(size);
    const int got = stbi_zlib_decode_buffer(reinterpret_cast<char *>(out.data()), int(size),
                                            reinterpret_cast<const char *>(file.data() + dca + 8),
                                            int(file.size() - (dca + 8)));
    if (got != int(size))
        return std::nullopt;
    return out;
}

void WritePng(const fs::path &path, int w, int h, int channels, const std::uint8_t *pixels) {
    Bytes png;
    const auto append = [](void *context, void *data, int size) {
        auto &out = *static_cast<Bytes *>(context);
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        out.insert(out.end(), bytes, bytes + size);
    };
    if (!stbi_write_png_to_func(append, &png, w, h, channels, pixels, w * channels))
        throw std::runtime_error("Cannot encode " + path.filename().string());
    auto temporary = path;
    temporary += L".part";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size())))
            throw std::runtime_error("Cannot write " + temporary.string() + " (is the disk full?)");
    }
    fs::rename(temporary, path);
}

// One texture to make: the first of its names, its bytes' key (the runtime's: XXH3 of the top
// level's first 64 KiB), where it is and its copy's scale.
// A weapon's (`weapon`): `data` its part file, offset and size its TPF inside the unpacked BND,
// `name` the texture in it.
struct Item {
    std::string name;
    std::uint64_t key{};
    fs::path data;
    std::uint64_t offset{}, size{};
    std::uint32_t width{}, height{}, scale{};
    bool weapon{};
    std::string Stem() const { return name + "." + Hex(key); }
    bool Panorama() const { return !weapon && width >= 4 * height && width >= 2 * WrapColumns; }
    // A moon's 1-bit cut-out edge (not a cloud ring around a moon); a weapon's cut-outs (BC1's
    // 1-bit alpha: tassels, frayed cloth) the same.
    bool HardEdge() const {
        return weapon ||
               (name.find("moon") != std::string::npos && name.find("clond") == std::string::npos);
    }
};

std::optional<Bytes> ReadTpf(std::ifstream &bdt, std::uint64_t offset, std::uint64_t size) {
    const auto packed = ReadRange(bdt, offset, size);
    return packed ? Inflate(*packed) : std::nullopt;
}

// The item's TPF and its texture (nullopt when the game's file cannot be read).
std::optional<Bytes> LoadTpf(const Item &item) {
    if (!item.weapon) {
        std::ifstream bdt(item.data, std::ios::binary);
        return ReadTpf(bdt, item.offset, item.size);
    }
    const auto bnd = Inflate(ReadFile(item.data));
    if (!bnd || item.offset > bnd->size() || item.size > bnd->size() - item.offset)
        return std::nullopt;
    return Bytes(bnd->begin() + std::ptrdiff_t(item.offset),
                 bnd->begin() + std::ptrdiff_t(item.offset + item.size));
}
std::optional<Sky::Texture> TextureOf(const Item &item, const Bytes &tpf) {
    if (!item.weapon)
        return Sky::FirstTexture(tpf);
    for (const auto &t : Sky::Textures(tpf))
        if (t.name == item.name)
            return t;
    return std::nullopt;
}

// Every weapon's albedo in the parts folders (each file unpacked once, in order).
std::vector<Item> WeaponCatalog(const std::vector<fs::path> &folders, Progress &progress) {
    std::vector<Item> items;
    std::unordered_set<std::uint64_t> keys;
    std::error_code ec;
    for (const auto &root : folders) {
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            if (progress.cancel)
                throw Cancelled();
            std::string file = it->path().filename().string();
            std::transform(file.begin(), file.end(), file.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (!it->is_regular_file(ec) || !Sky::WeaponFile(file))
                continue;
            std::optional<Bytes> bnd;
            try {
                bnd = Inflate(ReadFile(it->path()));
            } catch (const std::exception &) {
                continue; // unreadable (open elsewhere): the others are still made
            }
            if (!bnd)
                continue;
            for (const auto &f : Sky::ReadBnd4(*bnd)) {
                if (!f.name.ends_with(".tpf"))
                    continue;
                const Bytes tpf(bnd->begin() + std::ptrdiff_t(f.offset),
                                bnd->begin() + std::ptrdiff_t(f.offset + f.size));
                for (const auto &t : Sky::Textures(tpf)) {
                    if (!Sky::WeaponWanted(t.name) || t.blocks == Sky::Blocks::Other)
                        continue;
                    const auto scale = Sky::WeaponScaleFor(t.width, t.height, MaxSize);
                    if (scale < 2)
                        continue;
                    const auto key = XXH3_64bits(tpf.data() + t.offset,
                                                 std::min<size_t>(t.size, Sky::KeyBytes));
                    if (keys.insert(key).second)
                        items.push_back({t.name, key, it->path(), f.offset, f.size, t.width,
                                         t.height, scale, true});
                }
            }
        }
    }
    return items;
}

std::vector<Item> Catalog(const std::vector<fs::path> &folders, Progress &progress) {
    std::vector<Item> items;
    std::unordered_set<std::uint64_t> keys;
    std::error_code ec;
    for (const auto &root : folders) {
        for (auto it = fs::recursive_directory_iterator(root, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (progress.cancel)
                throw Cancelled();
            const auto data = it->is_regular_file(ec) ? Sky::DataFile(it->path()) : std::nullopt;
            if (!data)
                continue;
            std::ifstream bdt(*data, std::ios::binary);
            if (!bdt)
                continue;
            for (const auto &e : Sky::ReadList(ReadFile(it->path()))) {
                if (!Sky::Wanted(e.name))
                    continue;
                const auto tpf = ReadTpf(bdt, e.offset, e.size);
                const auto t = tpf ? Sky::FirstTexture(*tpf) : std::nullopt;
                if (!t || t->blocks == Sky::Blocks::Other)
                    continue;
                const auto scale = Sky::ScaleFor(t->width, t->height, MaxSize);
                if (scale < 2)
                    continue;
                const auto key = XXH3_64bits(tpf->data() + t->offset,
                                             std::min<size_t>(t->size, Sky::KeyBytes));
                if (keys.insert(key).second)
                    items.push_back({Sky::TextureName(e.name), key, *data, e.offset, e.size,
                                     t->width, t->height, scale});
            }
        }
    }
    return items;
}

// Runs `work(i)` for i in [0, count) on a few threads; the first exception is rethrown.
void Parallel(size_t count, const std::function<void(size_t)> &work) {
    std::atomic<size_t> next{0};
    std::exception_ptr failure;
    std::mutex mutex;
    const size_t threads =
        std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 8);
    std::vector<std::thread> pool;
    for (size_t t = 0; t < std::min(threads, count); ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < count;) {
                try {
                    work(i);
                } catch (...) {
                    std::scoped_lock lock{mutex};
                    if (!failure)
                        failure = std::current_exception();
                    next = count;
                }
            }
        });
    for (auto &thread : pool)
        thread.join();
    if (failure)
        std::rethrow_exception(failure);
}

// Memory for the finishing step (an 8192 x 8192 upscale of a 2048 x 2048 cloud layer needs about
// 1 GB on its own): jobs wait while others hold more than this; a larger one runs alone.
class Budget {
public:
    explicit Budget(std::uint64_t limit) : limit(limit) {}
    void Take(std::uint64_t bytes) {
        std::unique_lock lock{mutex};
        changed.wait(lock, [&] { return !used || used + bytes <= limit; });
        used += bytes;
    }
    void Give(std::uint64_t bytes) {
        {
            std::scoped_lock lock{mutex};
            used -= bytes;
        }
        changed.notify_all();
    }

private:
    std::uint64_t limit, used{};
    std::mutex mutex;
    std::condition_variable changed;
};

Plane Channel(const std::uint8_t *pixels, int w, int h, int channels, int c) {
    Plane p{w, h, std::vector<float>(size_t(w) * h)};
    for (size_t i = 0; i < p.v.size(); ++i)
        p.v[i] = pixels[i * channels + c];
    return p;
}

std::uint8_t Byte(float v) {
    return std::uint8_t(std::clamp(v + 0.5f, 0.0f, 255.0f));
}

// Step 1: the texture decoded, its transparent texels coloured, a panorama padded; written as
// the upscaler's input. Its alpha is kept for step 3 (empty: opaque).
std::optional<Plane> Prepare(const Item &item, const fs::path &input) {
    const auto tpf = LoadTpf(item);
    const auto t = tpf ? TextureOf(item, *tpf) : std::nullopt;
    const auto rgba = t ? Sky::DecodeTop(*tpf, *t) : Bytes{};
    if (rgba.empty())
        throw std::runtime_error("Cannot decode the game's " + item.name);
    const int w = int(item.width), h = int(item.height);
    std::array<Plane, 3> rgb{Channel(rgba.data(), w, h, 4, 0), Channel(rgba.data(), w, h, 4, 1),
                             Channel(rgba.data(), w, h, 4, 2)};
    std::optional<Plane> alpha = Channel(rgba.data(), w, h, 4, 3);
    if (std::all_of(alpha->v.begin(), alpha->v.end(), [](float a) { return a == 255; }))
        alpha.reset();
    else
        FillTransparent(rgb.data(), *alpha);
    const int pad = item.Panorama() ? WrapColumns : 0, pw = w + 2 * pad;
    Bytes out(size_t(pw) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < pw; ++x) {
            const int sx = (x - pad + w) % w;
            for (int c = 0; c < 3; ++c)
                out[(size_t(y) * pw + x) * 3 + c] = Byte(rgb[c].v[size_t(y) * w + sx]);
        }
    WritePng(input, pw, h, 3, out.data());
    return alpha;
}

// Step 3: the upscaler's 4x image to the copy's size (Lanczos, the padding cut off after it so a
// panorama's ends are filtered across the wrap) with its alpha; written as <stem>.png.
void Finish(const Item &item, const std::optional<Plane> &alpha, const fs::path &upscaled,
            const fs::path &target) {
    const int w = int(item.width), h = int(item.height), scale = int(item.scale);
    const int pad = item.Panorama() ? WrapColumns : 0;
    const auto png = ReadFile(upscaled);
    int uw = 0, uh = 0;
    std::uint8_t *pixels = stbi_load_from_memory(png.data(), int(png.size()), &uw, &uh, nullptr, 3);
    if (!pixels)
        throw std::runtime_error("The upscaler's image of " + item.name + " cannot be read.");
    std::array<Plane, 3> rgb;
    for (int c = 0; c < 3; ++c)
        rgb[c] = Channel(pixels, uw, uh, 3, c);
    stbi_image_free(pixels);
    if (uw != (w + 2 * pad) * 4 || uh != h * 4)
        throw std::runtime_error("The upscaler's image of " + item.name + " has the wrong size.");
    const int tw = w * scale, th = h * scale, cut = pad * scale;
    for (auto &plane : rgb)
        if (scale != 4)
            plane = Resize(plane, (w + 2 * pad) * scale, th);
    const auto a = alpha ? std::optional<Plane>(UpscaleAlpha(*alpha, tw, th, item.HardEdge()))
                         : std::nullopt;
    const int channels = a ? 4 : 3, stride = rgb[0].w;
    Bytes out(size_t(tw) * th * channels);
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            auto *o = &out[(size_t(y) * tw + x) * channels];
            for (int c = 0; c < 3; ++c)
                o[c] = Byte(rgb[c].v[size_t(y) * stride + x + cut]);
            if (a)
                o[3] = Byte(a->v[size_t(y) * tw + x]);
        }
    WritePng(target, tw, th, channels, out.data());
}

// The last line the upscaler printed (its log), for an error.
std::string LastLine(const fs::path &log) {
    std::ifstream file(log, std::ios::binary);
    std::string line, last;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (!line.empty())
            last = line;
    }
    return last;
}

// Step 2: Real-ESRGAN over the input folder (one model load for all), at a lower priority, in a
// job that ends it with the launcher. Progress: the outputs as they appear.
void Upscale(const fs::path &tool, const fs::path &input, const fs::path &output,
             const fs::path &log, size_t count, Progress &progress, float from, float to) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create " + log.string());
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job)
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    std::wstring command = Quote((tool / ToolExe).wstring()) + L" -i " + Quote(input.wstring()) +
                           L" -o " + Quote(output.wstring()) + L" -n " +
                           std::wstring(Model, Model + std::strlen(Model)) + L" -s 4 -f png -m " +
                           Quote((tool / "models").wstring());
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = out;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
        tool.c_str(), &startup, &process);
    CloseHandle(out);
    if (!started) {
        if (job)
            CloseHandle(job);
        throw std::runtime_error("Cannot start the upscaler (" + (tool / ToolExe).string() + ").");
    }
    if (job)
        AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    const auto end = [&] {
        CloseHandle(process.hProcess);
        if (job)
            CloseHandle(job); // ends the upscaler if it still runs
    };
    DWORD code = 0;
    for (;;) {
        if (WaitForSingleObject(process.hProcess, 300) == WAIT_OBJECT_0)
            break;
        if (progress.cancel) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            end();
            throw Cancelled();
        }
        size_t done = 0;
        std::error_code ec;
        for (const auto &file : fs::directory_iterator(output, ec))
            done += file.path().extension() == ".png";
        progress.fraction = from + (to - from) * float(done) / float(std::max<size_t>(count, 1));
        progress.Set("Upscaling " + std::to_string(std::min(done + 1, count)) + " of " +
                     std::to_string(count) + " (Please wait...)");
    }
    GetExitCodeProcess(process.hProcess, &code);
    end();
    if (code != 0) {
        const auto line = LastLine(log);
        throw std::runtime_error("The upscaler stopped (code " + std::to_string(code) + ")" +
                                 (line.empty() ? "." : ": " + line));
    }
}

struct Made {
    int recipe{};
    std::vector<std::string> images;
};
Made ReadMade(const fs::path &folder) {
    Made made;
    try {
        const auto bytes = ReadFile(folder / "made.json");
        const auto json = nlohmann::json::parse(bytes.begin(), bytes.end());
        made.recipe = json.value("recipe", 0);
        for (const auto &name : json.value("images", nlohmann::json::array()))
            made.images.push_back(name.get<std::string>());
    } catch (const std::exception &) {
    }
    return made;
}
} // namespace

void Progress::Set(std::string value) {
    std::scoped_lock lock{mutex};
    text = std::move(value);
}
std::string Progress::Text() {
    std::scoped_lock lock{mutex};
    return text;
}

fs::path ToolFolder() {
    return ExecutableDirectory() / "realesrgan";
}

bool ToolIncluded(const fs::path &tool) {
    std::error_code ec;
    return fs::is_regular_file(tool / ToolExe, ec) &&
           fs::is_regular_file(tool / "models" / (std::string(Model) + ".param"), ec) &&
           fs::is_regular_file(tool / "models" / (std::string(Model) + ".bin"), ec);
}

fs::path Folder(const fs::path &user, Kind kind) {
    return user / (kind == Kind::Weapons ? "weapon_detail" : "sky_detail");
}

State Look(const fs::path &user, Kind kind) {
    const auto folder = Folder(user, kind);
    const auto made = ReadMade(folder);
    State state{0, 0, made.recipe};
    std::error_code ec;
    for (const auto &stem : made.images) {
        const auto size = fs::file_size(folder / (stem + ".png"), ec);
        if (!ec) {
            ++state.images;
            state.bytes += size;
        }
    }
    return state;
}

namespace {
std::vector<fs::path> AssetFolders(const fs::path &game, const char *asset) {
    std::error_code ec;
    const auto base = fs::is_directory(game, ec) ? game : game.parent_path();
    auto overlay = [&](const wchar_t *suffix) {
        auto path = base;
        path += suffix;
        return path;
    };
    auto patch = overlay(L"-UPDATE");
    if (!fs::is_directory(patch, ec))
        patch = overlay(L"-patch");
    std::vector<fs::path> folders;
    for (const auto &root : {base, patch, overlay(L"-mods")}) {
        const auto folder = root / "dvdroot_ps4" / asset;
        if (fs::is_directory(folder, ec))
            folders.push_back(folder);
    }
    return folders;
}

} // namespace

std::vector<fs::path> MapFolders(const fs::path &game) {
    return AssetFolders(game, "map");
}

std::vector<fs::path> PartsFolders(const fs::path &game) {
    return AssetFolders(game, "parts");
}

std::string Make(const fs::path &game, const fs::path &user, const fs::path &tool,
                 Progress &progress, Kind kind) {
    const bool weapons = kind == Kind::Weapons;
    const auto start = std::chrono::steady_clock::now();
    if (!ToolIncluded(tool))
        throw std::runtime_error("This package has no upscaler (" + tool.string() + ").");
    progress.fraction = 0;
    progress.Set(weapons ? "Reading the game's weapons" : "Reading the game's textures");
    const auto folders = weapons ? PartsFolders(game) : MapFolders(game);
    if (folders.empty())
        throw std::runtime_error(weapons ? "The game folder has no dvdroot_ps4\\parts."
                                         : "The game folder has no dvdroot_ps4\\map.");
    const auto items = weapons ? WeaponCatalog(folders, progress) : Catalog(folders, progress);
    if (items.empty())
        throw std::runtime_error(weapons
                                     ? "No weapon textures were found in the game folder."
                                     : "No sky, moon or cloud textures were found in the game folder.");
    const auto folder = Folder(user, kind);
    const auto old = ReadMade(folder);
    std::error_code ec;
    std::vector<const Item *> todo;
    for (const auto &item : items) {
        const bool have = old.recipe == RecipeFor(kind) &&
                          std::ranges::find(old.images, item.Stem()) != old.images.end() &&
                          fs::is_regular_file(folder / (item.Stem() + ".png"), ec);
        if (!have)
            todo.push_back(&item);
    }
    const auto work = folder / "work";
    const auto input = work / "in", output = work / "out";
    struct Cleanup {
        fs::path work;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(work, ec);
        }
    } cleanup{work};
    // In batches (the weapons: a 2048 x 1024 albedo's 4x image is 8192 x 4096, ~100 MB of PNG in
    // the work folder; the skies all at once, as before): prepared, upscaled, finished, then the
    // work files go before the next.
    const size_t batch = weapons ? 4 : std::max<size_t>(todo.size(), 1);
    Budget budget{1536ull << 20};
    std::set<std::string> done; // this game's images already made (they stay listed)
    for (const auto &item : items)
        if (std::ranges::find(todo, &item) == todo.end())
            done.insert(item.Stem());
    const auto WriteMade = [&](const std::set<std::string> &stems) {
        nlohmann::json list{{"recipe", RecipeFor(kind)},
                            {"model", Model},
                            {"images", nlohmann::json::array()}};
        for (const auto &stem : stems)
            list["images"].push_back(stem);
        std::ofstream file(folder / "made.json", std::ios::binary | std::ios::trunc);
        file << list.dump(1);
    };
    size_t base = 0;
    for (; base < todo.size(); base += batch) {
        const size_t count = std::min(batch, todo.size() - base);
        const float span = float(count) / float(todo.size()), at = float(base) / float(todo.size());
        fs::remove_all(input, ec);
        fs::remove_all(output, ec);
        fs::create_directories(input);
        fs::create_directories(output);
        std::vector<std::optional<Plane>> alpha(count);
        std::atomic<size_t> prepared{0};
        Parallel(count, [&](size_t i) {
            if (progress.cancel)
                throw Cancelled();
            alpha[i] = Prepare(*todo[base + i], input / (todo[base + i]->Stem() + ".png"));
            progress.fraction = at + span * 0.08f * float(++prepared) / float(count);
            progress.Set("Preparing " + std::to_string(base + prepared.load()) + " of " +
                         std::to_string(todo.size()));
        });
        Upscale(tool, input, output, work / "upscaler.log", count, progress, at + span * 0.08f,
                at + span * 0.85f);
        std::atomic<size_t> finished{0};
        Parallel(count, [&](size_t i) {
            if (progress.cancel)
                throw Cancelled();
            const auto &item = *todo[base + i];
            const int pad = item.Panorama() ? WrapColumns : 0;
            // The upscaled bytes, its float planes, and the resized ones.
            const std::uint64_t big = std::uint64_t(item.width + 2 * pad) * item.height * 16;
            const std::uint64_t need = big * 3 + big * 12 + big * 3 * item.scale * item.scale / 4;
            budget.Take(need);
            try {
                Finish(item, alpha[i], output / (item.Stem() + ".png"),
                       folder / (item.Stem() + ".png"));
            } catch (...) {
                budget.Give(need);
                throw;
            }
            budget.Give(need);
            alpha[i].reset();
            progress.fraction = at + span * (0.85f + 0.15f * float(++finished) / float(count));
            progress.Set("Finishing " + std::to_string(base + finished.load()) + " of " +
                         std::to_string(todo.size()));
        });
        // What is made so far (a cancel or a crash keeps them: Remove finds them, and the next
        // run makes only the rest).
        for (size_t i = 0; i < count; ++i)
            done.insert(todo[base + i]->Stem());
        WriteMade(done);
    }

    // made.json lists this game's images; ones made before for other bytes go.
    std::set<std::string> stems;
    for (const auto &item : items)
        stems.insert(item.Stem());
    for (const auto &stem : old.images)
        if (!stems.contains(stem))
            fs::remove(folder / (stem + ".png"), ec);
    WriteMade(stems);
    progress.fraction = 1;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    const char *what = weapons ? "weapon" : "sky";
    if (todo.empty())
        return "All " + std::to_string(items.size()) + " sharper " + what +
               " textures were already made.";
    return "Made " + std::to_string(todo.size()) +
           (weapons ? " sharper weapon textures in " : " sharper sky, moon and cloud textures in ") +
           std::to_string(seconds / 60) + " min " + std::to_string(seconds % 60) + " s.";
}

void Remove(const fs::path &user, Kind kind) {
    const auto folder = Folder(user, kind);
    std::error_code ec;
    for (const auto &stem : ReadMade(folder).images)
        fs::remove(folder / (stem + ".png"), ec);
    fs::remove(folder / "made.json", ec);
    fs::remove_all(folder / "work", ec);
    fs::remove(folder, ec); // only when nothing else is in it
}

void Blur(Plane &plane, float sigma) {
    // Three boxes whose sizes give a Gaussian of `sigma` (W. Jarosz, "Fast image convolutions").
    const double ideal = std::sqrt(12.0 * sigma * sigma / 3 + 1);
    int lower = int(std::floor(ideal));
    if (lower % 2 == 0)
        --lower;
    const int m = int(std::lround((12.0 * sigma * sigma - 3.0 * lower * lower - 12.0 * lower - 9) /
                                  (-4.0 * lower - 4)));
    const int w = plane.w, h = plane.h;
    std::vector<float> line(std::max(w, h));
    const auto pass = [&](float *data, int n, int step, int r) {
        // One line, `n` values `step` apart, edges extended.
        for (int i = 0; i < n; ++i)
            line[i] = data[size_t(i) * step];
        const float scale = 1.0f / float(2 * r + 1);
        float sum = float(r + 1) * line[0];
        for (int k = 1; k <= r; ++k)
            sum += line[std::min(k, n - 1)];
        for (int i = 0; i < n; ++i) {
            data[size_t(i) * step] = sum * scale;
            sum += line[std::min(i + r + 1, n - 1)] - line[std::max(i - r, 0)];
        }
    };
    for (int box = 0; box < 3; ++box) {
        const int r = ((box < m ? lower : lower + 2) - 1) / 2;
        if (r <= 0)
            continue;
        for (int y = 0; y < h; ++y)
            pass(plane.v.data() + size_t(y) * w, w, 1, r);
        for (int x = 0; x < w; ++x)
            pass(plane.v.data() + x, h, w, r);
    }
}

Plane Resize(const Plane &plane, int w, int h) {
    struct Taps {
        std::vector<int> first, count;
        std::vector<float> weight;
        int stride{};
    };
    const auto kernel = [](int in, int out) {
        const double scale = double(in) / out, widen = std::max(scale, 1.0),
                     support = 3.0 * widen;
        Taps taps;
        taps.stride = int(std::ceil(support)) * 2 + 1;
        taps.first.resize(out);
        taps.count.resize(out);
        taps.weight.assign(size_t(out) * taps.stride, 0.0f);
        const auto lanczos = [](double x) {
            if (x == 0)
                return 1.0;
            if (std::abs(x) >= 3)
                return 0.0;
            const double pi = 3.14159265358979323846, a = pi * x;
            return std::sin(a) / a * std::sin(a / 3) / (a / 3);
        };
        for (int i = 0; i < out; ++i) {
            const double centre = (i + 0.5) * scale;
            const int lo = std::max(int(centre - support + 0.5), 0);
            const int hi = std::min(int(centre + support + 0.5), in);
            double sum = 0;
            std::vector<double> k(size_t(hi - lo));
            for (int x = lo; x < hi; ++x)
                sum += k[x - lo] = lanczos((x - centre + 0.5) / widen);
            taps.first[i] = lo;
            taps.count[i] = std::min(hi - lo, taps.stride);
            for (int j = 0; j < taps.count[i]; ++j)
                taps.weight[size_t(i) * taps.stride + j] = float(sum ? k[j] / sum : 0);
        }
        return taps;
    };
    const auto across = kernel(plane.w, w), down = kernel(plane.h, h);
    std::vector<float> rows(size_t(w) * plane.h);
    for (int y = 0; y < plane.h; ++y) {
        const float *src = plane.v.data() + size_t(y) * plane.w;
        for (int x = 0; x < w; ++x) {
            const float *k = &across.weight[size_t(x) * across.stride];
            float sum = 0;
            for (int j = 0; j < across.count[x]; ++j)
                sum += src[across.first[x] + j] * k[j];
            rows[size_t(y) * w + x] = sum;
        }
    }
    Plane out{w, h, std::vector<float>(size_t(w) * h)};
    for (int y = 0; y < h; ++y) {
        const float *k = &down.weight[size_t(y) * down.stride];
        float *dst = out.v.data() + size_t(y) * w;
        for (int j = 0; j < down.count[y]; ++j) {
            const float *src = rows.data() + size_t(down.first[y] + j) * w;
            for (int x = 0; x < w; ++x)
                dst[x] += src[x] * k[j];
        }
    }
    return out;
}

void FillTransparent(Plane *rgb, const Plane &alpha) {
    const size_t n = alpha.v.size();
    std::vector<std::uint8_t> hole(n);
    bool any = false;
    for (size_t i = 0; i < n; ++i)
        any |= bool(hole[i] = alpha.v[i] < 127.5f);
    if (!any)
        return;
    std::array<Plane, 3> covered;
    for (int c = 0; c < 3; ++c) {
        covered[c] = rgb[c];
        for (size_t i = 0; i < n; ++i)
            covered[c].v[i] *= alpha.v[i] / 255.0f;
    }
    for (const float radius : {2.0f, 4.0f, 8.0f, 16.0f, 32.0f}) {
        Plane coverage = alpha;
        Blur(coverage, radius);
        std::array<Plane, 3> blurred = covered;
        for (auto &plane : blurred)
            Blur(plane, radius);
        any = false;
        for (size_t i = 0; i < n; ++i) {
            if (!hole[i])
                continue;
            const float c = coverage.v[i] / 255.0f;
            if (c > 0.02f) {
                for (int k = 0; k < 3; ++k)
                    rgb[k].v[i] = blurred[k].v[i] / std::max(c, 1e-3f);
                hole[i] = 0;
            } else
                any = true;
        }
        if (!any)
            break;
    }
}

Plane UpscaleAlpha(Plane alpha, int w, int h, bool hard) {
    const bool one_bit = std::all_of(alpha.v.begin(), alpha.v.end(),
                                     [](float a) { return a == 0 || a == 255; });
    if (one_bit)
        Blur(alpha, hard ? 1.2f : 0.7f);
    auto up = Resize(alpha, w, h);
    if (one_bit && hard)
        for (auto &a : up.v) {
            const float t = std::clamp((a / 255.0f - 0.4f) / 0.2f, 0.0f, 1.0f);
            a = t * t * (3 - 2 * t) * 255.0f;
        }
    return up;
}
} // namespace Launcher::Skies
