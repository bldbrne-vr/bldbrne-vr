#include "core.h"
#include <Velopack.hpp>
#include <atomic>
#include <d3d11.h>
#include <dwmapi.h>
#include <future>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_stdlib.h>
#include <memory>
#include <string_view>
#include <psapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#ifndef BBVR_RELEASES_URL
#define BBVR_RELEASES_URL ""
#endif
using namespace Launcher;
using Microsoft::WRL::ComPtr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static HWND window_handle{};
static int repaint = 3;
static bool close_requested{};
static UINT resize_width{}, resize_height{};
static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> context;
static ComPtr<IDXGISwapChain> swapchain;
static ComPtr<ID3D11RenderTargetView> target;
static ImFont *heading_font{};
static ImFont *brand_font{};
static ImVec4 gold{0.67f, 0.58f, 0.40f, 1}, muted{0.57f, 0.57f, 0.55f, 1};
static float scale = 1;
static float font_scale = 1;
static float S(float v) {
    return v * scale;
}
static void Require(HRESULT r, const char *message) {
    if (FAILED(r))
        throw std::runtime_error(message);
}
static void RenderTarget() {
    ComPtr<ID3D11Texture2D> buffer;
    Require(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Cannot access the display buffer.");
    Require(device->CreateRenderTargetView(buffer.Get(), nullptr, &target),
            "Cannot create display target.");
}
static void CreateGraphics(HWND window) {
    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferCount = 2;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = window;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    auto r = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &d, &swapchain, &device, nullptr,
                                           &context);
    if (FAILED(r))
        r = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                          D3D11_SDK_VERSION, &d, &swapchain, &device, nullptr,
                                          &context);
    Require(r, "Direct3D 11 is unavailable.");
    RenderTarget();
}
static void Capture(const fs::path &path) {
    ComPtr<ID3D11Texture2D> buffer, staging;
    Require(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Cannot read preview.");
    D3D11_TEXTURE2D_DESC d{};
    buffer->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    Require(device->CreateTexture2D(&d, nullptr, &staging), "Cannot stage preview.");
    context->CopyResource(staging.Get(), buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Require(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Cannot map preview.");
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    Require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&factory)),
            "Cannot create PNG writer.");
    Require(factory->CreateStream(&stream), "Cannot create PNG stream.");
    Require(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Cannot create PNG file.");
    Require(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
            "Cannot encode PNG.");
    Require(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Cannot initialize PNG.");
    Require(encoder->CreateNewFrame(&frame, nullptr), "Cannot create PNG frame.");
    frame->Initialize(nullptr);
    frame->SetSize(d.Width, d.Height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&format);
    std::vector<BYTE> pixels(size_t(d.Width) * d.Height * 4);
    for (UINT y = 0; y < d.Height; ++y) {
        const auto *source = static_cast<const BYTE *>(mapped.pData) + y * mapped.RowPitch;
        auto *dest = pixels.data() + size_t(y) * d.Width * 4;
        for (UINT x = 0; x < d.Width; ++x) {
            dest[x * 4] = source[x * 4 + 2];
            dest[x * 4 + 1] = source[x * 4 + 1];
            dest[x * 4 + 2] = source[x * 4];
            dest[x * 4 + 3] = source[x * 4 + 3];
        }
    }
    Require(frame->WritePixels(d.Height, d.Width * 4, DWORD(pixels.size()), pixels.data()),
            "Cannot write preview pixels.");
    frame->Commit();
    encoder->Commit();
    context->Unmap(staging.Get(), 0);
}
static LRESULT WINAPI WindowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    repaint = 3;
    if (ImGui_ImplWin32_WndProcHandler(window, msg, wp, lp))
        return 1;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            resize_width = LOWORD(lp);
            resize_height = HIWORD(lp);
        }
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO *>(lp)->ptMinTrackSize = {LONG(S(860)), LONG(S(690))};
        return 0;
    case WM_CLOSE:
        close_requested = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DPICHANGED: {
        const float next_scale = float(HIWORD(wp)) / 96.0f;
        if (ImGui::GetCurrentContext()) {
            ImGui::GetStyle().ScaleAllSizes(next_scale / scale);
            ImGui::GetIO().FontGlobalScale = next_scale / font_scale;
        }
        scale = next_scale;
        const auto r = reinterpret_cast<RECT *>(lp);
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    }
    return DefWindowProcW(window, msg, wp, lp);
}
static void Theme() {
    ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowPadding = {0, 0};
    s.FramePadding = {12, 9};
    s.ButtonTextAlign = {0.5f, 0.5f};
    s.SelectableTextAlign = {0.0f, 0.5f};
    s.ItemSpacing = {10, 12};
    s.FrameRounding = s.ChildRounding = s.PopupRounding = 3;
    s.ScrollbarSize = 10;
    s.FrameBorderSize = 1;
    s.Colors[ImGuiCol_WindowBg] = s.Colors[ImGuiCol_ChildBg] = {0.055f, 0.058f, 0.061f, 1};
    s.Colors[ImGuiCol_Text] = {0.85f, 0.83f, 0.78f, 1};
    s.Colors[ImGuiCol_TextDisabled] = muted;
    s.Colors[ImGuiCol_Border] = s.Colors[ImGuiCol_Separator] = {0.19f, 0.18f, 0.16f, 1};
    s.Colors[ImGuiCol_FrameBg] = {0.09f, 0.09f, 0.09f, 1};
    s.Colors[ImGuiCol_FrameBgHovered] = {0.23f, 0.20f, 0.15f, 1};
    s.Colors[ImGuiCol_FrameBgActive] = {0.18f, 0.16f, 0.12f, 1};
    s.Colors[ImGuiCol_Button] = {0.13f, 0.13f, 0.12f, 1};
    s.Colors[ImGuiCol_ButtonHovered] = {0.34f, 0.28f, 0.19f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {0.29f, 0.24f, 0.17f, 1};
    s.Colors[ImGuiCol_Header] = {0.20f, 0.17f, 0.12f, 1};
    s.Colors[ImGuiCol_HeaderHovered] = {0.34f, 0.28f, 0.19f, 1};
    s.Colors[ImGuiCol_HeaderActive] = {0.30f, 0.25f, 0.17f, 1};
    s.Colors[ImGuiCol_CheckMark] = s.Colors[ImGuiCol_SliderGrab] = gold;
    s.Colors[ImGuiCol_TitleBg] = s.Colors[ImGuiCol_TitleBgActive] = {0.20f, 0.17f, 0.12f, 1};
    s.Colors[ImGuiCol_PopupBg] = {0.07f, 0.072f, 0.075f, 1};
    s.Colors[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, 0.55f};
    s.ScaleAllSizes(scale);
}
static void Hint(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
// The main window has no padding; dialogs and their lists need some.
static bool Modal(const char *name, bool *open, ImGuiWindowFlags flags) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(20), S(16)});
    const bool shown = ImGui::BeginPopupModal(name, open, flags);
    ImGui::PopStyleVar();
    return shown;
}
static void ListBegin(const char *id, ImVec2 size, ImGuiWindowFlags flags = 0) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(10), S(8)});
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, flags);
    ImGui::PopStyleVar();
}
static void Title(const char *eyebrow, const char *title) {
    if (eyebrow) {
        ImGui::TextColored(gold, "%s", eyebrow);
        ImGui::Dummy({0, S(3)});
    }
    ImGui::PushFont(heading_font);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
}
struct UpdateResult {
    std::optional<Velopack::UpdateInfo> release;
    std::string message, error;
    bool downloaded{};
};
struct App {
    Paths paths;
    Profile profile;
    std::string ini;
    std::map<std::string, std::optional<std::string>> edits;
    int page{}, category{};
    std::string search, error;
    std::string status = "Ready when you are.";
    bool running{}, quit{}, preview{};
    HANDLE game_process{};
    std::unique_ptr<Velopack::UpdateManager> updater;
    std::optional<Velopack::UpdateInfo> available;
    std::future<UpdateResult> task;
    std::atomic<int> progress{};
    bool downloaded{};
    bool downloading{};
    std::string setup_issue;
    std::string update_status = "Check for a new launcher and VR runtime release.";
    SystemInfo system;
    std::string xr_runtime;
    bool shaders_ready{};
    // The eboot check reads the game file: done again only when the file changes.
    std::string build_issue;
    fs::path build_checked;
    fs::file_time_type build_time{};
    std::vector<SaveBackup> backups;
    std::optional<fs::path> restore_target;
    bool show_backups{}, show_crash{};
    std::optional<DWORD> last_exit;
    struct Crash {
        DWORD code;
        std::string reason;
        std::vector<std::string> lines;
    };
    std::optional<Crash> crash;
    struct Upscalers {
        std::string runtime;
        bool dlss{}, fsr4{}, dlss5{};
    } upscalers{"\x01"};
    const Upscalers &Available() {
        if (upscalers.runtime != profile.runtime) {
            const auto runtime = RuntimePath();
            upscalers = {profile.runtime, system.nvidia && DlssIncluded(runtime),
                         Fsr4Included(runtime), system.nvidia && Dlss5Included(runtime)};
        }
        return upscalers;
    }
    App(Paths p, bool pv) : paths(std::move(p)), preview(pv) {
        fs::create_directories(paths.User());
        profile = Profile::Load(paths);
        Reload();
        system = ReadSystem();
        xr_runtime = OpenXrRuntime();
        shaders_ready = ShaderCacheReady(paths);
        if (!shaders_ready)
            status = FirstLaunchNote;
    }
    static constexpr const char *FirstLaunchNote =
        "First launch: the game builds its shaders, so the first few minutes may stutter. Later "
        "launches are smooth.";
    ~App() {
        if (task.valid())
            task.wait();
        if (game_process)
            CloseHandle(game_process);
    }
    bool Busy() const {
        return task.valid();
    }
    template <class F> void Try(F action) {
        try {
            action();
        } catch (const std::exception &ex) {
            error = ex.what();
            status = error;
        }
    }
    void RequireStopped() {
        if (RuntimeRunning())
            throw std::runtime_error("Close shadPS4 before changing files or installing updates.");
    }
    void Reload() {
        ini = Read(paths.Ini());
        edits.clear();
    }
    void SaveIni() {
        RequireStopped();
        std::vector<VR::Ini::Edit> list;
        for (const auto &[key, value] : edits)
            list.push_back({key, value});
        auto backup = Write(paths.Ini(), VR::Ini::ApplyEdits(ini, list), ini);
        Reload();
        status = backup.empty() ? "Settings saved."
                                : "Settings saved. A backup of vr.ini is in your data folder.";
    }
    void SaveProfile() {
        profile.saves = PathText(NormalizeSaveHome(Path(profile.saves)));
        profile.Save(paths);
        status = "Paths saved. Your saves stay in their selected folder.";
    }
    void Check() {
        if (Busy())
            return;
        if (std::string_view(BBVR_RELEASES_URL).empty()) {
            update_status = "Updates are unavailable for this build.";
            return;
        }
        downloaded = false;
        downloading = false;
        progress = 0;
        available.reset();
        update_status = "Checking for updates...";
        task = std::async(std::launch::async, [this] {
            UpdateResult r;
            try {
                if (!updater) {
                    const Velopack::HttpOptions http{{}, 180000};
                    const Velopack::UpdateOptions options{false, "vr-win", -1};
                    updater = std::make_unique<Velopack::UpdateManager>(
                        std::make_unique<Velopack::HttpSource>(
                            std::string(BBVR_RELEASES_URL) + "/latest/download",
                            http),
                        &options);
                }
                if (updater->GetCurrentVersion().empty())
                    r.message = "Development build. Install a packaged release to enable updates.";
                else {
                    r.release = updater->CheckForUpdates();
                    r.message = r.release ? "Version " + r.release->TargetFullRelease.Version +
                                                " is available."
                                          : "You are up to date.";
                }
            } catch (const std::exception &ex) {
                r.message = "Update check unavailable. You can still play offline.";
                r.error = ex.what();
            }
            return r;
        });
    }
    void Download() {
        if (Busy() || !available)
            return;
        progress = 0;
        downloading = true;
        update_status = "Downloading the update...";
        task = std::async(std::launch::async, [this] {
            UpdateResult r;
            r.release = available;
            try {
                updater->DownloadUpdates(
                    *available,
                    [](void *user, size_t value) {
                        static_cast<App *>(user)->progress.store(int(value));
                    },
                    this);
                r.downloaded = true;
                r.message = "Download verified. Ready to install after the game closes.";
            } catch (const std::exception &ex) {
                r.message = "Download failed. Your installed version is unchanged.";
                r.error = ex.what();
            }
            return r;
        });
    }
    void Tick() {
        if (Busy() && task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto r = task.get();
            available = std::move(r.release);
            downloaded = r.downloaded;
            downloading = false;
            update_status = std::move(r.message);
            if (!r.error.empty())
                status = std::move(r.error);
            repaint = 3;
        }
        const bool before = running;
        if (game_process && WaitForSingleObject(game_process, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(game_process, &code);
            // A graphics restart: follow the relaunched game instead of reporting an exit. Its
            // parent is looked up while the old handle still holds that PID.
            const DWORD next = code == 0 ? RestartedProcess(GetProcessId(game_process)) : 0;
            CloseHandle(game_process);
            game_process =
                next ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, next)
                     : nullptr;
            if (!game_process)
                last_exit = code;
        }
        Try([&] { running = !preview && (game_process || RuntimeRunning()); });
        if (before != running)
            repaint = 3;
        if (before && !running) {
            const auto problems = LogProblems(paths);
            const bool load_failed =
                std::any_of(problems.begin(), problems.end(), [](const std::string &line) {
                    return line.find("Failed to load game's eboot.bin") != std::string::npos;
                });
            if (last_exit && (*last_exit || load_failed)) {
                crash = Crash{*last_exit,
                              *last_exit ? ExitReason(*last_exit) : "the game could not be loaded",
                              problems};
                show_crash = true;
                status = "The game stopped because of " + crash->reason + ".";
            } else
                status = "The hunt has ended. Ready for your next journey.";
            last_exit.reset();
            shaders_ready = ShaderCacheReady(paths);
            if (edits.empty())
                Try([&] { Reload(); });
            ShowWindow(window_handle, SW_RESTORE);
        }
    }
    void Field(const char *label, const char *hint, std::string &value, bool folder,
               const wchar_t *pattern = L"*.*") {
        ImGui::TextUnformatted(label);
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(98));
        ImGui::InputText("##path", &value);
        ImGui::SameLine();
        if (ImGui::Button("Browse", {S(88), 0}))
            Try([&] {
                if (auto p = PickPath(window_handle, folder, Wide(label).c_str(), pattern))
                    value = PathText(
                        std::string(label) == "Save home folder" ? NormalizeSaveHome(*p) : *p);
            });
        Hint(hint);
        ImGui::PopID();
        ImGui::Dummy({0, S(2)});
    }
    void Motif() {
        auto *d = ImGui::GetWindowDrawList();
        auto p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, x = p.x + w - S(55), y = p.y - S(57);
        auto c = IM_COL32(105, 91, 65, 130);
        d->AddCircle({x, y}, S(23), c, 40, S(1));
        d->AddLine({x, y - S(33)}, {x, y + S(43)}, c, S(1));
        d->AddLine({x - S(12), y + S(29)}, {x + S(12), y + S(29)}, c, S(1));
        d->AddLine({x - S(26), y - S(20)}, {x + S(26), y + S(20)}, c, S(1));
        d->AddLine({p.x, p.y + S(4)}, {p.x + w, p.y + S(4)}, c);
        ImGui::Dummy({0, S(20)});
    }
    // The desktop mirror and Save paths share one row.
    void LaunchOptions() {
        ImGui::Checkbox("Fullscreen desktop mirror", &profile.fullscreen);
        ImGui::SameLine();
        if (ImGui::Button("Save paths"))
            Try([&] {
                RequireStopped();
                SaveProfile();
            });
    }
    void PlayPage() {
        Title(nullptr, "The hunt awaits.");
        Hint("Return to Yharnam. See it with your own eyes.");
        Motif();
        ImGui::BeginDisabled(running || Busy());
        Field("Game folder", "Your game folder (version 1.09), containing eboot.bin.", profile.game,
              true);
        Field("Save home folder",
              "Choose shadPS4's home folder: home / 1000 / savedata / CUSA...\nAn existing "
              "savedata folder is also accepted. Saves are used in place.",
              profile.saves, true);
        if (ImGui::CollapsingHeader("Runtime and setup")) {
            if (ImGui::Checkbox("Use the included VR runtime", &profile.use_bundled_runtime)) {
                if (profile.use_bundled_runtime) {
                    const auto bundled = ExecutableDirectory() / "runtime" / "shadps4.exe";
                    profile.runtime =
                        fs::is_regular_file(bundled) ? PathText(bundled) : std::string{};
                }
            }
            if (!profile.use_bundled_runtime)
                Field("VR runtime", "Choose a custom shadps4.exe. This choice is remembered.",
                      profile.runtime, false, L"shadps4.exe");
            else
                Hint(profile.runtime.empty()
                         ? "The included runtime is missing. Use a complete package."
                         : "The included runtime is updated with the launcher.");
            if (ImGui::Button("Open system modules folder"))
                Try([&] {
                    const auto modules = paths.User() / "sys_modules";
                    fs::create_directories(modules);
                    OpenPath(modules);
                });
            Hint("Emulator data is kept separately. Bloodborne runs without system modules; "
                 "optional ones go in user / sys_modules inside the data folder.");
        }
        LaunchOptions();
        ImGui::EndDisabled();
        ImGui::Separator();
        setup_issue.clear();
        try {
            ValidateSetup(profile);
            const auto game = GameExecutable(Path(profile.game));
            const auto time = fs::last_write_time(game);
            if (game != build_checked || time != build_time) {
                build_checked = game;
                build_time = time;
                build_issue.clear();
                try {
                    CheckGameBuild(game);
                } catch (const std::exception &ex) {
                    build_issue = ex.what();
                }
            }
            setup_issue = build_issue;
        } catch (const std::exception &ex) {
            setup_issue = ex.what();
        }
        StatusCards();
    }
    void PlayButton() {
        ImGui::BeginDisabled(running || Busy() || !setup_issue.empty());
        ImGui::PushStyleColor(ImGuiCol_Button, {0.34f, 0.075f, 0.073f, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.47f, 0.105f, 0.09f, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.28f, 0.06f, 0.06f, 1});
        if (ImGui::Button(running               ? "HUNT IN PROGRESS"
                          : setup_issue.empty() ? "BEGIN THE HUNT"
                                                : "COMPLETE SETUP TO PLAY",
                          {-1, S(48)}))
            Try([&] { StartGame(); });
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
    }
    void StartGame() {
        RequireStopped();
        SaveProfile();
        if (!edits.empty())
            SaveIni();
        auto plan = Prepare(profile, paths);
        Reload();
        game_process = Launch(plan);
        last_exit.reset();
        crash.reset();
        running = true;
        status = shaders_ready ? "Starting BB-VR..." : FirstLaunchNote;
        ShowWindow(window_handle, SW_MINIMIZE);
    }
    // Everything a bug report needs, in one paste.
    std::string SupportInfo() const {
        std::string out = "BB-VR launcher " BBVR_VERSION "\n";
        try {
            const auto info = Json::parse(Read(ExecutableDirectory() / "build-info.json"));
            out += "Package " + info.value("version", std::string{}) + " from " +
                   info.value("source_commit", std::string{}) + "\n";
        } catch (const std::exception &) {
            out += "Package: development build\n";
        }
        out += "Runtime " + profile.runtime + "\n";
        using RtlGetVersion = LONG(WINAPI *)(OSVERSIONINFOW *);
        OSVERSIONINFOW os{sizeof(os)};
        if (auto get = reinterpret_cast<RtlGetVersion>(
                GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
            get(&os);
        out += "Windows " + std::to_string(os.dwMajorVersion) + "." +
               std::to_string(os.dwMinorVersion) + " build " + std::to_string(os.dwBuildNumber) +
               "\n";
        out += "GPU " + system.gpu + ", driver " + system.driver + ", " +
               std::to_string(system.vram >> 20) + " MB video memory, " +
               std::to_string(system.ram >> 20) + " MB system memory\n";
        out += "OpenXR runtime " + (xr_runtime.empty() ? std::string("none") : xr_runtime) + "\n";
        out += std::string("Desktop mirror ") + (profile.fullscreen ? "fullscreen" : "windowed") +
               "\n";
        out += "graphics_config " +
               GraphicsText(ParseGraphics(VR::Ini::ValueOf(ini, "graphics_config"))) +
               ", aa_mode " + VR::Ini::ValueOf(ini, "aa_mode").value_or("default") + "\n";
        if (crash) {
            char code[16];
            snprintf(code, sizeof(code), "0x%08lX", crash->code);
            out += "Exit " + std::string(code) + " (" + crash->reason + ")\n";
            for (const auto &line : crash->lines)
                out += "  " + line + "\n";
        }
        return out;
    }
    // GAME FILES / HEADSET / GRAPHICS CARD under the launch options.
    void StatusCards() {
        constexpr double GB = double(1ull << 30);
        if (!ImGui::BeginTable("setup-status", 3, ImGuiTableFlags_SizingStretchSame))
            return;
        ImGui::TableNextColumn();
        ImGui::TextColored(gold, "%s", setup_issue.empty() ? "GAME FILES LOCATED" : "FINISH SETUP");
        Hint(setup_issue.empty() ? "Your game, runtime and save paths are set."
                                 : setup_issue.c_str());
        ImGui::TableNextColumn();
        if (xr_runtime.empty()) {
            ImGui::TextColored(muted, "HEADSET SETUP");
            Hint("Set your headset software (Meta Quest Link, SteamVR...) as the active OpenXR "
                 "runtime.");
        } else {
            ImGui::TextColored(gold, "HEADSET  /  %s", xr_runtime.c_str());
            Hint("Connect your headset. If it is not ready yet, the game waits for it.");
        }
        ImGui::TableNextColumn();
        ImGui::TextColored(system.notes.empty() ? gold : muted, "GRAPHICS CARD");
        std::string card = system.gpu.empty() ? "Not found" : system.gpu;
        if (!system.gpu.empty()) {
            char memory[64];
            snprintf(memory, sizeof(memory), "\n%.0f GB video memory, driver ", system.vram / GB);
            card += memory + system.driver;
        }
        for (const auto &note : system.notes)
            card += "\n" + note;
        Hint(card.c_str());
        ImGui::EndTable();
    }
    void BackupsWindow() {
        if (show_backups) {
            Try([&] { backups = SaveBackups(paths); });
            ImGui::OpenPopup("Save backups");
            show_backups = false;
        }
        ImGui::SetNextWindowSize({S(600), 0});
        if (!Modal("Save backups", nullptr, ImGuiWindowFlags_NoResize))
            return;
        ImGui::PushTextWrapPos(0);
        Hint("BB-VR copies your saves before every launch and keeps the last 10 different ones. "
             "Restoring backs up your current saves first.");
        ImGui::PopTextWrapPos();
        ListBegin("backup-list", {0, S(220)});
        if (backups.empty())
            Hint("No backups yet. One is made when you start the game.");
        for (const auto &backup : backups) {
            ImGui::PushID(PathText(backup.folder.filename()).c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(backup.label.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(70));
            ImGui::BeginDisabled(running);
            if (ImGui::SmallButton("Restore"))
                restore_target = backup.folder;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::BeginDisabled(running);
        if (ImGui::Button("Back up now"))
            Try([&] {
                const auto made = BackupSaves(paths, NormalizeSaveHome(Path(profile.saves)));
                backups = SaveBackups(paths);
                status = made.empty() ? "Your saves match the newest backup."
                                      : "Saves backed up.";
            });
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Open folder"))
            Try([&] {
                fs::create_directories(SaveBackupRoot(paths));
                OpenPath(SaveBackupRoot(paths));
            });
        ImGui::SameLine();
        if (ImGui::Button("Close"))
            ImGui::CloseCurrentPopup();
        if (restore_target)
            ImGui::OpenPopup("Restore saves?");
        if (Modal("Restore saves?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Replace your saves with this backup?");
            Hint("Your current saves are backed up first, so you can undo this.");
            if (ImGui::Button("Restore")) {
                Try([&] {
                    RequireStopped();
                    RestoreSaves(paths, NormalizeSaveHome(Path(profile.saves)), *restore_target);
                    backups = SaveBackups(paths);
                    status = "Saves restored. The previous saves are in the newest backup.";
                });
                restore_target.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                restore_target.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::EndPopup();
    }
    void CrashWindow() {
        if (show_crash) {
            ImGui::OpenPopup("The game stopped");
            show_crash = false;
        }
        ImGui::SetNextWindowSize({S(660), 0});
        if (!crash || !Modal("The game stopped", nullptr,
                                               ImGuiWindowFlags_NoResize))
            return;
        char code[16];
        snprintf(code, sizeof(code), "0x%08lX", crash->code);
        ImGui::PushTextWrapPos(0);
        ImGui::TextWrapped("BB-VR closed because of %s (code %s).", crash->reason.c_str(), code);
        Hint("Your saves were backed up before this session. Copy the support info into a bug "
             "report so it can be fixed.");
        ImGui::PopTextWrapPos();
        if (!crash->lines.empty()) {
            ImGui::TextColored(gold, "LAST PROBLEMS IN THE LOG");
            ListBegin("crash-lines", {0, S(150)}, ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto &line : crash->lines)
                Hint(line.c_str());
            ImGui::EndChild();
        }
        if (ImGui::Button("Try again")) {
            ImGui::CloseCurrentPopup();
            Try([&] { StartGame(); });
        }
        ImGui::SameLine();
        if (ImGui::Button("Open log"))
            Try([&] {
                const auto log = LogFile(paths);
                OpenPath(log.empty() ? paths.User() / "log" : log);
            });
        ImGui::SameLine();
        if (ImGui::Button("Copy support info")) {
            ImGui::SetClipboardText(SupportInfo().c_str());
            status = "Support info copied. Paste it into your bug report.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Close"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    std::optional<std::string> Value(const std::string &key) {
        auto it = edits.find(key);
        return it == edits.end() ? VR::Ini::ValueOf(ini, key) : it->second;
    }
    static void Details(const char *text) {
        if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetNextWindowSize({S(380), 0});
            ImGui::SetTooltip("%s", text);
        }
    }
    // One settings row: label and help on the left, the control on the right.
    template <class Control>
    void Row(const char *label, const char *help, const char *details, Control control) {
        ImGui::PushID(label);
        if (ImGui::BeginTable("row", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, S(205));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            Details(details);
            Hint(help);
            Details(details);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            control();
            Details(details);
            ImGui::EndTable();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    struct Pick {
        const char *label;
        bool selected, enabled;
        const char *why; // tooltip when not enabled
    };
    // A list of choices; returns the index picked this frame, -1 for none.
    int Combo(const char *current, const std::vector<Pick> &picks) {
        int picked = -1;
        if (ImGui::BeginCombo("##value", current)) {
            for (size_t i = 0; i < picks.size(); ++i) {
                ImGui::BeginDisabled(!picks[i].enabled);
                if (ImGui::Selectable(picks[i].label, picks[i].selected))
                    picked = int(i);
                ImGui::EndDisabled();
                if (!picks[i].enabled)
                    Details(picks[i].why);
            }
            ImGui::EndCombo();
        }
        return picked;
    }
    // A slider in the page's steps. Shows `text`; returns true with `value` snapped when moved.
    static bool StepSlider(int &value, int min, int max, int step, std::string text) {
        std::string format;
        for (char c : text)
            format += c == '%' ? std::string("%%") : std::string(1, c);
        int v = value;
        if (!ImGui::SliderInt("##value", &v, min, max, format.c_str(),
                              ImGuiSliderFlags_AlwaysClamp))
            return false;
        v = std::min(max, min + (v - min + step / 2) / step * step);
        if (v == value)
            return false;
        value = v;
        return true;
    }
    void Edit(const std::string &key, const std::optional<std::string> &value) {
        if (value == VR::Ini::ValueOf(ini, key))
            edits.erase(key);
        else
            edits[key] = value;
        status = edits.empty() ? "Settings match the saved file."
                               : "Unsaved settings. Save or launch to apply.";
    }
    GraphicsFields PendingGraphics() {
        return ParseGraphics(Value("graphics_config"));
    }
    void SetGraphics(const GraphicsFields &fields) {
        Edit("graphics_config", GraphicsText(fields));
    }
    fs::path RuntimePath() const {
        try {
            return Path(profile.runtime);
        } catch (const std::exception &) {
            return {};
        }
    }
    static std::string Signed(int value, const char *unit) {
        return (value > 0 ? "+" : "") + std::to_string(value) + unit;
    }
    // The in-game Graphics tab (vr_page.h graphics rows and GraphicsDraftStore::Set), on
    // graphics_config. The page's read-only headset resolution is left out.
    void GraphicsTab() {
        auto f = PendingGraphics();
        const bool dlss_ok = Available().dlss;
        const bool fsr4_ok = Available().fsr4;
        const bool dlss = f[2] == 1 && !f[5], fsr4 = f[2] == 2;
        Row("Scaling", "Relative to headset resolution.",
            "Each eye's resolution relative to the size your headset asks for: higher is sharper "
            "and slower.",
            [&] {
                if (StepSlider(f[1], -50, 50, 10, Signed(f[1], " %"))) {
                    SetGraphics(f);
                }
            });
        const char *no_dlss = !system.nvidia ? "DLSS needs an NVIDIA RTX graphics card."
                                             : "Needs the DLSS files, which this package does not "
                                               "include.";
        Row("DLSS", "Render smaller and reconstruct both eyes. DLSS turns FSR 4 and DLSS 5 off.",
            "Each eye is drawn at a lower resolution and rebuilt to full size by DLSS. "
            "Anti-aliasing is skipped while it is on.",
            [&] {
                const int pick = Combo(dlss ? "On" : "Off",
                                       {{"Off", !dlss, true, ""}, {"On", dlss, dlss_ok, no_dlss}});
                if (pick == 1) {
                    f[2] = 1;
                    f[5] = 0;
                } else if (pick == 0 && dlss)
                    f[2] = 0;
                if (pick >= 0)
                    SetGraphics(f);
            });
        Row("FSR 4", "Render smaller and reconstruct both eyes. FSR 4 turns DLSS and DLSS 5 off.",
            "Each eye is drawn at a lower resolution and rebuilt to full size by FSR 4, which "
            "needs a recent graphics card. Anti-aliasing is skipped while it is on.",
            [&] {
                const int pick =
                    Combo(fsr4 ? "On" : "Off",
                          {{"Off", !fsr4, true, ""},
                           {"On", fsr4, fsr4_ok,
                            "Needs the FSR 4 files, which this package does not include."}});
                if (pick == 1) {
                    f[2] = 2;
                    f[5] = 0;
                } else if (pick == 0 && fsr4)
                    f[2] = 0;
                if (pick >= 0)
                    SetGraphics(f);
            });
        Row("Profile", "Requires DLSS or FSR 4. Each remembers its own profile.",
            "Quality looks best and Performance is fastest. DLSS and FSR 4 each keep their own "
            "choice.",
            [&] {
                const char *names[]{"Performance", "Balanced", "Quality"};
                const int current = fsr4 ? f[4] : f[3];
                ImGui::BeginDisabled(!dlss && !fsr4);
                const int pick = Combo(names[current], {{names[0], current == 0, true, ""},
                                                        {names[1], current == 1, true, ""},
                                                        {names[2], current == 2, true, ""}});
                ImGui::EndDisabled();
                if (pick >= 0) {
                    f[fsr4 ? 4 : 3] = pick;
                    SetGraphics(f);
                }
            });
    }
    // The in-game DLSS 5 tab: on graphics_config, like the page's draft.
    void Dlss5Tab() {
        auto f = PendingGraphics();
        const bool available = Available().dlss5;
        const bool neural = f[2] == 1 && f[5] == 1;
        Row("DLSS 5", "Neural lighting and detail. Turns regular DLSS and FSR 4 off.",
            "DLSS 5 neural rendering, on an NVIDIA RTX card with the DLSS 5 files.", [&] {
                const int pick =
                    Combo(neural ? "On" : "Off",
                          {{"Off", !neural, true, ""},
                           {"On", neural, available,
                            !system.nvidia ? "DLSS 5 needs an NVIDIA RTX graphics card."
                                           : "Needs the DLSS 5 files, which this package does "
                                             "not include."}});
                if (pick == 1) {
                    f[2] = 1;
                    f[5] = 1;
                } else if (pick == 0) {
                    if (neural)
                        f[2] = 0;
                    f[5] = 0;
                }
                if (pick >= 0)
                    SetGraphics(f);
            });
        ImGui::BeginDisabled(!neural);
        Row("Profile", "Controls render and neural resolution together. Balanced is the default.",
            "Performance renders at half size, Balanced at two thirds. Choosing one also sets "
            "the three strengths below to that profile's values.",
            [&] {
                const int current = f[6] == 0 ? 0 : 1;
                const int pick = Combo(current ? "Balanced" : "Performance",
                                       {{"Performance", current == 0, true, ""},
                                        {"Balanced", current == 1, true, ""}});
                if (pick >= 0) {
                    // graphics_settings.h SetNeuralProfile / NeuralProfiles.
                    f[6] = pick;
                    f[7] = 100;
                    f[8] = pick ? 80 : 85;
                    f[9] = pick ? 85 : 50;
                    SetGraphics(f);
                }
            });
        const struct {
            const char *label, *help;
            int field;
        } strengths[]{
            {"Enhancement", "Strength of the neural treatment. Zero keeps the original look.", 7},
            {"Preserve detail", "Preserve the structure of the hunter, weapons and architecture.",
             8},
            {"Lighting treatment", "Strength of the neural lighting treatment. Requires DLSS 5.",
             9}};
        for (const auto &s : strengths)
            Row(s.label, s.help, s.help, [&] {
                if (StepSlider(f[s.field], 0, 100, 5, std::to_string(f[s.field]) + " %"))
                    SetGraphics(f);
            });
        ImGui::EndDisabled();
    }
    void NumberRow(const Setting &row) {
        const auto &key = row.keys[0];
        const auto value = Value(key);
        const auto parsed = value ? NumberShown(row, *value) : std::nullopt;
        const int shown = parsed.value_or(row.fallback);
        const bool zero = row.zero && shown == 0;
        std::string text = zero ? row.zero : row.sign ? Signed(shown, row.unit)
                                                      : std::to_string(shown) + row.unit;
        if (!value)
            text += "  (default)";
        else if (!parsed)
            text = "Custom: " + *value;
        Row(row.label, row.help, row.details, [&] {
            const int min = row.zero ? row.min - 1 : row.min;
            int slider = zero ? min : shown;
            if (StepSlider(slider, min, row.max, row.zero && slider < row.min ? 1 : row.step, text))
                Edit(key, row.zero && slider < row.min ? "0" : NumberValue(row, slider));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                Edit(key, std::nullopt);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("Right-click for the game default.");
        });
    }
    void SettingRow(const Setting &row) {
        if (row.Number()) {
            NumberRow(row);
            return;
        }
        std::vector<std::optional<std::string>> values;
        bool any = false;
        for (const auto &key : row.keys) {
            values.push_back(Value(key));
            any |= bool(values.back());
        }
        // Keep matches any value: the page's snap turns leave the smooth speed as it is.
        int selected = any ? -1 : 0;
        for (size_t i = 0; i < row.options.size() && selected < 1; ++i) {
            bool match = true;
            for (size_t j = 0; j < values.size(); ++j)
                match &= row.options[i].values[j] == Keep ||
                         (values[j] && SameValue(*values[j], row.options[i].values[j]));
            if (match && any)
                selected = int(i) + 1;
        }
        std::string custom = "Custom";
        for (const auto &v : values)
            custom += " / " + v.value_or("default");
        // The page greys anti-aliasing out while an upscaler is on: it is bypassed then.
        const bool locked = row.keys[0] == "aa_mode" && PendingGraphics()[2] != 0;
        Row(row.label, row.help, row.details, [&] {
            std::vector<Pick> picks{{"Game default", selected == 0, true, ""}};
            for (size_t i = 0; i < row.options.size(); ++i)
                picks.push_back({row.options[i].label, selected == int(i) + 1, true, ""});
            ImGui::BeginDisabled(locked);
            const int pick = Combo(selected < 0    ? custom.c_str()
                                   : selected == 0 ? "Game default"
                                                   : row.options[selected - 1].label,
                                   picks);
            ImGui::EndDisabled();
            if (pick == 0)
                for (const auto &key : row.keys)
                    Edit(key, std::nullopt);
            else if (pick > 0)
                for (size_t j = 0; j < row.keys.size(); ++j)
                    if (row.options[pick - 1].values[j] != Keep)
                        Edit(row.keys[j], row.options[pick - 1].values[j]);
        });
    }
    // What the custom tabs contain, for search.
    static constexpr const char *GraphicsWords =
        "graphics scaling relative to headset resolution dlss fsr 4 profile render smaller "
        "reconstruct upscaling";
    static constexpr const char *Dlss5Words =
        "dlss 5 neural lighting detail profile enhancement preserve treatment";
    void CategoryDefaults(const std::string &name) {
        // Anti-aliasing is left alone while an upscaler bypasses it, as the page does.
        const bool aa_locked = PendingGraphics()[2] != 0;
        for (const auto &row : Settings())
            if (name == row.category && !(aa_locked && row.keys[0] == "aa_mode"))
                for (const auto &key : row.keys)
                    Edit(key, std::nullopt);
        auto f = PendingGraphics();
        if (name == "Graphics") {
            // The page's Defaults: scale 0, DLSS and FSR 4 off, both profiles Balanced.
            f[1] = 0;
            if ((f[2] == 1 && !f[5]) || f[2] == 2)
                f[2] = 0;
            f[3] = f[4] = 1;
            SetGraphics(f);
        } else if (name == "DLSS 5") {
            if (f[2] == 1 && f[5])
                f[2] = 0;
            f[5] = 0;
            f[6] = DefaultGraphics[6];
            f[7] = DefaultGraphics[7];
            f[8] = DefaultGraphics[8];
            f[9] = DefaultGraphics[9];
            SetGraphics(f);
        }
        status = edits.empty() ? "Already at the game defaults."
                               : "Unsaved settings. Save or launch to apply.";
    }
    void SettingsPage() {
        Title("P R E F E R E N C E S", "Your way through the night.");
        Hint("The in-game VR settings, ready before you play. Changes apply on your next launch.");
        ImGui::Dummy({0, S(2)});
        const auto &categories = Categories();
        category = std::clamp(category, 0, int(categories.size()) - 1);
        // The page's tabs, wrapping onto a second line.
        const float available = ImGui::GetContentRegionAvail().x;
        float x = 0;
        for (int i = 0; i < int(categories.size()); ++i) {
            const float width =
                ImGui::CalcTextSize(categories[i]).x + ImGui::GetStyle().FramePadding.x * 2;
            if (i && x + ImGui::GetStyle().ItemSpacing.x + width <= available) {
                ImGui::SameLine();
                x += ImGui::GetStyle().ItemSpacing.x + width;
            } else
                x = width;
            const bool active = i == category;
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, {0.25f, 0.21f, 0.14f, 1});
            if (ImGui::Button(categories[i]))
                category = i;
            if (active)
                ImGui::PopStyleColor();
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##search", "Search all VR settings...", &search);
        ImGui::BeginDisabled(running || Busy());
        ImGui::BeginChild("settings-list", {0, -S(100)});
        bool matched = false;
        auto lower = [](std::string t) {
            std::transform(t.begin(), t.end(), t.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            return t;
        };
        const auto needle = lower(search);
        for (const auto *name : categories) {
            if (search.empty() && name != categories[category])
                continue;
            ImGui::PushID(name);
            bool heading = false;
            auto head = [&] {
                if (!search.empty() && !heading)
                    ImGui::TextColored(gold, "%s", name);
                heading = matched = true;
            };
            const std::string_view tab = name;
            if (tab == "Graphics" || tab == "DLSS 5") {
                if (search.empty() ||
                    lower(std::string(name) + " " + (tab == "Graphics" ? GraphicsWords : Dlss5Words))
                            .find(needle) != std::string::npos) {
                    head();
                    if (tab == "Graphics")
                        GraphicsTab();
                    else
                        Dlss5Tab();
                }
            }
            for (const auto &row : Settings()) {
                if (tab != row.category)
                    continue;
                if (!search.empty() &&
                    lower(std::string(row.category) + " " + row.label + " " + row.help)
                            .find(needle) == std::string::npos)
                    continue;
                head();
                SettingRow(row);
            }
            ImGui::PopID();
        }
        if (!matched)
            Hint("No settings match your search. Try a shorter word.");
        ImGui::EndChild();
        ImGui::BeginDisabled(edits.empty());
        if (ImGui::Button("Save settings"))
            Try([&] { SaveIni(); });
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reload")) {
            if (edits.empty())
                Try([&] {
                    Reload();
                    status = "Settings reloaded.";
                });
            else
                ImGui::OpenPopup("Discard changes?");
        }
        ImGui::SameLine();
        if (ImGui::Button("Import vr.ini"))
            Try([&] {
                RequireStopped();
                if (!edits.empty())
                    throw std::runtime_error("Save or reload your edits before importing.");
                if (auto file = PickPath(window_handle, false, L"Import VR settings", L"*.ini")) {
                    auto incoming = Read(*file);
                    if (incoming.find('\0') != incoming.npos)
                        throw std::runtime_error("Choose a text vr.ini file.");
                    Write(paths.Ini(), incoming, ini);
                    Reload();
                    status = "Imported. Previous settings were backed up.";
                }
            });
        ImGui::SameLine();
        if (ImGui::Button("Reset defaults"))
            ImGui::OpenPopup("Restore defaults?");
        ImGui::SameLine();
        const auto category_label = std::string(categories[category]) + " defaults";
        if (ImGui::Button(category_label.c_str()))
            CategoryDefaults(categories[category]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Set every %s option back to the game default. Save to apply.",
                              categories[category]);
        ImGui::EndDisabled();
        Hint("Custom INI entries are preserved. Reset restores every VR option.");
        if (Modal("Discard changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Discard unsaved VR settings and reload the file?");
            if (ImGui::Button("Discard")) {
                Try([&] { Reload(); });
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep editing"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (Modal("Restore defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(
                "Reset all VR options, including calibration and custom entries?");
            ImGui::TextUnformatted("A backup is kept. Your saves and paths stay unchanged.");
            if (ImGui::Button("Restore defaults")) {
                Try([&] {
                    RequireStopped();
                    Write(paths.Ini(), "", ini);
                    Reload();
                    status = "Defaults restored. A backup is in your data folder.";
                });
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    void UpdatesPage() {
        Title("T H E   L A T E S T   B U I L D", "Keep the hunt going.");
        Hint("Check when you want. Install when you are ready.");
        ImGui::Dummy({0, S(18)});
        ImGui::Text("Installed version  %s", BBVR_VERSION);
        ImGui::TextWrapped("%s", update_status.c_str());
        if (available)
            ImGui::TextColored(gold, "Version %s  /  %.1f MiB",
                               available->TargetFullRelease.Version.c_str(),
                               double(available->TargetFullRelease.Size) / (1024 * 1024));
        if (ImGui::Checkbox("Check for updates at startup", &profile.check_updates))
            Try([&] { profile.Save(paths); });
        ImGui::Dummy({0, S(6)});
        ImGui::BeginDisabled(Busy());
        if (ImGui::Button("Check for updates"))
            Check();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(Busy() || !available || downloaded);
        if (ImGui::Button("Download"))
            Download();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(Busy() || !downloaded || running);
        if (ImGui::Button("Install & restart"))
            Try([&] {
                RequireStopped();
                if (!edits.empty())
                    SaveIni();
                SaveProfile();
                updater->WaitExitThenApplyUpdates(*available);
                quit = true;
            });
        ImGui::EndDisabled();
        if (Busy() && downloading) {
            const auto percent = std::clamp(progress.load(), 0, 100);
            const auto label = std::to_string(percent) + "% downloaded";
            ImGui::ProgressBar(float(percent) / 100, {-1, S(22)}, label.c_str());
        }
        if (downloaded)
            Hint(running ? "Close the game to install this update."
                         : "Ready to install. The launcher will close and reopen automatically.");
        ImGui::Dummy({0, S(20)});
        ImGui::Separator();
        ImGui::TextColored(gold, "RELEASE NOTES");
        ImGui::BeginChild("release-notes", {0, S(150)});
        const char *notes = "Release notes appear here after checking.";
        if (available)
            notes = available->TargetFullRelease.NotesMarkdown.empty()
                        ? "No release notes were provided for this version."
                        : available->TargetFullRelease.NotesMarkdown.c_str();
        ImGui::TextWrapped("%s", notes);
        ImGui::EndChild();
        Hint("Updates replace the launcher and bundled runtime together. Settings and saves stay "
             "outside the installation. Play works offline.");
        ImGui::BeginDisabled(std::string_view(BBVR_RELEASES_URL).empty());
        if (ImGui::Button("View releases"))
            Try([&] { OpenPath(Path(BBVR_RELEASES_URL)); });
        ImGui::EndDisabled();
    }
    void Draw() {
        auto size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("BB-VR", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.035f, 0.038f, 0.04f, 1});
        ImGui::BeginChild("sidebar", {S(187), 0});
        ImGui::SetCursorPos({S(22), S(38)});
        ImGui::PushFont(brand_font);
        ImGui::TextColored(gold, "BB-VR");
        ImGui::PopFont();
        ImGui::SetCursorPosY(S(140));
        const char *pages[]{"Play", edits.empty() ? "VR settings" : "VR settings *",
                            available ? "Updates *" : "Updates"};
        for (int i = 0; i < 3; ++i) {
            ImGui::SetCursorPosX(S(16));
            if (ImGui::Selectable(pages[i], page == i, 0, {S(155), S(35)}))
                page = i;
        }
        ImGui::SetCursorPos({S(22), size.y - S(185)});
        if (ImGui::SmallButton("Save backups"))
            show_backups = true;
        ImGui::SetCursorPosX(S(22));
        if (ImGui::SmallButton("Desktop shortcut"))
            Try([&] {
                CreatePlayShortcut();
                status = "\"Play BB-VR\" is on your desktop. It starts the game with your saved "
                         "settings, without this window.";
            });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A desktop shortcut that starts the game straight away.");
        ImGui::SetCursorPosX(S(22));
        if (ImGui::SmallButton("Data folder"))
            Try([&] { OpenPath(paths.root); });
        ImGui::SetCursorPosX(S(22));
        if (ImGui::SmallButton("Open logs"))
            Try([&] {
                fs::create_directories(paths.User() / "log");
                OpenPath(paths.User() / "log");
            });
        ImGui::Dummy({0, S(6)});
        ImGui::SetCursorPosX(S(22));
        Hint("NATIVE / WINDOWS");
        ImGui::SetCursorPosX(S(22));
        Hint(BBVR_VERSION);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("main", {0, 0});
        ImGui::SetCursorPos({S(32), S(32)});
        ImGui::BeginChild("content", {ImGui::GetContentRegionAvail().x - S(32),
                                      ImGui::GetContentRegionAvail().y - S(page == 0 ? 125 : 60)});
        if (page == 0)
            PlayPage();
        else if (page == 1)
            SettingsPage();
        else
            UpdatesPage();
        ImGui::EndChild();
        if (page == 0) {
            ImGui::SetCursorPosX(S(32));
            ImGui::BeginChild("play-action", {ImGui::GetContentRegionAvail().x - S(32), S(55)});
            PlayButton();
            ImGui::EndChild();
        }
        ImGui::SetCursorPosX(S(32));
        ImGui::PushTextWrapPos(size.x - S(240));
        Hint(status.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        BackupsWindow();
        CrashWindow();
        if (!error.empty())
            ImGui::OpenPopup("Something needs attention");
        if (Modal("Something needs attention", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(S(500));
            ImGui::TextWrapped("%s", error.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("OK", {S(100), 0})) {
                error.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (close_requested) {
            if (Busy()) {
                status = "An update operation is in progress. Please wait before closing.";
                close_requested = false;
            } else if (edits.empty())
                quit = true;
            else {
                ImGui::OpenPopup("Unsaved settings");
                close_requested = false;
            }
        }
        if (Modal("Unsaved settings", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Save your VR settings before closing?");
            if (ImGui::Button("Save & close"))
                Try([&] {
                    SaveIni();
                    quit = true;
                });
            ImGui::SameLine();
            if (ImGui::Button("Discard & close"))
                quit = true;
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::End();
    }
};
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        Velopack::VelopackApp::Build().SetAutoApplyOnStartup(false).Run();
        int argc = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argc == 3 && std::wstring_view(args[1]) == L"--version-file") {
            Write(fs::absolute(args[2]), BBVR_VERSION);
            LocalFree(args);
            return 0;
        }
        if (argc == 4 && std::wstring_view(args[1]) == L"--update-smoke") {
            auto feed = fs::absolute(args[2]);
            auto marker = fs::absolute(args[3]);
            LocalFree(args);
            try {
                const Velopack::UpdateOptions options{false, "vr-win", -1};
                Velopack::UpdateManager manager(PathText(feed), &options);
                auto update = manager.CheckForUpdates();
                if (!update)
                    throw std::runtime_error("No newer test package.");
                manager.DownloadUpdates(*update);
                manager.WaitExitThenApplyUpdates(*update, true, true,
                                                 {"--version-file", PathText(marker)});
                return 0;
            } catch (const std::exception &ex) {
                Write(fs::path(marker.native() + L".error"), ex.what());
                return 1;
            }
        }
        if (argc == 2 && std::wstring_view(args[1]) == L"--play") {
            // The desktop shortcut: start the game with the saved launcher settings, no window.
            LocalFree(args);
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const HANDLE starting = CreateMutexW(nullptr, FALSE, L"Local\\BloodborneVR.Play");
            WaitForSingleObject(starting, 30000);
            struct Release {
                HANDLE h;
                ~Release() {
                    ReleaseMutex(h);
                    CloseHandle(h);
                }
            } release{starting};
            try {
                if (RuntimeRunning())
                    throw std::runtime_error("BB-VR is already running.");
                const auto paths = Paths::Default();
                CloseHandle(Launch(Prepare(Profile::Load(paths), paths)));
            } catch (const std::exception &ex) {
                MessageBoxW(nullptr,
                            (Wide(ex.what()) + L"\n\nOpen BB-VR from the Start menu to fix this.")
                                .c_str(),
                            L"BB-VR", MB_OK | MB_ICONWARNING);
                return 1;
            }
            return 0;
        }
        bool preview = argc == 3 && std::wstring_view(args[1]) == L"--preview";
        bool idle_smoke = argc == 3 && std::wstring_view(args[1]) == L"--idle-smoke";
        fs::path preview_dir = (preview || idle_smoke) ? fs::absolute(args[2]) : fs::path{};
        LocalFree(args);
        HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\BloodborneVR.Native");
        if (GetLastError() == ERROR_ALREADY_EXISTS && !preview) {
            if (HWND existing = FindWindowW(L"BloodborneVR.Native", nullptr)) {
                ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            CloseHandle(singleton);
            return 0;
        }
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ImGui_ImplWin32_EnableDpiAwareness();
        scale = preview ? 1
                        : ImGui_ImplWin32_GetDpiScaleForMonitor(
                              MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
        WNDCLASSEXW wc{sizeof(wc),
                       CS_CLASSDC,
                       WindowProc,
                       0,
                       0,
                       instance,
                       nullptr,
                       LoadCursor(nullptr, IDC_ARROW),
                       nullptr,
                       nullptr,
                       L"BloodborneVR.Native",
                       nullptr};
        RegisterClassExW(&wc);
        RECT r{0, 0, LONG(S(980)), LONG(S(740))};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        window_handle = CreateWindowW(wc.lpszClassName, L"BB-VR", WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                      r.bottom - r.top, nullptr, nullptr, instance, nullptr);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window_handle, 20, &dark, sizeof(dark));
        CreateGraphics(window_handle);
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        wchar_t win_dir[MAX_PATH]{};
        GetWindowsDirectoryW(win_dir, MAX_PATH);
        auto fonts = fs::path(win_dir) / "Fonts";
        auto add_font = [&](const wchar_t *name, float size) {
            auto f = fonts / name;
            return fs::exists(f) ? io.Fonts->AddFontFromFileTTF(PathText(f).c_str(), S(size))
                                 : io.Fonts->AddFontDefault();
        };
        add_font(L"segoeui.ttf", 16);
        heading_font = add_font(L"georgia.ttf", 32);
        brand_font = add_font(L"georgia.ttf", 17);
        font_scale = scale;
        Theme();
        ImGui_ImplWin32_Init(window_handle);
        ImGui_ImplDX11_Init(device.Get(), context.Get());
        if (preview)
            fs::create_directories(preview_dir);
        App app((preview || idle_smoke) ? Paths{preview_dir / "data"} : Paths::Default(),
                preview || idle_smoke);
        if (!preview && !idle_smoke) {
            ShowWindow(window_handle, SW_SHOWDEFAULT);
            UpdateWindow(window_handle);
            if (app.profile.check_updates)
                app.Check();
        }
        uint64_t last_tick = 0;
        int preview_frames = 0, preview_step = 0;
        const auto idle_start = GetTickCount64();
        uint64_t baseline_cpu = 0;
        int rendered_frames = 0, baseline_frames = 0;
        auto cpu_ticks = [] {
            FILETIME creation{}, exit{}, kernel{}, user{};
            GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
            return (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                   (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
        };
        while (!app.quit) {
            if (idle_smoke && !baseline_cpu && GetTickCount64() - idle_start > 1000) {
                baseline_cpu = cpu_ticks();
                baseline_frames = rendered_frames;
            }
            if (idle_smoke && GetTickCount64() - idle_start > 6000) {
                PROCESS_MEMORY_COUNTERS counters{sizeof(counters)};
                GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
                Write(preview_dir / "idle.json",
                      Json{{"idle_cpu_ms", (cpu_ticks() - baseline_cpu) / 10000.0},
                           {"sample_seconds", 5},
                           {"working_set_bytes", counters.WorkingSetSize},
                           {"idle_rendered_frames", rendered_frames - baseline_frames}}
                          .dump(2));
                app.quit = true;
                break;
            }
            if (!preview)
                MsgWaitForMultipleObjectsEx(0, nullptr,
                                            repaint > 0  ? 16
                                            : app.Busy() ? 100
                                                         : 1000,
                                            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT)
                    app.quit = true;
            }
            if (GetTickCount64() - last_tick >= 1000 || app.Busy()) {
                app.Tick();
                last_tick = GetTickCount64();
                if (app.Busy())
                    repaint = 1;
            }
            if (!preview && (IsIconic(window_handle) || (repaint == 0 && !io.WantTextInput)))
                continue;
            if (resize_width && resize_height) {
                target.Reset();
                Require(swapchain->ResizeBuffers(0, resize_width, resize_height,
                                                 DXGI_FORMAT_UNKNOWN, 0),
                        "Cannot resize.");
                resize_width = resize_height = 0;
                RenderTarget();
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.Draw();
            ++rendered_frames;
            ImGui::Render();
            const float clear[]{0.055f, 0.058f, 0.061f, 1};
            auto *rt = target.Get();
            context->OMSetRenderTargets(1, &rt, nullptr);
            context->ClearRenderTargetView(rt, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (preview && ++preview_frames % 4 == 0) {
                // Each page, then the Graphics tab and the backups and crash windows.
                const char *names[]{"play.png",     "settings.png", "updates.png",
                                    "graphics.png", "crash.png",    "backups.png"};
                Capture(preview_dir / names[preview_step]);
                switch (++preview_step) {
                case 1:
                case 2:
                    app.page = preview_step;
                    break;
                case 3:
                    app.page = 1;
                    app.category = 6; // Graphics
                    break;
                case 4:
                    app.page = 0;
                    app.crash = App::Crash{0xC0000005, ExitReason(0xC0000005),
                                           {"[Debug] <Critical> signals.cpp:175 Unhandled "
                                            "Exception code 0xC0000005 (preview)"}};
                    app.show_crash = true;
                    break;
                case 5:
                    // Opening it at the same popup level replaces the crash window.
                    app.show_backups = true;
                    break;
                default:
                    app.quit = true;
                }
            }
            swapchain->Present(1, 0);
            if (repaint > 0)
                --repaint;
        }
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        target.Reset();
        swapchain.Reset();
        context.Reset();
        device.Reset();
        DestroyWindow(window_handle);
        UnregisterClassW(wc.lpszClassName, instance);
        CloseHandle(singleton);
        CoUninitialize();
        return 0;
    } catch (const std::exception &ex) {
        MessageBoxW(nullptr, Wide(ex.what()).c_str(), L"BB-VR", MB_OK | MB_ICONERROR);
        return 1;
    }
}
