// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core.h"
#include "benchmark.h"
#include <mutex>
#include "resource.h"
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
#include <psapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
using namespace Launcher;
using Microsoft::WRL::ComPtr;
// The bridge (BBVR_BRIDGE=1): the last release under the old installer id, BloodborneVR, which every
// launcher up to 0.2.6 updates to. It checks the old update channel and moves the player to the
// new installer (id Bldbrne-VR) with one button. Its locks are its own, so the new launcher the new
// installer starts is not taken for this one.
#ifndef BBVR_BRIDGE
#define BBVR_BRIDGE 0
#endif
static constexpr const char *UpdateChannel = BBVR_BRIDGE ? "vr-win" : "win";
#if BBVR_BRIDGE
#define BBVR_LOCK L"Bldbrne-VR.Bridge"
#else
#define BBVR_LOCK L"Bldbrne-VR"
#endif
#if BBVR_BRIDGE
#include <urlmon.h>
#include <wininet.h>
static constexpr const wchar_t *NewSetupUrl =
    L"https://github.com/bldbrne-vr/bldbrne-vr/releases/latest/download/Bldbrne-VR-win-Setup.exe";
static constexpr const wchar_t *NewPortableUrl =
    L"https://github.com/bldbrne-vr/bldbrne-vr/releases/latest/download/Bldbrne-VR-win-Portable.zip";
// An installed copy: <root>\Update.exe and <root>\current\<this exe>, without the portable ZIP's
// .portable mark.
static bool InstalledCopy() {
    const auto dir = ExecutableDirectory();
    const auto root = dir.parent_path();
    return dir.filename() == L"current" && fs::exists(root / "Update.exe") &&
           !fs::exists(root / ".portable");
}
static std::wstring Environment(const wchar_t *name) {
    std::wstring value(32768, L'\0');
    const DWORD size = GetEnvironmentVariableW(name, value.data(), DWORD(value.size()));
    value.resize(size < value.size() ? size : 0);
    return value;
}
// Downloads the new installer, runs it and waits: it installs Bldbrne-VR and starts it. Throws
// unless the new launcher is then in place. Tests: BBVR_BRIDGE_SETUP is a local Setup.exe,
// BBVR_BRIDGE_INSTALLTO a folder to install to silently.
static void InstallNewVersion() {
    fs::path setup = Environment(L"BBVR_BRIDGE_SETUP");
    if (setup.empty()) {
        std::wstring temp(MAX_PATH + 1, L'\0');
        temp.resize(GetTempPathW(DWORD(temp.size()), temp.data()));
        setup = fs::path(temp) / L"Bldbrne-VR-win-Setup.exe";
        std::error_code ignored;
        fs::remove(setup, ignored);
        DeleteUrlCacheEntryW(NewSetupUrl); // never yesterday's installer
        if (FAILED(URLDownloadToFileW(nullptr, NewSetupUrl, setup.c_str(), 0, nullptr)) ||
            !fs::exists(setup))
            throw std::runtime_error(
                "Could not download the new installer. Check your connection and try again.");
    }
    const auto install_to = Environment(L"BBVR_BRIDGE_INSTALLTO");
    const auto root = install_to.empty() ? LocalAppData() / L"Bldbrne-VR" : fs::path(install_to);
    std::wstring command = Quote(setup.native());
    if (!install_to.empty())
        command += L" --silent --installto " + Quote(install_to);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process))
        throw std::runtime_error("Could not start the new installer.");
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (code != 0 || !fs::exists(root / L"current" / L"Bldbrne-VR.exe"))
        throw std::runtime_error("The new installer did not finish. This copy still works; try "
                                 "again.");
}
// This installation removes itself (files, Start menu entry, Apps list entry) a few seconds after
// this launcher has closed. The data folder is outside it and stays.
static void UninstallThisCopy() {
    const auto update = ExecutableDirectory().parent_path() / L"Update.exe";
    std::wstring command = L"cmd.exe /c ping -n 6 127.0.0.1 >nul & " + Quote(update.native()) +
                           L" --uninstall --silent";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        throw std::runtime_error("The new version is installed. Remove this old copy from "
                                 "Windows' Apps list.");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}
#endif
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
    std::string preview_update; // --preview only: the Play page's update button, with this version
    HANDLE game_process{};
    std::unique_ptr<Velopack::UpdateManager> updater;
    std::optional<Velopack::UpdateInfo> available;
    std::future<UpdateResult> task;
    std::future<Benchmark::Report> benchmark_task;
    std::future<Benchmark::Headset> headset_task;
    struct PendingGameLaunch {
        Profile profile;
        std::string expected_ini, expected_profile;
    };
    std::optional<PendingGameLaunch> pending_game_launch;
    int frame_rate_draft=90;
    bool frame_rate_editing=false;
    struct PendingBenchmarkApply {
        Benchmark::Candidate candidate;
        int target_hz;
        std::string expected_ini, expected_profile;
    };
    std::optional<PendingBenchmarkApply> pending_benchmark_apply;
    std::future<Benchmark::Headset> benchmark_apply_task;
    std::optional<Benchmark::Headset> benchmark_headset;
    std::string headset_error;
    bool open_perf_scan = false;
    std::optional<Benchmark::Report> benchmark_report;
    std::atomic_bool benchmark_cancel{false};
    std::mutex benchmark_mutex;
    std::string benchmark_progress;
    std::vector<std::string> benchmark_log;
    std::chrono::steady_clock::time_point benchmark_log_start;
    bool benchmark_log_new = false;
    std::optional<std::chrono::steady_clock::time_point> benchmark_run_start;

    void LogBenchmark(const std::string& message) {
        std::lock_guard lock(benchmark_mutex);
        benchmark_progress=message;
        const auto seconds=std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now()-benchmark_log_start).count();
        const auto stamp=std::to_string(seconds/60)+":"+(seconds%60<10?"0":"")+std::to_string(seconds%60);
        if(benchmark_log.size()>=200) benchmark_log.erase(benchmark_log.begin());
        benchmark_log.push_back(stamp+"  "+message);
        benchmark_log_new=true;
    }
    void BenchmarkLog() {
        std::vector<std::string> lines;
        bool added;
        {
            std::lock_guard lock(benchmark_mutex);
            lines=benchmark_log;
            added=benchmark_log_new;
            benchmark_log_new=false;
        }
        if(lines.empty()) return;
        ImGui::TextUnformatted("SCAN ACTIVITY");
        if(ImGui::BeginChild("scan-activity",{0,S(110)},ImGuiChildFlags_Borders)) {
            const bool at_bottom=ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-S(4);
            for(const auto& line:lines) ImGui::TextWrapped("%s",line.c_str());
            if(added && at_bottom) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    int benchmark_hz = 90;
    bool benchmark_auto_hz = true;
    bool benchmark_thorough = false;

#if BBVR_BRIDGE
    std::future<std::string> moving; // InstallNewVersion's error, empty when it worked
    std::string move_error;
    bool installed_copy = InstalledCopy();
#endif
    std::atomic<int> progress{};
    bool downloaded{};
    bool downloading{};
    std::string setup_issue;
    std::string update_status = "Check for a new launcher and VR runtime release.";
    SystemInfo system;
    std::future<SystemInfo> system_task; // the full ReadSystem, with the multiview probe
    // The system with the multiview probe's result (waits for it the first time, ~0.1 s).
    const SystemInfo &FullSystem() {
        if (system_task.valid())
            system = system_task.get();
        return system;
    }
    std::string xr_runtime;
    bool shaders_ready{};
    // Recheck the build when either its executable or version metadata changes.
    std::string build_issue;
    fs::path build_checked;
    fs::file_time_type build_time{}, build_sfo_time{};
    std::vector<SaveBackup> backups;
    std::optional<fs::path> restore_target;
    bool show_backups{}, show_crash{};
    std::optional<DWORD> last_exit;
    std::string session_game_info;
    struct Crash {
        DWORD code;
        std::string reason;
        std::vector<std::string> lines;
        std::string game_info;
    };
    std::optional<Crash> crash;
    struct Upscalers {
        std::string runtime;
        bool dlss{}, fsr4{}, dlss5{}, fsr31{};
    } upscalers{"\x01"};
    const Upscalers &Available() {
        if (upscalers.runtime != profile.runtime) {
            const auto runtime = RuntimePath();
            upscalers = {profile.runtime, system.nvidia && DlssIncluded(runtime),
                         Fsr4Included(runtime), system.nvidia && Dlss5Included(runtime), Fsr31Included(runtime)};
        }
        return upscalers;
    }
    App(Paths p, bool pv) : paths(std::move(p)), preview(pv) {
        fs::create_directories(paths.User());
        profile = Profile::Load(paths);
        Reload();
        // The Vulkan multiview probe (0.1 s or more with overlay layers) runs off the UI thread;
        // until it reports, the window shows the DXGI facts.
        system = ReadSystem(false);
        system_task = std::async(std::launch::async, [] { return ReadSystem(); });
        xr_runtime = OpenXrRuntime();
        shaders_ready = ShaderCacheReady(paths);
        if (!shaders_ready)
            status = FirstLaunchNote;
    }
    static constexpr const char *FirstLaunchNote =
        "First launch: the game builds its shaders, so the first few minutes may stutter. Later "
        "launches are smooth.";
    ~App() {
        benchmark_cancel=true;
        if(system_task.valid()) system_task.wait();
        if(benchmark_task.valid()) benchmark_task.wait();
        if(headset_task.valid()) headset_task.wait();
        if(benchmark_apply_task.valid()) benchmark_apply_task.wait();
        if (task.valid())
            task.wait();
        if (game_process)
            CloseHandle(game_process);
    }
    bool Busy() const {
#if BBVR_BRIDGE
        if (moving.valid())
            return true;
#endif
        return task.valid() || benchmark_task.valid() || headset_task.valid() || benchmark_apply_task.valid();
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
        HANDLE benchmark=OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\BldbrneVRBenchmark");
        if(benchmark) {CloseHandle(benchmark);throw std::runtime_error("Cancel or finish the benchmark first.");}
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
                    const Velopack::UpdateOptions options{false, UpdateChannel, -1};
                    updater = std::make_unique<Velopack::UpdateManager>(
                        std::make_unique<Velopack::HttpSource>(
                            "https://github.com/bldbrne-vr/bldbrne-vr/releases/latest/download",
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
        if(benchmark_apply_task.valid() && benchmark_apply_task.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            Try([&] {
                const auto verified_headset=benchmark_apply_task.get();
                if(benchmark_cancel) throw std::runtime_error("Applying the recommendation was cancelled. Settings unchanged.");
                RequireStopped();
                const auto& pending=*pending_benchmark_apply;
                if(Read(paths.Profile())!=pending.expected_profile)
                    throw std::runtime_error("Launcher settings changed during verification. Review and apply again.");
                profile=Benchmark::ApplySettings(paths,profile,pending.candidate,pending.target_hz,pending.expected_ini);
                benchmark_headset=verified_headset;
                Reload();
                status="Benchmark recommendation applied. Try it in VR to assess visual quality.";
                LogBenchmark(status);
            });
            pending_benchmark_apply.reset();
            repaint=3;
        }
        if(system_task.valid() && system_task.wait_for(std::chrono::seconds(0))==std::future_status::ready)
            system=system_task.get();
        if(headset_task.valid() && headset_task.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            try { benchmark_headset=headset_task.get(); headset_error.clear(); }
            catch(const std::exception& e) { benchmark_headset.reset(); headset_error=e.what(); }
            if(pending_game_launch) {
                auto pending=std::move(*pending_game_launch);
                pending_game_launch.reset();
                Try([&] {
                    if(benchmark_cancel) throw std::runtime_error("Launch cancelled.");
                    RequireStopped();
                    if(Read(paths.Profile())!=pending.expected_profile || Read(paths.Ini())!=pending.expected_ini)
                        throw std::runtime_error("Settings changed during headset detection. Launch again.");
                    pending.profile.refresh_hz=GameRefreshRate(true,pending.profile.refresh_hz,
                        benchmark_headset ? benchmark_headset->refresh_hz : 0);
                    LaunchGame(pending.profile);
                });
            }
            repaint=3;
        }
        // Benchmark processes have their own lifecycle; never report them as normal gameplay
        // or read crash information from the player's unrelated live profile.
        if(benchmark_task.valid()) {
            if(benchmark_task.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
                benchmark_report=benchmark_task.get();
                if(!benchmark_report->error.empty()) status=benchmark_report->error;
                else if(benchmark_report->cancelled) status="Benchmark cancelled. Your settings were not changed.";
                else status="Performance scan finished. Open PERF SCAN to review the recommendation.";
                if(!benchmark_report->error.empty()) LogBenchmark("Scan stopped: "+benchmark_report->error);
                else if(benchmark_report->cancelled) LogBenchmark("Scan cancelled. Settings unchanged.");
                else if(benchmark_report->recommended)
                    LogBenchmark("Recommended: "+benchmark_report->results[*benchmark_report->recommended].candidate.label+". Review below; settings are not applied yet.");
                else if(benchmark_report->provisional) LogBenchmark("Provisional suggestion available: one usable run, not yet confirmed. Review below.");
                else LogBenchmark("Scan finished without a confirmed recommendation. See the measurement reasons below.");
                repaint=3;
            }
            return;
        }

        if (task.valid() && task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto r = task.get();
            available = std::move(r.release);
            downloaded = r.downloaded;
            downloading = false;
            update_status = std::move(r.message);
            if (!r.error.empty())
                status = std::move(r.error);
            repaint = 3;
        }
#if BBVR_BRIDGE
        if (moving.valid() &&
            moving.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            move_error = moving.get();
            if (move_error.empty()) {
                // The new launcher is open: this old copy closes and removes itself.
                try {
                    UninstallThisCopy();
                } catch (const std::exception &) {
                }
                quit = true;
            }
            repaint = 3;
        }
#endif
        // Headset probes are shadPS4 processes too, but are not gameplay sessions.
        if(headset_task.valid() || benchmark_apply_task.valid()) return;
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
                // The game's crash record names an out-of-memory stop (LogProblems puts its advice
                // first): say that, not the generic check.
                const bool out_of_memory =
                    std::any_of(problems.begin(), problems.end(), [](const std::string &line) {
                        return line.find("ran out of memory") != std::string::npos;
                    });
                crash = Crash{*last_exit,
                              out_of_memory ? "the PC running out of memory"
                              : *last_exit  ? ExitReason(*last_exit)
                                            : "the game could not be loaded",
                              problems, session_game_info};
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
                        &value == &profile.saves ? NormalizeSaveHome(*p) : *p);
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
        if (ImGui::Checkbox("Fullscreen desktop mirror", &profile.fullscreen))
            profile.fullscreen_chosen = true; // the player's own choice from now on
        ImGui::SameLine();
        if (ImGui::Button("Save paths"))
            Try([&] {
                RequireStopped();
                SaveProfile();
            });
    }
#if BBVR_BRIDGE
    // Updates continue under the new installer: one button installs it (settings and saves come
    // along, they are outside both installations) and removes this copy. A portable copy gets the
    // new portable ZIP instead.
    void BridgeNotice() {
        ImGui::Dummy({0, S(4)});
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - S(130)); // clear of the motif
        if (installed_copy)
            ImGui::TextColored(gold, "Bldbrne-VR has a new installer. Install it once to keep "
                                     "getting updates. Your settings and saves come with you, "
                                     "and this old copy removes itself afterwards.");
        else
            ImGui::TextColored(gold, "Bldbrne-VR has a new download. Get the new portable ZIP to "
                                     "keep getting updates. It uses the same settings and saves.");
        ImGui::PopTextWrapPos();
        ImGui::BeginDisabled(running || Busy());
        if (installed_copy) {
            if (ImGui::Button(moving.valid() ? "Installing the new version..."
                                             : "Install the new version"))
                Try([&] {
                    RequireStopped();
                    if (!edits.empty())
                        SaveIni();
                    SaveProfile();
                    move_error.clear();
                    moving = std::async(std::launch::async, [] {
                        try {
                            InstallNewVersion();
                            return std::string{};
                        } catch (const std::exception &ex) {
                            return std::string(ex.what());
                        }
                    });
                });
        } else if (ImGui::Button("Get the new portable ZIP")) {
            Try([&] { OpenPath(NewPortableUrl); });
        }
        ImGui::EndDisabled();
        if (!move_error.empty())
            ImGui::TextWrapped("%s", move_error.c_str());
    }
#endif
    void PlayPage() {
        Title(nullptr, "The hunt awaits.");
        Hint("Return to Yharnam. See it with your own eyes.");
#if BBVR_BRIDGE
        BridgeNotice();
#endif
        const std::string next =
            available ? available->TargetFullRelease.Version : preview_update;
        if (!next.empty()) {
            // A newer release was found (the check at startup, or Check for updates): a big gold
            // button here, where everyone looks. It opens Updates and starts the download at once
            // (2026-10-18, the tester: the small link only switched the page).
            ImGui::Dummy({0, S(8)});
            ImGui::PushStyleColor(ImGuiCol_Button, gold);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.80f, 0.70f, 0.48f, 1});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.57f, 0.49f, 0.33f, 1});
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.06f, 0.05f, 0.04f, 1});
            const std::string label = "UPDATE TO " + next + " NOW";
            if (ImGui::Button(label.c_str(), {S(300), S(44)})) {
                page = 2;
                if (available && !downloaded)
                    Download();
            }
            ImGui::PopStyleColor(4);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(gold, "  A new version is ready.");
        }
        Motif();
        ImGui::BeginDisabled(running || Busy());
        Field("Game folder", "Your game folder (version 1.09), containing eboot.bin.", profile.game,
              true);
        Field("Saves folder (optional)",
              "Leave as it is to keep your saves with the mod. To continue a save from another "
              "shadPS4, choose its home folder (home / 1000 / savedata / CUSA...) or that savedata "
              "folder: those saves are used in place.",
              profile.saves, true);
        if (ImGui::CollapsingHeader("Runtime and setup")) {
            // Always the included runtime (Profile::Load): no other shadPS4 is needed, nor used
            // (another build lacks the VR renderer, its fixes and patches).
            Hint(profile.runtime.empty()
                     ? "The included VR runtime is missing. Use a complete package."
                     : "The game runs on the included VR runtime (a custom shadPS4), updated with "
                       "the launcher. No other shadPS4 is needed.");
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
            // No param.sfo: CheckGameBuild says what is wrong (not the file system's error).
            std::error_code sfo_error;
            const auto sfo_time =
                fs::last_write_time(game.parent_path() / "sce_sys" / "param.sfo", sfo_error);
            if (game != build_checked || time != build_time || sfo_time != build_sfo_time) {
                build_checked = game;
                build_time = time;
                build_sfo_time = sfo_time;
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
        if(pending_game_launch) {
            Hint("Detecting headset refresh rate before launch (90 FPS fallback)...");
            if(ImGui::Button("Cancel launch")) benchmark_cancel=true;
        }
    }
    void StartGame() {
        if(Busy()) throw std::runtime_error("Wait for the current launcher operation to finish.");
        RequireStopped();
        SaveProfile();
        if (!edits.empty())
            SaveIni();
        if(profile.follow_headset) {
            PendingGameLaunch pending{profile,Read(paths.Ini()),Read(paths.Profile())};
            DetectBenchmarkHeadset();
            if(!headset_task.valid()) throw std::runtime_error("Headset detection could not start.");
            pending_game_launch=std::move(pending);
            status="Detecting headset refresh rate before launch...";
            return;
        }
        LaunchGame(profile);
    }
    void LaunchGame(const Profile& launch_profile) {
        auto plan = Prepare(launch_profile, paths);
        Reload();
        game_process = Launch(plan);
        session_game_info = std::move(plan.game_support_info);
        last_exit.reset();
        crash.reset();
        running = true;
        status = shaders_ready ? "Starting Bldbrne-VR..." : FirstLaunchNote;
        ShowWindow(window_handle, SW_MINIMIZE);
    }
    // Everything a bug report needs, in one paste.
    std::string SupportInfo() const {
        std::string out = "Bldbrne-VR launcher " BBVR_VERSION "\n";
        try {
            const auto info = Json::parse(Read(ExecutableDirectory() / "build-info.json"));
            out += "Package " + info.value("version", std::string{}) + " from " +
                   info.value("source_commit", std::string{}) + "\n";
        } catch (const std::exception &) {
            out += "Package: development build\n";
        }
        out += "Runtime " + profile.runtime + "\n";
        // A crash keeps the identity of the game we launched, even if the selected folder changes.
        if (crash && !crash->game_info.empty())
            out += crash->game_info;
        else if (!session_game_info.empty())
            out += session_game_info;
        else {
            try {
                out += GameSupportInfo(GameExecutable(Path(profile.game)));
            } catch (const std::exception &) {
                out += "Game metadata unavailable\nGame eboot.bin SHA-256 unavailable\n";
            }
        }
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
        out += profile.SelectedStereo(system) == StereoPreset::Dibr
                   ? "Stereo preset DIBR\n" : "Stereo preset Native\n";
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
        if (profile.SelectedStereo(system) == StereoPreset::Dibr)
            card += "\nStarts in DIBR to reduce video memory use.";
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
        Hint("Bldbrne-VR copies your saves before every launch and keeps the last 10 different ones. "
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
        ImGui::TextWrapped("Bldbrne-VR closed because of %s (code %s).", crash->reason.c_str(), code);
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
    void Edit(const std::string &key, std::optional<std::string> value) {
        // A reset (the game default, a tab's defaults, a right-click) goes through ResetValue.
        if (!value)
            value = ResetValue(key);
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
    void DetectBenchmarkHeadset() {
        if(Busy() || running) return;
        benchmark_headset.reset();
        headset_error.clear();
        const auto runtime=RuntimePath();
        benchmark_cancel=false;
        headset_task=std::async(std::launch::async,[this,runtime] { return Benchmark::ScanHeadset(runtime,&benchmark_cancel); });
    }
    void BenchmarkPanel(bool sidebar = false) {
        const bool clicked = sidebar
            ? ImGui::Selectable("PERF SCAN", false, 0, {S(155), S(35)})
            : ImGui::Button("PERF SCAN");
        if(clicked) open_perf_scan=true;
        Details("Run a roughly 3-minute performance scan to suggest optimal settings for your hardware and headset. Review the recommendation before applying it.");
    }
    void BenchmarkWindow() {
        if(open_perf_scan) {
            open_perf_scan=false;
            ImGui::OpenPopup("PERF SCAN");
            if(!preview && !benchmark_report && !benchmark_task.valid()) DetectBenchmarkHeadset();
        }
        const auto display=ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowSize({std::min(S(780),display.x-S(40)),std::min(S(620),display.y-S(40))},ImGuiCond_Appearing);
        if(!Modal("PERF SCAN",nullptr,ImGuiWindowFlags_NoSavedSettings)) return;
        Hint("Find recommended graphics settings for your PC and headset. Quick scan takes roughly 3 minutes. Settings change only when you choose Apply.");
        ImGui::Separator();
        if(benchmark_apply_task.valid()) {
            Hint("Verifying hardware and headset before applying settings...");
            if(ImGui::Button("Cancel apply")) benchmark_cancel=true;
        }
        if(headset_task.valid()) {
            ImGui::TextUnformatted("Detecting VR headset...");
            if(ImGui::Button("Cancel detection")) benchmark_cancel=true;
        }
        else if(benchmark_headset) {
            const auto& h=*benchmark_headset;
            ImGui::Text("Headset: %s",h.name.c_str());
            if(h.refresh_hz>0) ImGui::Text("FPS cap (active headset refresh): %.0f FPS",h.refresh_hz);
            else Hint("Headset refresh rate unavailable; using a 90 FPS target.");
            ImGui::Text("Headset-requested resolution per eye: left %u x %u, right %u x %u",h.width[0],h.height[0],h.width[1],h.height[1]);
        } else {
            ImGui::TextUnformatted("No headset detected");
            Hint("Connect your headset and enable its OpenXR runtime, then retry detection.");
            if(!headset_error.empty()) Hint(headset_error.c_str());
        }
        if(!benchmark_task.valid()) {
            ImGui::BeginDisabled(Busy() || running);
            if(ImGui::Button("Detect headset again")) DetectBenchmarkHeadset();
            ImGui::EndDisabled();
        }
        if(running) Hint("Stop the game before detecting the headset or running a scan.");
        const auto package=ExecutableDirectory()/"benchmark";
        const bool available=fs::is_regular_file(package/"manifest.json");
        if(!available) Hint("This build does not include the benchmark save and test runtime.");
        if(!edits.empty()) Hint("Save or discard pending settings before running a scan.");
        if(benchmark_task.valid()) {
            {std::lock_guard lock(benchmark_mutex);ImGui::TextWrapped("%s",benchmark_progress.c_str());}
            const auto elapsed=std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now()-benchmark_log_start).count();
            std::optional<std::chrono::steady_clock::time_point> started;
            {std::lock_guard lock(benchmark_mutex);started=benchmark_run_start;}
            if(!benchmark_thorough) {
                // The adaptive search can finish early. This is time-budget progress,
                // not a fabricated count of completed tests; never show 100% while running.
                const auto scan_seconds=started ? std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now()-*started).count() : 0;
                const int percent=int(std::clamp<long long>(scan_seconds*100/Benchmark::QuickScanBudgetSeconds,0,99));
                const auto label=std::to_string(percent)+"% estimated";
                ImGui::ProgressBar(float(percent)/100,{ -1,S(20)},label.c_str());
                ImGui::Text("Elapsed: %lld:%02lld",static_cast<long long>(elapsed/60),static_cast<long long>(elapsed%60));
                if(benchmark_cancel) Hint("Cancelling and stopping the test game...");
                else if(!started) Hint("Preparing scan. Testing has a 3-minute budget after headset detection (a slower PC may take a little longer to confirm its result).");
                else if(scan_seconds>=Benchmark::QuickScanBudgetSeconds) Hint("Confirming the result or saving it: a slower PC takes a little longer.");
                else {
                    const auto remaining=Benchmark::QuickScanBudgetSeconds-scan_seconds;
                    ImGui::Text("Testing budget remaining: %lld:%02lld (may finish sooner)",
                        static_cast<long long>(remaining/60),static_cast<long long>(remaining%60));
                }
            } else {
                ImGui::Text("Elapsed: %lld:%02lld",static_cast<long long>(elapsed/60),static_cast<long long>(elapsed%60));
                const auto seconds=started ? std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now()-*started).count() : 0;
                const int percent=int(std::clamp<long long>(seconds*100/Benchmark::LongScanBudgetSeconds,0,99));
                const auto label=std::to_string(percent)+"% of 30-minute budget";
                ImGui::ProgressBar(float(percent)/100,{-1,S(20)},label.c_str());
                const auto remaining=std::max<long long>(0,Benchmark::LongScanBudgetSeconds-seconds);
                ImGui::Text("Budget remaining: %lld:%02lld (may finish sooner)",remaining/60,remaining%60);
                Hint("Tests more configurations, not longer samples. Each run ends once enough valid data is collected; the scan can finish well before 30 minutes.");
            }
            Hint("Game controls are disabled during the scan. Startup is automatic.");
            if(ImGui::Button("Cancel scan")) benchmark_cancel=true;
        } else {
            if(benchmark_headset) {
                ImGui::BeginDisabled(Busy());
                ImGui::Checkbox("Use headset refresh rate (90 FPS fallback)",&benchmark_auto_hz);
                if(!benchmark_auto_hz) { ImGui::SetNextItemWidth(S(180)); ImGui::SliderInt("Target FPS",&benchmark_hz,60,144); }
                if(ImGui::CollapsingHeader("Advanced scan options"))
                    ImGui::Checkbox("Thorough scan (30 minutes maximum)",&benchmark_thorough);
                ImGui::EndDisabled();
            }
            if(benchmark_headset) {
                ImGui::BeginDisabled(preview || Busy() || running || !available || !edits.empty());
                if(ImGui::Button(benchmark_report ? "Re-run PERF SCAN" : "RUN PERF SCAN")) Try([&] {
                    RequireStopped();
                    Benchmark::Request request{profile,paths,package,*benchmark_headset,FullSystem(),benchmark_auto_hz?0:benchmark_hz,150};
                    request.quick=!benchmark_thorough;
                    benchmark_cancel=false; benchmark_report.reset();
                    {std::lock_guard lock(benchmark_mutex);benchmark_log.clear();benchmark_run_start.reset();benchmark_log_start=std::chrono::steady_clock::now();}
                    LogBenchmark("Checking headset settings...");
                    benchmark_task=std::async(std::launch::async,[this,request]() mutable {
                        try {
                            const auto current=Benchmark::ScanHeadset(Path(request.profile.runtime),&benchmark_cancel);
                            if(current.name!=request.headset.name || current.width!=request.headset.width || current.height!=request.headset.height ||
                               std::abs(current.refresh_hz-request.headset.refresh_hz)>.5f)
                                throw std::runtime_error("Headset settings changed. Detect the headset again before running the scan.");
                            request.headset=current;
                            {std::lock_guard lock(benchmark_mutex);benchmark_run_start=std::chrono::steady_clock::now();}
                            return Benchmark::Run(request,benchmark_cancel,[this](const std::string& message) {
                                LogBenchmark(message);
                            });
                        } catch(const std::exception& e) { Benchmark::Report r; r.cancelled=benchmark_cancel.load(); if(!r.cancelled) r.error=e.what(); return r; }
                    });
                });
                ImGui::EndDisabled();
            }
        }
        BenchmarkLog();
        BenchmarkResults();
        ImGui::Separator();
        if(ImGui::Button(benchmark_task.valid() ? "Close (scan continues)" : "Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    void BenchmarkResults() {
        if(!benchmark_report)return;
        auto& report=*benchmark_report;
        const auto suggestion=report.recommended ? report.recommended : report.provisional;
        const bool provisional=!report.recommended && report.provisional.has_value();
        if(!report.cancelled && report.error.empty())
            ImGui::ProgressBar(1.0f,{-1,S(20)},"100% - Scan complete");
        if(!report.error.empty())ImGui::TextWrapped("%s",report.error.c_str());
        if(report.cancelled)Hint("Benchmark cancelled. Completed measurements are saved; no settings were applied.");
        ImGui::Text("Measured target: %d FPS",report.target_hz);
        if(suggestion && !report.cancelled && report.error.empty()) {
            const auto& winner=report.results[*suggestion];
            const auto& c=winner.candidate;
            ImGui::TextColored(gold,provisional?"PROVISIONAL SETTINGS - NOT YET CONFIRMED":"SUGGESTED SETTINGS");
            if(provisional) Hint("One usable run met the performance checks. Re-run to confirm repeatability, or try these settings provisionally.");
            ImGui::Text("Stereo: %s | Output scale: %d%% of headset request",c.stereo==StereoPreset::Native?"Native stereo":"DIBR",100+c.graphics[1]);
            ImGui::Text("Upscaler: %s",c.graphics[2]==1?"DLSS":c.graphics[2]==2?"FSR 4":c.graphics[2]==3?"FSR 3.1.5":"Off");
            if(c.graphics[2]) {
                const int preset=c.graphics[c.graphics[2]==1?3:4];
                ImGui::Text("Preset: %s",preset==2?"Quality":preset==1?"Balanced":"Performance");
                if(c.graphics[2]==1) ImGui::Text("DLSS model: %s",c.model.c_str());
            }
            for(const auto& sample:winner.repeats) if(sample.valid && sample.input_width) {
                ImGui::Text("Output per eye: %u x %u",sample.output_width,sample.output_height);
                ImGui::Text("Internal render per eye: %u x %u",sample.input_width,sample.input_height);
                if(c.graphics[2] && sample.output_width && sample.output_height) {
                    ImGui::Text("Internal size: %.1f%% width / %.1f%% height of output",
                        100.0*sample.input_width/sample.output_width,100.0*sample.input_height/sample.output_height);
                    Hint("The game renders at the internal size; the upscaler reconstructs the output sent to VR. Lowering output scale lowers both sizes.");
                } else Hint("Upscaling is off; the game renders directly at the output resolution.");
                Hint("Scale percentages apply to width and height, not total pixels. Actual sizes are aligned by the renderer.");
                break;
            }
            ImGui::Text("Target cap: %d FPS | AA: %s",report.target_hz,c.graphics[2]?"temporal upscaler (FXAA bypassed)":"FXAA");
            if(c.stereo==StereoPreset::Dibr) ImGui::TextUnformatted("DIBR eye TAA: 0.25");
            ImGui::Text("Measured: %.1f FPS | GPU: %.2f ms",winner.fps,winner.gpu_ms);
        }
        if(!suggestion && !report.cancelled && report.error.empty()) {
            ImGui::TextColored(gold,"NO CONFIRMED RECOMMENDATION");
            Hint("Observed FPS is shown below, including excluded runs. Excluded runs cannot justify applying settings.");
            for(const auto& r:report.results) if(!r.reason.empty()) ImGui::TextWrapped("%s: %s",r.candidate.label.c_str(),r.reason.c_str());
        }
        if(suggestion && !report.cancelled && report.error.empty()) {
            const auto& c=report.results[*suggestion].candidate;
            ImGui::BeginDisabled(preview || Busy() || running || !edits.empty());
            if(ImGui::Button(provisional?"Apply provisional settings":"Apply recommendation")) Try([&] {
                RequireStopped();
                const auto hardware=Json::parse(Read(report.folder/"hardware.json"));
                const auto runtime=RuntimePath(), game=Path(profile.game);
                PendingBenchmarkApply pending{c,hardware.at("target_hz").get<int>(),ini,Read(paths.Profile())};
                benchmark_cancel=false;
                benchmark_apply_task=std::async(std::launch::async,[this,runtime,game,hardware] {
                    const auto current_system=ReadSystem();
                    if(FileSha256(runtime)!=hardware.at("runtime_sha256").get<std::string>() ||
                       current_system.gpu!=hardware.at("gpu").get<std::string>() || current_system.driver!=hardware.at("driver").get<std::string>() ||
                       GameSupportInfo(GameExecutable(game))!=hardware.at("game").get<std::string>())
                        throw std::runtime_error("Hardware or runtime changed. Run a fresh benchmark.");
                    for(auto it=hardware.at("upscaler_files").begin();it!=hardware.at("upscaler_files").end();++it)
                        if(FileSha256(runtime.parent_path()/it.key())!=it.value().get<std::string>())
                            throw std::runtime_error("An upscaler library changed. Run a fresh benchmark.");
                    const auto headset=Benchmark::ScanHeadset(runtime,&benchmark_cancel);
                    if(headset.name!=hardware.at("headset").get<std::string>())
                        throw std::runtime_error("Headset changed. Run a fresh benchmark.");
                    if(Json(headset.width)!=hardware.at("eye_width") || Json(headset.height)!=hardware.at("eye_height"))
                        throw std::runtime_error("Headset resolution changed. Run a fresh benchmark.");
                    if(hardware.value("headset_refresh_hz",0.0f)>0 && headset.refresh_hz>0 &&
                       std::abs(hardware.at("headset_refresh_hz").get<float>()-headset.refresh_hz)>.5f)
                        throw std::runtime_error("Headset refresh rate changed. Run a fresh benchmark.");
                    return headset;
                });
                pending_benchmark_apply=std::move(pending);
                status="Verifying hardware and headset before applying the recommendation...";
            });
            ImGui::EndDisabled();
        } else if(!report.cancelled && report.error.empty() && !report.results.empty())
            Hint("No settings were applied. Excluded measurements do not mean your PC missed the FPS target. Review the reasons above before re-running.");
        ImGui::TextUnformatted("FPS REPORT AND TESTED SETTINGS");
        if(ImGui::BeginTable("benchmark_results",5,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Configuration",ImGuiTableColumnFlags_WidthStretch,2);ImGui::TableSetupColumn("Game FPS");ImGui::TableSetupColumn("Frame p99");ImGui::TableSetupColumn("Game GPU memory");ImGui::TableSetupColumn("Result",ImGuiTableColumnFlags_WidthStretch,2);ImGui::TableHeadersRow();
            for(auto& r:report.results) {
                ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextWrapped("%s",r.candidate.label.c_str());
                ImGui::TextWrapped("%s | output %d%% | %s",r.candidate.stereo==StereoPreset::Native?"Native stereo":"DIBR",100+r.candidate.graphics[1],r.candidate.graphics[2]==1?"DLSS":r.candidate.graphics[2]==3?"FSR 3.1.5":r.candidate.graphics[2]==2?"FSR 4":"Upscaler off");
                if(r.candidate.graphics[2]) {
                    const int preset=r.candidate.graphics[r.candidate.graphics[2]==1?3:4];
                    ImGui::Text("%s",preset==2?"Quality":preset==1?"Balanced":"Performance");
                    if(r.candidate.graphics[2]==1) ImGui::Text("Model %s",r.candidate.model.c_str());
                }
                for(const auto& sample:r.repeats)if(sample.input_width) {
                    ImGui::TextWrapped("Internal: %u x %u",sample.input_width,sample.input_height);
                    ImGui::TextWrapped("Output: %u x %u per eye",sample.output_width,sample.output_height);break;
                }
                ImGui::TableNextColumn();
                if(r.reliable) ImGui::Text("%.1f confirmed",r.fps);
                for(size_t i=0;i<r.repeats.size();++i) {
                    const auto& sample=r.repeats[i];
                    if(sample.fps>0) ImGui::TextWrapped("Run %u: %.1f%s",unsigned(i+1),sample.fps,sample.valid?"":" (excluded)");
                    else ImGui::Text("Run %u: no data",unsigned(i+1));
                }
                ImGui::TableNextColumn();
                double pacing=0;for(const auto& sample:r.repeats)if(sample.frame_p99>0)pacing=std::max(pacing,sample.frame_p99);
                if(pacing>0)ImGui::Text("%.1f ms",pacing);else ImGui::TextUnformatted("--");
                ImGui::TableNextColumn();
                double memory=0;for(const auto& sample:r.repeats)if(sample.peak_device_heap_mib>0)memory=std::max(memory,sample.peak_device_heap_mib);
                if(memory>0)ImGui::Text("%.0f MiB",memory);else ImGui::TextUnformatted("--");
                ImGui::TableNextColumn();ImGui::TextWrapped("%s",r.provisional ? r.reason.c_str() : r.meets_target ? "Target + headroom" : r.reason.c_str());
                for(const auto& sample:r.repeats) if(!sample.warning.empty()) ImGui::TextWrapped("Warning: %s",sample.warning.c_str());
            }
            ImGui::EndTable();
        }
        if(report.time_limited) Hint("Scan budget reached. Single-run suggestions are provisional until confirmed.");
        Hint("Quick scans compare a limited set of settings; the recommendation is the best confirmed option tested.");
        Hint("Rendering estimates at the scanned resolution, using a fixed test field of view. "
             "GPU memory estimates this process's main device-local heap usage, not whole-GPU usage; brief peaks may be missed. This does not measure headset latency or judge shimmer, ghosting, or DIBR artifacts.");
        if(!report.folder.empty() && ImGui::Button("Open benchmark report"))OpenPath(report.folder);
    }
    void GraphicsTab() {
        BenchmarkPanel();
        Row("Game frame-rate cap", "Applies on the next launch. Manual edits save when released.",
            "Follow headset reads its active refresh rate at launch and keeps that display rate; "
            "if detection is unavailable, gameplay uses 90 FPS. Manual mode caps gameplay and "
            "requests a matching headset rate where supported. Applying PERF SCAN pins its measured target.", [&] {
                bool follow=profile.follow_headset;
                if(ImGui::Checkbox("Follow headset",&follow)) Try([&] {
                    RequireStopped();
                    auto updated=profile; updated.follow_headset=follow;
                    updated.Save(paths); profile=std::move(updated);
                    frame_rate_editing=false;
                    status=follow ? "Game frame rate will follow the headset on the next launch."
                                  : "Manual frame-rate mode saved.";
                });
                if(profile.follow_headset) {
                    Hint("Use the active headset rate; 90 FPS if unavailable. Checked at each launch.");
                    return;
                }
                if(!frame_rate_editing) frame_rate_draft=profile.refresh_hz;
                ImGui::SetNextItemWidth(S(180));
                ImGui::SliderInt("##game-fps", &frame_rate_draft, MinGameRefreshHz, MaxGameRefreshHz,
                                 "%d FPS", ImGuiSliderFlags_AlwaysClamp);
                const bool commit=ImGui::IsItemDeactivatedAfterEdit();
                frame_rate_editing=ImGui::IsItemActive();
                ImGui::SameLine();
                const bool reset=ImGui::Button("Reset to 90 FPS");
                const int chosen=reset ? 90 : frame_rate_draft;
                if((commit || reset) && chosen!=profile.refresh_hz) Try([&] {
                    RequireStopped();
                    auto updated=profile; updated.refresh_hz=chosen;
                    updated.Save(paths); profile=std::move(updated);
                    status="Game frame-rate cap saved. Applies on the next launch.";
                });
            });

        Row("Stereo rendering", "Applies on the next launch; saved automatically.",
            "Auto selects DIBR on GPUs with 8 GB of dedicated memory or less, or whose driver "
            "lacks the multiview support native stereo needs. DIBR renders "
            "one view and reconstructs both eyes to reduce GPU work and memory use, but can "
            "show artifacts around object edges. Native stereo renders both eyes separately.",
            [&] {
                const char *labels[] = {"Auto (based on GPU memory)",
                                        "DIBR (lower memory)", "Native stereo"};
                const int selected = static_cast<int>(profile.stereo);
                if (ImGui::BeginCombo("##stereo", labels[selected])) {
                    for (int i = 0; i < 3; ++i) {
                        if (ImGui::Selectable(labels[i], selected == i)) {
                            Try([&] {
                                auto updated = profile;
                                updated.stereo = static_cast<StereoChoice>(i);
                                updated.Save(paths);
                                profile.stereo = updated.stereo;
                                status = "Stereo rendering saved. Applies on the next launch.";
                            });
                        }
                    }
                    ImGui::EndCombo();
                }
            });
        const bool dibr = profile.SelectedStereo(system) == StereoPreset::Dibr;
        if (dibr)
            Hint("DIBR is selected. Scaling sets the center render resolution. DLSS / FSR "
                 "require native stereo. Your saved upscaler settings are kept.");
        auto f = PendingGraphics();
        const bool dlss_ok = Available().dlss;
        const bool fsr4_ok = Available().fsr4;
        Row("Scaling", "Relative to headset resolution.",
            "Each eye's resolution relative to the size your headset asks for: higher is sharper "
            "and slower.",
            [&] {
                if (StepSlider(f[1], -50, 50, 10, Signed(f[1], " %"))) {
                    SetGraphics(f);
                }
            });
        ImGui::BeginDisabled(dibr);
        const char *no_dlss = !system.nvidia ? "DLSS needs an NVIDIA RTX graphics card."
                                             : "Needs the DLSS files, which this package does not "
                                               "include.";
        Row("Upscaler", "Choose one reconstruction method, or Off for direct rendering.",
            "DLSS requires NVIDIA RTX. FSR 3.1.5 and our FSR 4 implementation support compatible "
            "AMD and NVIDIA GPUs. Performance depends on your hardware and resolution; newer "
            "does not always mean faster. Selecting an option replaces the active upscaler.",
            [&] {
                const bool neural = f[2] == 1 && f[5];
                const char* selected = neural ? "DLSS 5 (separate tab)" :
                    f[2] == 1 ? "DLSS" : f[2] == 2 ? "FSR 4" : f[2] == 3 ? "FSR 3.1.5" : "Off";
                const int pick = Combo(selected,
                    {{"Off", f[2] == 0, true, ""},
                     {"DLSS", f[2] == 1 && !neural, dlss_ok, no_dlss},
                     {"FSR 3.1.5", f[2] == 3, Available().fsr31, "FSR 3.1.5 runtime files are missing."},
                     {"FSR 4", f[2] == 2, fsr4_ok, "FSR 4 runtime files are missing."}});
                if (pick >= 0) {
                    constexpr int providers[]{0, 1, 3, 2};
                    f[2] = providers[pick];
                    f[5] = 0;
                    SetGraphics(f);
                }
            });
        const bool dlss = f[2] == 1 && !f[5], fsr4 = f[2] == 2, fsr31 = f[2] == 3;
        Row("Quality preset", "Internal render size relative to output. Requires an upscaler.",
            "Quality renders at about 67% of output width and height, Balanced at 58%, "
            "and Performance at 50%. DLSS remembers its preset; both FSR versions share theirs.",
            [&] {
                const char *names[]{"Performance", "Balanced", "Quality"};
                const int current = (fsr4 || fsr31) ? f[4] : f[3];
                ImGui::BeginDisabled(!dlss && !fsr4 && !fsr31);
                const int pick = Combo(names[current], {{names[0], current == 0, true, ""},
                                                        {names[1], current == 1, true, ""},
                                                        {names[2], current == 2, true, ""}});
                ImGui::EndDisabled();
                if (pick >= 0) {
                    f[(fsr4 || fsr31) ? 4 : 3] = pick;
                    SetGraphics(f);
                }
            });
        ImGui::EndDisabled();
    }
    // The in-game DLSS 5 tab: on graphics_config, like the page's draft.
    void Dlss5Tab() {
        const bool dibr = profile.SelectedStereo(system) == StereoPreset::Dibr;
        if (dibr)
            Hint("DLSS 5 requires native stereo and is unavailable in DIBR. Saved settings kept.");
        ImGui::BeginDisabled(dibr);
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
        const bool locked = row.keys[0] == "aa_mode" && PendingGraphics()[2] != 0 &&
                            profile.SelectedStereo(system) == StereoPreset::Native;
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
        const bool aa_locked = PendingGraphics()[2] != 0 &&
                               profile.SelectedStereo(system) == StereoPreset::Native;
        for (const auto &row : Settings())
            if (name == row.category && !(aa_locked && row.keys[0] == "aa_mode"))
                for (const auto &key : row.keys)
                    Edit(key, std::nullopt);
        auto f = PendingGraphics();
        if (name == "Graphics") {
            // Reset output scale; preserve the disabled native upscaler rows in DIBR.
            f[1] = 0;
            if (profile.SelectedStereo(system) == StereoPreset::Native) {
                if ((f[2] == 1 && !f[5]) || f[2] == 2 || f[2] == 3)
                    f[2] = 0;
                f[3] = f[4] = 1;
            }
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
        if (ImGui::Checkbox("Check for updates at startup", &profile.check_updates)) {
            profile.check_updates_chosen = true; // the player's own choice from now on
            Try([&] { profile.Save(paths); });
        }
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
        // Ready to install: gold and larger, the one thing to press (2026-10-18).
        const bool installable = !Busy() && downloaded && !running;
        ImGui::BeginDisabled(!installable);
        if (installable) {
            ImGui::PushStyleColor(ImGuiCol_Button, gold);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.80f, 0.70f, 0.48f, 1});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.57f, 0.49f, 0.33f, 1});
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.06f, 0.05f, 0.04f, 1});
        }
        if (ImGui::Button(installable ? "INSTALL & RESTART" : "Install & restart",
                          installable ? ImVec2{S(220), S(40)} : ImVec2{0, 0}))
            Try([&] {
                RequireStopped();
                if (!edits.empty())
                    SaveIni();
                SaveProfile();
                updater->WaitExitThenApplyUpdates(*available);
                quit = true;
            });
        if (installable)
            ImGui::PopStyleColor(4);
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
        if (ImGui::Button("View releases"))
            Try([&] { OpenPath(L"https://github.com/bldbrne-vr/bldbrne-vr/releases"); });
    }
    void Draw() {
        auto size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("Bldbrne-VR", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.035f, 0.038f, 0.04f, 1});
        ImGui::BeginChild("sidebar", {S(187), 0});
        ImGui::SetCursorPos({S(22), S(38)});
        ImGui::PushFont(brand_font);
        ImGui::TextColored(gold, "Bldbrne-VR");
        ImGui::PopFont();
        ImGui::SetCursorPosY(S(140));
        const char *pages[]{"Play", edits.empty() ? "VR settings" : "VR settings *",
                            available ? "Updates (NEW)" : "Updates"};
        for (int i = 0; i < 3; ++i) {
            ImGui::SetCursorPosX(S(16));
            if (ImGui::Selectable(pages[i], page == i, 0, {S(155), S(35)}))
                page = i;
        }
        ImGui::SetCursorPosX(S(16));
        BenchmarkPanel(true);
        ImGui::SetCursorPos({S(22), size.y - S(185)});
        if (ImGui::SmallButton("Save backups"))
            show_backups = true;
        ImGui::SetCursorPosX(S(22));
        if (ImGui::SmallButton("Desktop shortcut"))
            Try([&] {
                CreatePlayShortcut();
                status = "\"Play Bldbrne-VR\" is on your desktop. It starts the game with your saved "
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
        BenchmarkWindow();
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
                const Velopack::UpdateOptions options{false, UpdateChannel, -1};
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
#if BBVR_BRIDGE
        if (argc == 3 && std::wstring_view(args[1]) == L"--bridge-smoke") {
            // The Install the new version button without the window (release tests).
            auto marker = fs::absolute(args[2]);
            LocalFree(args);
            try {
                InstallNewVersion();
                if (InstalledCopy())
                    UninstallThisCopy();
                Write(marker, "moved");
                return 0;
            } catch (const std::exception &ex) {
                Write(fs::path(marker.native() + L".error"), ex.what());
                return 1;
            }
        }
#endif
        if (argc == 2 && std::wstring_view(args[1]) == L"--play") {
            // The desktop shortcut: start the game with the saved launcher settings, no window.
            LocalFree(args);
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            const HANDLE starting = CreateMutexW(nullptr, FALSE, L"Local\\" BBVR_LOCK L".Play");
            WaitForSingleObject(starting, 30000);
            struct Release {
                HANDLE h;
                ~Release() {
                    ReleaseMutex(h);
                    CloseHandle(h);
                }
            } release{starting};
            try {
                HANDLE benchmark=OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\BldbrneVRBenchmark");
                if(benchmark) {CloseHandle(benchmark);throw std::runtime_error("Cancel or finish the benchmark first.");}
                if (RuntimeRunning())
                    throw std::runtime_error("Bldbrne-VR is already running.");
                MigrateLegacyData();
                const auto paths = Paths::Default();
                auto launch_profile=Profile::Load(paths);
                if(launch_profile.follow_headset) {
                    float refresh=0;
                    try { refresh=Benchmark::ScanHeadset(Path(launch_profile.runtime)).refresh_hz; }
                    catch(const std::exception&) { /* Same 90 FPS fallback as the launcher UI. */ }
                    launch_profile.refresh_hz=GameRefreshRate(true,launch_profile.refresh_hz,refresh);
                    if(RuntimeRunning()) throw std::runtime_error("Bldbrne-VR is already running.");
                }
                CloseHandle(Launch(Prepare(launch_profile, paths)));
            } catch (const std::exception &ex) {
                MessageBoxW(nullptr,
                            (Wide(ex.what()) + L"\n\nOpen Bldbrne-VR from the Start menu to fix this.")
                                .c_str(),
                            L"Bldbrne-VR", MB_OK | MB_ICONWARNING);
                return 1;
            }
            return 0;
        }
        bool preview = argc == 3 && std::wstring_view(args[1]) == L"--preview";
        bool idle_smoke = argc == 3 && std::wstring_view(args[1]) == L"--idle-smoke";
        fs::path preview_dir = (preview || idle_smoke) ? fs::absolute(args[2]) : fs::path{};
        LocalFree(args);
        HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\" BBVR_LOCK L".Native");
        if (GetLastError() == ERROR_ALREADY_EXISTS && !preview) {
            if (HWND existing = FindWindowW(BBVR_LOCK L".Native", nullptr)) {
                ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            CloseHandle(singleton);
            return 0;
        }
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (!preview) {
            // A "Play Bldbrne-VR" shortcut made by a launcher before the rename points at
            // BloodborneVR.exe, which an update removes: point it at this one.
            try {
                RepairPlayShortcut();
            } catch (const std::exception &) {
            }
        }
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
                       BBVR_LOCK L".Native",
                       nullptr};
        wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON),
                                               IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                               GetSystemMetrics(SM_CYICON), LR_SHARED));
        wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON),
                                                 IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                 GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        RegisterClassExW(&wc);
        RECT r{0, 0, LONG(S(980)), LONG(S(740))};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        window_handle = CreateWindowW(wc.lpszClassName, L"Bldbrne-VR", WS_OVERLAPPEDWINDOW,
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
        if (!preview && !idle_smoke) {
            // The data folder of a launcher before 0.2.7 (BloodborneVRData), moved once.
            try {
                MigrateLegacyData();
            } catch (const std::exception &) {
            }
        }
        App app((preview || idle_smoke) ? Paths{preview_dir / "data"} : Paths::Default(),
                preview || idle_smoke);
        if (preview)
            app.preview_update = "0.3.3"; // shows the Play page's update button
        if (preview)
            app.downloaded = true; // ... and Updates' install button as it looks when ready
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
                                    "graphics.png", "benchmark-results.png", "perf-scan-no-headset.png", "perf-scan-ready.png", "crash.png", "backups.png"};
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
                case 4: {
                    const auto fixture=preview_dir/"benchmark-results.json";
                    if(fs::exists(fixture)) {
                        Benchmark::Report report;
                        for(const auto& item:Json::parse(Read(fixture))) {
                            Benchmark::Result result;result.candidate.label=item.at("label");
                            result.candidate.id=item.at("id");result.candidate.quality=item.at("quality_rank");
                            result.target_hz=item.value("target_hz",90);report.target_hz=result.target_hz;
                            result.candidate.graphics=ParseGraphics(item.at("graphics").get<std::string>());
                            result.candidate.stereo=item.at("stereo")=="native"?StereoPreset::Native:StereoPreset::Dibr;
                            result.gpu_ms=item.at("gpu_ms");
                            result.candidate.model=item.value("dlss_model",std::string("builtin"));
                            result.fps=item.at("fps");result.reliable=item.at("reliable");
                            result.provisional=item.value("provisional",false);
                            result.meets_target=item.at("meets_target");result.near_target=item.value("near_target",false);result.reason=item.at("reason");
                            for(const auto& entry:item.at("repeats")) {
                                Benchmark::Sample sample;sample.valid=entry.at("valid");sample.fps=entry.value("fps",0.0);
                                sample.frame_p99=entry.at("frame_p99");
                                sample.warning=entry.value("warning",std::string{});
                                sample.input_width=entry.value("input_width",0u);sample.input_height=entry.value("input_height",0u);
                                sample.output_width=entry.value("output_width",0u);sample.output_height=entry.value("output_height",0u);
                                sample.peak_device_heap_mib=entry.at("peak_logged_device_heap_mib");
                                result.repeats.push_back(sample);
                            }
                            report.results.push_back(result);
                        }
                        report.recommended=Benchmark::Recommend(report.results);
                        if(!report.recommended) report.provisional=Benchmark::Recommend(report.results,true);
                        app.benchmark_report=std::move(report);
                        app.benchmark_headset=Benchmark::Headset{"Quest 3 (preview)",{1840,1840},{2016,2016},3680,4032,90};
                        app.open_perf_scan=true;
                    }
                    break;
                }
                case 5:
                    app.benchmark_report.reset();
                    app.benchmark_headset.reset();
                    break;
                case 6:
                    app.benchmark_headset=Benchmark::Headset{"Quest 3 (preview)",{1840,1840},{2016,2016},3680,4032,90};
                    break;
                case 7:
                    app.page = 0;
                    app.crash = App::Crash{0xC0000005, ExitReason(0xC0000005),
                                           {"[Debug] <Critical> signals.cpp:175 Unhandled "
                                            "Exception code 0xC0000005 (preview)"}};
                    app.show_crash = true;
                    break;
                case 8:
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
        MessageBoxW(nullptr, Wide(ex.what()).c_str(), L"Bldbrne-VR", MB_OK | MB_ICONERROR);
        return 1;
    }
}
