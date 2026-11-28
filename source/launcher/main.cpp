// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core.h"
#include "mods.h"
#include "benchmark.h"
#include "controls_guide.h"
#include "controller_diagram.h"
#include "hunter_theme.h"
#include "release_version.h"
#include "sky_upscale.h"
#include "core/vr/card_memory.h"
#include "core/vr/card_tier.h"
#include "core/vr/play_style.h"
#include "core/vr/graphics_resolution.h"
#include "core/vr/headset_panels.h"
#include "core/vr/mirror_world.h"
#include <mutex>
#include "resource.h"
#include <Velopack.hpp>
#include <atomic>
#include <cmath>
#include <d3d11.h>
#include <dwmapi.h>
#include <future>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_stdlib.h>
#include <memory>
#include <psapi.h>
#include <regex>
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
static fs::path dropped_mod;
static bool invalid_mod_drop{};
static int repaint = 3;
static bool close_requested{};
static UINT resize_width{}, resize_height{};
static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> context;
static ComPtr<IDXGISwapChain> swapchain;
static ComPtr<ID3D11RenderTargetView> target;
static ImFont *heading_font{};
static ImFont *brand_font{};
static ImFont *logo_font{};
static ImFont *display_font{}, *italic_font{}, *launch_font{};
static ImFont *sidebar_nav_font{}, *sidebar_tag_font{};
static ImVec4 gold{0.70f, 0.65f, 0.54f, 1}, muted{0.62f, 0.63f, 0.62f, 1};
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
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wp);
        wchar_t name[32768]{};
        invalid_mod_drop = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) != 1 ||
                           !DragQueryFileW(drop, 0, name, DWORD(std::size(name)));
        if (!invalid_mod_drop)
            dropped_mod = name;
        else dropped_mod.clear();
        DragFinish(drop);
        return 0;
    }
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
    s.ScrollbarSize = 16.f + HunterTheme::ScrollbarGutter;
    // The launcher paints these after the native scrollbar has handled input.
    s.Colors[ImGuiCol_ScrollbarBg] = s.Colors[ImGuiCol_ScrollbarGrab] =
        s.Colors[ImGuiCol_ScrollbarGrabHovered] = s.Colors[ImGuiCol_ScrollbarGrabActive] =
        {0, 0, 0, 0};
    s.FrameBorderSize = 1;
    s.Colors[ImGuiCol_WindowBg] = s.Colors[ImGuiCol_ChildBg] = {0.033f, 0.040f, 0.046f, 1};
    s.Colors[ImGuiCol_Text] = {0.88f, 0.84f, 0.77f, 1};
    s.Colors[ImGuiCol_TextDisabled] = muted;
    s.Colors[ImGuiCol_Border] = s.Colors[ImGuiCol_Separator] = {0.26f, 0.26f, 0.24f, 1};
    s.Colors[ImGuiCol_FrameBg] = {0.09f, 0.09f, 0.09f, 1};
    s.Colors[ImGuiCol_FrameBgHovered] = {0.18f, 0.21f, 0.23f, 1};
    s.Colors[ImGuiCol_FrameBgActive] = {0.12f, 0.15f, 0.17f, 1};
    s.Colors[ImGuiCol_Button] = {0.075f, 0.085f, 0.095f, 1};
    s.Colors[ImGuiCol_ButtonHovered] = {0.18f, 0.20f, 0.21f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {0.09f, 0.11f, 0.12f, 1};
    s.Colors[ImGuiCol_Header] = {0.09f, 0.105f, 0.115f, 1};
    s.Colors[ImGuiCol_HeaderHovered] = {0.18f, 0.20f, 0.21f, 1};
    s.Colors[ImGuiCol_HeaderActive] = {0.25f, 0.09f, 0.09f, 1};
    // The Controls page's controller tabs as the settings' tab buttons (not ImGui's blue).
    s.Colors[ImGuiCol_Tab] = s.Colors[ImGuiCol_TabDimmed] = s.Colors[ImGuiCol_Button];
    s.Colors[ImGuiCol_TabHovered] = s.Colors[ImGuiCol_ButtonHovered];
    s.Colors[ImGuiCol_TabSelected] = s.Colors[ImGuiCol_TabDimmedSelected] = HunterTheme::Crimson;
    s.Colors[ImGuiCol_TabSelectedOverline] = s.Colors[ImGuiCol_TabDimmedSelectedOverline] = gold;
    s.Colors[ImGuiCol_CheckMark] = s.Colors[ImGuiCol_SliderGrab] = gold;
    s.Colors[ImGuiCol_TitleBg] = s.Colors[ImGuiCol_TitleBgActive] = {0.20f, 0.065f, 0.065f, 1};
    s.Colors[ImGuiCol_PopupBg] = {0.07f, 0.072f, 0.075f, 1};
    s.Colors[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, 0.55f};
    s.Colors[ImGuiCol_NavCursor] = {0.85f, 0.78f, 0.61f, 1};
    s.Colors[ImGuiCol_SliderGrabActive] = {.86f,.79f,.64f,1};
    s.Colors[ImGuiCol_TextSelectedBg] = {.40f,.34f,.25f,.65f};
    s.Colors[ImGuiCol_TableHeaderBg] = {.11f,.12f,.13f,1};
    s.Colors[ImGuiCol_TableBorderStrong] = {.28f,.28f,.26f,1};
    s.Colors[ImGuiCol_TableBorderLight] = {.17f,.18f,.18f,1};
    s.Colors[ImGuiCol_TableRowBgAlt] = {.55f,.58f,.60f,.045f};
    s.Colors[ImGuiCol_PlotHistogram] = {.44f,.19f,.17f,1};
    s.ScaleAllSizes(scale);
}
static void Hint(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, muted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}
// The main window has no padding; dialogs and their lists need some.
static bool ThemedModal(const char *name, bool *open, ImGuiWindowFlags flags,
                        const HunterTheme::Art& art) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(20), S(16)});
    ImGui::PushFont(brand_font);
    const bool shown = ImGui::BeginPopupModal(name, open, flags);
    ImGui::PopFont();
    if (shown) {
        const auto* window = ImGui::GetCurrentWindow();
        const auto p = window->Pos, size = window->Size;
        auto* d = window->DrawList;
        const float body_top = p.y + window->TitleBarHeight;
        // Draw the body and frame to the window edges, outside the content clip.
        d->PushClipRect(window->OuterRectClipped.Min, window->OuterRectClipped.Max, false);
        HunterTheme::Leather(d,{p.x+S(1),body_top},{p.x+size.x-S(1),p.y+size.y-S(1)},
                             art,S(1),IM_COL32(147,144,140,255));
        HunterTheme::MetalFrame(d,{p.x+S(1),p.y+S(1)},{p.x+size.x-S(1),p.y+size.y-S(1)},
                                art.frame,S(1),{23,23},{.14f,.14f});
        d->PopClipRect();
    }
    ImGui::PopStyleVar();
    return shown;
}
static void Title(const char *eyebrow, const char *title) {
    if (eyebrow) {
        ImGui::TextColored(gold, "%s", eyebrow);
        ImGui::Dummy({0, S(3)});
    }
    ImGui::PushFont(heading_font);
    const auto pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText({pos.x+S(1),pos.y+S(2)},IM_COL32(0,0,0,220),title);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
}

static void InsetList(const char* id, float height, const HunterTheme::Art& art,
                      ImGuiWindowFlags flags = 0) {
    const auto pos = ImGui::GetCursorScreenPos();
    const auto available = ImGui::GetContentRegionAvail();
    height = height < 0 ? std::max(S(18),available.y+height) : height;
    HunterTheme::InsetPanel(ImGui::GetWindowDrawList(),pos,
                           {pos.x+available.x,pos.y+height},art,S(1));
    // Keep the scrolling viewport and scrollbar inside the metal rim.
    const float rim = S(5);
    ImGui::SetCursorScreenPos({pos.x+rim,pos.y+rim});
    ImGui::PushStyleColor(ImGuiCol_ChildBg,{0,0,0,0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{S(9),S(4)});
    ImGui::BeginChild(id,{available.x-2*rim,height-2*rim},ImGuiChildFlags_AlwaysUseWindowPadding,flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

static void EndInsetList() {
    ImGui::EndChild();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY()+S(5));
}
struct UpdateResult {
    std::optional<Velopack::UpdateInfo> release;
    std::string message, error;
    bool downloaded{};
};
struct App {
    HunterTheme::Art theme_art;
    bool Button(const char* label, ImVec2 size = {},
                HunterTheme::ButtonFace face = HunterTheme::ButtonFace::Standard,
                bool selected = false) {
        return HunterTheme::Button(label,size,theme_art,S(1),face,selected);
    }
    bool SmallButton(const char* label) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            {std::max(ImGui::GetStyle().FramePadding.x,S(10)),S(3)});
        const bool clicked = Button(label,{},HunterTheme::ButtonFace::Utility);
        ImGui::PopStyleVar();
        return clicked;
    }
    bool UpdateButton(const char* label, ImVec2 size = {}) {
        HunterTheme::PushUpdateAccent();
        const bool clicked = Button(label, size, HunterTheme::ButtonFace::Gilt);
        ImGui::PopStyleColor(4);
        return clicked;
    }
    bool Header(const char* label) {
        return HunterTheme::Header(label,theme_art,S(1));
    }
    bool Checkbox(const char* label, bool* checked) {
        return HunterTheme::Checkbox(label,checked,theme_art,S(1));
    }
    static bool TextField(const char* id, std::string& value, const char* hint = nullptr) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, {.055f,.054f,.052f,1.f});
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        const bool changed = ImGui::InputTextWithHint(id, hint, &value);
        ImGui::PopStyleColor(2);

        // Draw the border after InputText has resolved hover and editing focus.
        const auto& style = ImGui::GetStyle();
        if (ImGui::IsItemVisible() && style.FrameBorderSize > 0.f) {
            auto* draw = ImGui::GetWindowDrawList();
            const auto min = ImGui::GetItemRectMin();
            const auto max = ImGui::GetItemRectMax();
            ImVec4 border{.34f, .31f, .26f, 1.f};
            if (ImGui::IsItemActive()) {
                border = {.72f, .62f, .46f, 1.f};
            } else if (ImGui::IsItemHovered()) {
                border = {.66f, .55f, .40f, 1.f};
                const float spread = S(1.5f);
                for (int ring = 2; ring >= 1; --ring) {
                    const float outset = ring * spread;
                    const auto shadow = ImGui::GetColorU32(ImVec4{.46f, .30f, .17f, .30f / ring});
                    draw->AddRect({min.x - outset, min.y - outset},
                                  {max.x + outset, max.y + outset}, shadow,
                                  style.FrameRounding + outset, 0, spread);
                }
            }
            draw->AddRect(min, max, ImGui::GetColorU32(border), style.FrameRounding,
                          0, style.FrameBorderSize);
        }
        return changed;
    }
    bool Modal(const char *name, bool *open, ImGuiWindowFlags flags) {
        return ThemedModal(name,open,flags,theme_art);
    }
    Paths paths;
    Profile profile;
    std::string ini;
    std::map<std::string, std::optional<std::string>> edits;
    std::string close_edit_summary;
    int page{}, category{};
    std::optional<HeadsetRequest> headset_request;
    double headset_request_read{-1};
    std::string search, error;
    std::string status = "Ready when you are.";
    bool running{}, quit{}, preview{};
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
    fs::path mod_game;
    std::vector<Mods::Package> mods;
    std::optional<Mods::Package> mod_selection;
    bool confirm_mod_removal{};
    Mods::Report mod_report;
    struct ModResult {
        std::optional<Mods::Package> selection;
        Mods::Report report;
        std::string note;
    };
    std::future<ModResult> mod_task;
    bool mod_refresh = true;
    bool mod_list_ok = false;
    size_t mod_file_page = 0;
    std::string mod_note;
    int frame_rate_draft=90;
    bool frame_rate_editing=false;
    struct PendingBenchmarkApply {
        Benchmark::Candidate candidate;
        int target_hz;
        std::string expected_ini, expected_profile;
        fs::path folder; // the scan report's
        Profile before;
    };
    std::optional<PendingBenchmarkApply> pending_benchmark_apply;
    // The scan suggestion applied last (its report and candidate) and the settings before it: the
    // scan window lists what Apply changed while the saved settings still hold it.
    struct AppliedScan {
        fs::path folder;
        std::string id;
        Profile before;
        std::string before_ini;
    };
    std::optional<AppliedScan> benchmark_applied;
    std::future<Benchmark::Headset> benchmark_apply_task;
    bool close_after_benchmark_apply{};
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
    bool game_folder_needed{true};
    bool reveal_play_setup{};
    std::string dlc_game, dlc_path;
    DlcStatus dlc_status;
    bool dlc_checked = false, dlc_game_found = false;
    std::string update_status = "Check for a new version.";
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
        bool fsr41{}; // AMD cards only, as DLSS is NVIDIA's
    } upscalers{"\x01"};
    // The player's DLSS 5 file (user/dlss5; core/vr/dlss5_file.h): checked, found or copied off the
    // UI thread. dlss5_added is empty while that runs.
    struct Dlss5Result {
        VR::Dlss5File::Check added;
        std::string note;
    };
    std::future<Dlss5Result> dlss5_task;
    std::optional<VR::Dlss5File::Check> dlss5_added;
    std::string dlss5_note;
    // Sharper skies and sharper weapons (DetailRows): made off the UI thread, one at a time (both
    // use the graphics card); `state` is what the folder holds.
    struct DetailJob {
        Skies::Kind kind;
        std::future<std::string> task;
        Skies::Progress progress;
        Skies::State state;
        std::string note;
    };
    DetailJob sky{Skies::Kind::Skies}, weapons{Skies::Kind::Weapons};
    template <class Work> void Dlss5Work(Work work) {
        if (dlss5_task.valid())
            return;
        dlss5_added.reset();
        dlss5_task = std::async(std::launch::async, [user = paths.User(), work] {
            Dlss5Result r;
            try {
                r.note = work();
            } catch (const std::exception &e) {
                r.note = e.what();
            }
            r.added = VR::Dlss5File::Verify(VR::Dlss5File::InUser(user));
            return r;
        });
    }
    const Upscalers &Available() {
        if (upscalers.runtime != profile.runtime) {
            const auto runtime = RuntimePath();
            upscalers = {profile.runtime, system.nvidia && DlssIncluded(runtime),
                         Fsr4Included(runtime), system.nvidia && Dlss5Included(runtime), Fsr31Included(runtime),
                         system.amd && Fsr41Included(runtime)};
        }
        return upscalers;
    }
    App(Paths p, bool pv) : paths(std::move(p)), preview(pv) {
        fs::create_directories(paths.User());
        std::error_code error;
        const bool new_player = !fs::exists(paths.Profile(), error) && !error;
        profile = Profile::Load(paths);
        Reload();
        // The Vulkan multiview probe (0.1 s or more with overlay layers) runs off the UI thread;
        // until it reports, the window shows the DXGI facts.
        system = ReadSystem(false);
        system_task = std::async(std::launch::async, [] { return ReadSystem(); });
        if (new_player && !preview)
            StartNewPlayerGraphics();
        xr_runtime = OpenXrRuntime();
        Dlss5Work([] { return std::string(); });
        sky.state = Skies::Look(paths.User(), sky.kind);
        weapons.state = Skies::Look(paths.User(), weapons.kind);
        // The Controls page (and the settings' button names) open on the controllers of the
        // headset the game last ran on.
        if (const auto headset = LastHeadsetRequest(paths))
            controls_select = controls_platform =
                static_cast<int>(ControlsGuide::PlatformOf(headset->system));
        shaders_ready = ShaderCacheReady(paths);
        if (!shaders_ready)
            status = FirstLaunchNote;
    }
    // A new player (no launcher profile yet) starts with the upscaler their card runs well
    // (StartupUpscaler), on a fast card with Light foveation (StartupFoveation) and with 8 GB or less on
    // Reduced far shadows (StartupReducedShadows); a player with a
    // profile keeps their own settings, and a choice already in vr.ini (the game's VR page)
    // stays. The profile is written at once, so a later reset of the graphics row is never undone
    // at the next start.
    void StartNewPlayerGraphics() {
        if (VR::Ini::ValueOf(ini, "graphics_config") || RuntimeRunning())
            return;
        auto fields = StartupGraphics(system);
        const auto &have = Available();
        if ((fields[2] == 1 && !have.dlss) || (fields[2] == 3 && !have.fsr31) ||
            (fields[2] == 4 && !have.fsr41))
            return; // the build lacks that upscaler: the game's default (off)
        try {
            profile.Save(paths);
            std::vector<VR::Ini::Edit> edits;
            if (fields != DefaultGraphics)
                edits.push_back({"graphics_config", GraphicsText(fields)});
            const bool light =
                StartupFoveation(system) == 3 && !VR::Ini::ValueOf(ini, "foveation");
            if (light)
                edits.push_back({"foveation", "3"});
            const bool shadows_reduced =
                StartupReducedShadows(system) && !VR::Ini::ValueOf(ini, "shadows_far");
            if (shadows_reduced)
                edits.push_back({"shadows_far", "reduced"});
            if (edits.empty())
                return;
            Write(paths.Ini(), VR::Ini::ApplyEdits(ini, edits), ini);
            Reload();
            if (fields != DefaultGraphics)
                new_player_upscaler = std::string(fields[2] == 1 ? "DLSS " : "FSR 3.1.5 ") +
                                      PresetName(fields[2] == 1 ? fields[3] : fields[4]) +
                                      (light ? " with Light foveation" : "");
            new_player_shadows_reduced = shadows_reduced;
        } catch (const std::exception &) {
            // Unwritable: the game's defaults, as before.
        }
    }
    // What StartNewPlayerGraphics chose ("DLSS Quality with Light foveation"), shown on the Play
    // page; empty otherwise.
    std::string new_player_upscaler;
    bool new_player_shadows_reduced{false};
    static constexpr const char *FirstLaunchNote =
        "First launch may stutter while shaders build.";
    ~App() {
        benchmark_cancel=true;
        for (DetailJob *job : {&sky, &weapons}) {
            job->progress.cancel = true;
            if (job->task.valid())
                job->task.wait();
        }
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
        return task.valid() || benchmark_task.valid() || headset_task.valid() || benchmark_apply_task.valid() || mod_task.valid();
    }
    // Busy, or making sharper skies or weapons (repaints for its progress; Play waits for it).
    bool Working() const {
        return Busy() || sky.task.valid() || weapons.task.valid();
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
        // An old default the rows show, changed once as the game does (MigrateIni). While the game
        // runs, or should the file be locked, the game's own start does it instead.
        if (const auto migrated = MigrateIni(ini); migrated != ini && !RuntimeRunning()) {
            try {
                Write(paths.Ini(), migrated, ini);
                ini = migrated;
            } catch (const std::exception &) {
            }
        }
    }
    void SaveIni() {
        RequireStopped();
        std::vector<VR::Ini::Edit> list;
        for (const auto &[key, value] : edits)
            list.push_back({key, value});
        auto backup = Write(paths.Ini(), MigrateIni(ini, list), ini);
        Reload();
        status = backup.empty() ? "Settings saved."
                                : "Settings saved.";
    }
    void SaveProfile() {
        profile.saves = PathText(NormalizeSaveHome(Path(profile.saves)));
        profile.Save(paths);
        profile_pending = false;
        status = "Saved.";
    }
    // The launcher's own options (profile.json) save as soon as one is changed: a folder typed
    // and left (Enter or a click elsewhere), or picked with Browse; the mirror checkbox. They used
    // to wait for Save paths or Play, and closing the launcher before either lost them. Not on
    // every key: each changed write keeps a backup of the file (core.cpp Write). A folder typed
    // and never left is saved as the launcher closes (profile_pending). The game reads them at Play.
    void SaveChange() {
        Try([&] { SaveProfile(); });
    }
    bool profile_pending{}; // a folder typed, its field not left yet: saved at the latest on close
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
                    r.message = "Development build: updates are off.";
                else {
                    r.release = updater->CheckForUpdates();
                    r.message = r.release ? "Version " +
                                                ReleaseVersion::Display(r.release->TargetFullRelease.Version) +
                                                " is available."
                                          : "You are up to date.";
                }
            } catch (const std::exception &ex) {
                r.message = "Can't check for updates. You can still play.";
                r.error = ex.what();
            }
            return r;
        });
    }
    void Download() {
        if (Busy() || !available || !updater)
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
                r.message = "Update downloaded. Select Install & Restart when ready.";
            } catch (const std::exception &ex) {
                r.message = "Download failed. Nothing changed.";
                r.error = ex.what();
            }
            return r;
        });
    }
    void Tick() {
        if (mod_task.valid() && mod_task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            mod_note.clear();
            mod_selection.reset();
            mod_file_page = 0;
            confirm_mod_removal = false;
            Try([&] {
                auto result = mod_task.get();
                mod_report = std::move(result.report);
                mod_selection = std::move(result.selection);
                mod_note = std::move(result.note);
            });
            mod_refresh = true;
            repaint = 3;
        }
        if(benchmark_apply_task.valid() && benchmark_apply_task.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            // WM_CLOSE can also arrive on the very frame verification completes. Only resume
            // closing once both files are saved and reloaded. A failure stays visible for review.
            const bool finish_close = close_after_benchmark_apply || close_requested;
            close_after_benchmark_apply = close_requested = false;
            Try([&] {
                const auto verified_headset=benchmark_apply_task.get();
                if(benchmark_cancel) throw std::runtime_error("Applying the recommendation was cancelled. Settings unchanged.");
                RequireStopped();
                const auto& pending=*pending_benchmark_apply;
                if (!edits.empty())
                    throw std::runtime_error("VR settings changed during verification. Save or discard your edits and apply the scan again.");
                if(profile_pending || Read(paths.Profile())!=pending.expected_profile)
                    throw std::runtime_error("Launcher settings changed during verification. Review and apply again.");
                profile=Benchmark::ApplySettings(paths,profile,pending.candidate,pending.target_hz,pending.expected_ini);
                benchmark_applied=AppliedScan{pending.folder,pending.candidate.id,pending.before,pending.expected_ini};
                benchmark_headset=verified_headset;
                Reload();
                status="Recommendation applied. Check how it looks in VR.";
                LogBenchmark(status);
                close_requested = finish_close;
            });
            pending_benchmark_apply.reset();
            repaint=3;
        }
        if(system_task.valid() && system_task.wait_for(std::chrono::seconds(0))==std::future_status::ready)
            system=system_task.get();
        if (dlss5_task.valid() && dlss5_task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto r = dlss5_task.get();
            dlss5_added = r.added;
            dlss5_note = r.note;
            repaint = 3;
        }
        for (DetailJob *job : {&sky, &weapons}) {
            if (!job->task.valid() ||
                job->task.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                continue;
            try {
                job->note = job->task.get();
            } catch (const std::exception &e) {
                job->note = e.what();
            }
            job->state = Skies::Look(paths.User(), job->kind);
            repaint = 3;
        }
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
                else status="Scan done. Open PERF SCAN to review.";
                if(!benchmark_report->error.empty()) LogBenchmark("Scan stopped: "+benchmark_report->error);
                else if(benchmark_report->cancelled) LogBenchmark("Scan cancelled. Settings unchanged.");
                else if(benchmark_report->recommended)
                    LogBenchmark(std::string(benchmark_report->results[*benchmark_report->recommended].meets_target ? "Recommended: " : "Limited-headroom fallback: ")+benchmark_report->results[*benchmark_report->recommended].candidate.label+". Review below; settings are not applied yet.");
                else if(benchmark_report->provisional) LogBenchmark("One usable run, not confirmed yet. Review below.");
                else LogBenchmark("No confirmed recommendation. See the reasons below.");
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
    bool Field(const char *label, const char *hint, std::string &value, bool folder,
               const wchar_t *pattern = L"*.*") {
        ImGui::TextUnformatted(label);
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(98));
        bool changed = TextField("##path", value);
        bool done = ImGui::IsItemDeactivatedAfterEdit(); // typed, then left: saved (not per key)
        ImGui::SameLine();
        if (Button("Browse", {S(88), 0}))
            Try([&] {
                if (auto p = PickPath(window_handle, folder, Wide(label).c_str(), pattern)) {
                    value = PathText(
                        &value == &profile.saves ? NormalizeSaveHome(*p) : *p);
                    changed = done = true;
                }
            });
        if (done)
            SaveChange();
        else if (changed)
            profile_pending = true;
        Hint(hint);
        ImGui::PopID();
        ImGui::Dummy({0, S(2)});
        return changed;
    }
    // Match the effective setting in the Graphics tab. If Auto would pick Simplified Rendering,
    // clearing the checkbox explicitly selects native stereo.
    void SimplifiedRenderingOption() {
        constexpr const char* help =
            "Renders the scene once and uses depth to reconstruct the other eye. "
            "This reduces GPU load and can help if native stereo stutters or crashes. "
            "Small artifacts can appear near object edges. Applies on the next launch.";
        const bool auto_dibr = StartupStereo(system) == StereoPreset::Dibr;
        bool dibr = profile.SelectedStereo(system) == StereoPreset::Dibr;
        if (Checkbox(StereoLabel(StereoPreset::Dibr), &dibr))
            Try([&] {
                RequireStopped();
                auto updated = profile;
                updated.stereo = dibr        ? StereoChoice::Dibr
                                 : auto_dibr ? StereoChoice::Native
                                             : StereoChoice::Auto;
                updated.Save(paths);
                profile.stereo = updated.stereo;
                status = std::string(StereoLabel(profile.SelectedStereo(system))) +
                         " applies on the next launch.";
            });
        Details(help);
        HelpMarker(help);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(gold, "8 GB card or very poor performance? Try it.");
        Hint(system.multiview_limited && profile.stereo == StereoChoice::Auto
                 ? "Auto uses Simplified Rendering because this driver lacks multiview."
             : auto_dibr && profile.stereo == StereoChoice::Auto
                 ? "Simplified Rendering is the default on this AMD card. Untick it for native stereo."
                 : "Renders one view for both eyes to reduce GPU load. Small edge artifacts are possible.");
    }
    // Play style (core/vr/play_style.h, vr.ini play_style): motion controls, or a gamepad in
    // first or third person. A gamepad style sets the motion settings it cannot use while it
    // plays (their rows show locked) and never writes them: back on Motion they are yours again.
    void PlayStyleOption() {
        using VR::PlayStyle::Style;
        // Short names in the box; the line under it says what the chosen one is.
        static constexpr std::array<std::pair<const char *, Style>, 3> styles{{
            {"First Person / Motion", Style::Motion},
            {"Gamepad", Style::PadFirst},
            {"Third Person / Gamepad", Style::PadThird},
        }};
        const Style current = PlayStyleNow();
        std::vector<Pick> picks;
        for (const auto &[label, style] : styles)
            picks.push_back({label, style == current, true, ""});
        // A little room above and below sets the row apart from its neighbours.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {S(12), S(7)});
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored({.91f, .82f, .67f, 1.f}, "Play style");
        constexpr const char *help =
            "First Person / Motion: swing, shoot and heal with the VR controllers. Gamepad: play "
            "in first person with the game's own buttons on a controller, no motion controls. "
            "Third Person / Gamepad: the same, with the hunter in view. The motion settings a "
            "gamepad can't use are set for it and locked; yours come back when you switch to "
            "First Person / Motion. Applies on the next launch.";
        HelpMarker(help);
        ImGui::SameLine(0.f, S(12));
        ImGui::SetNextItemWidth(S(ValueWidth + 40));
        ImGui::PushID("play_style");
        const int pick = Combo(styles[size_t(current)].first, picks);
        ImGui::PopID();
        const float row_height = ImGui::GetFrameHeight(); // the box's, with its padding
        ImGui::PopStyleVar();
        // Left-handed right beside it (2026-11-25), centred on the box's height.
        ImGui::SameLine(0.f, S(18));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_height - ImGui::GetFrameHeight()) * .5f);
        LeftHandedOption(current);
        if (pick >= 0 && styles[size_t(pick)].second != current)
            Try([&] {
                RequireStopped();
                SetPlayStyle(styles[size_t(pick)].second);
            });
        // (Third person as the game reads first_person: VR::Mirror::ThirdPersonWord, "0" or "0.0".)
        const auto first_person = Value("first_person");
        Hint(current == Style::Motion && LeftHandedTicked() && first_person &&
                     VR::Mirror::ThirdPersonWord(*first_person)
                 ? "Left-handed needs the first-person camera (VR settings > Extras > Camera). "
                   "In third person you play right-handed."
             : current == Style::Motion && LeftHandedTicked()
                 ? "Swing, shoot and heal with the VR controllers. Left-handed: the weapon in "
                   "your left hand, the gun in your right."
             : current == Style::Motion
                 ? "Swing, shoot and heal with the VR controllers."
             : current == Style::PadFirst
                 ? "First person with a gamepad and the game's own buttons. Motion-only settings "
                   "are locked."
                 : "Third person with a gamepad: the hunter in view, the game's own buttons. "
                   "Motion-only settings are locked.");
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4));
    }
    void SetPlayStyle(VR::PlayStyle::Style chosen) {
        const auto saved = VR::Ini::ValueOf(ini, "play_style");
        // Returning to the saved style also restores its spelling (or absent default).
        if (VR::PlayStyle::Parse(saved.value_or("motion")) == chosen)
            Edit("play_style", saved);
        else
            Edit("play_style", VR::PlayStyle::Name(chosen));
    }
    // The play style the next launch uses (pending edits included).
    VR::PlayStyle::Style PlayStyleNow() {
        return VR::PlayStyle::Parse(Value("play_style").value_or("motion"))
            .value_or(VR::PlayStyle::Style::Motion);
    }
    // The play style sets one of the row's keys: the row is locked (core/vr/play_style.h).
    bool LockedByPlayStyle(const Setting &row) {
        const auto style = PlayStyleNow();
        for (const auto &key : row.keys)
            if (VR::PlayStyle::IsForced(style, key))
                return true;
        return false;
    }
    static constexpr const char *PlayStyleLockedText = "Set by Play style";
    // Left-handed is ticked for the next launch (pending edits included); only First Person /
    // Motion plays it (a gamepad style sets it off: core/vr/play_style.h).
    bool LeftHandedTicked() {
        const auto value = Value("left_handed");
        return PlayStyleNow() == VR::PlayStyle::Style::Motion && value && VR::Mirror::OnWord(*value);
    }
    // Left-handed beside Play style (2026-11-25: "right next to the Play style
    // selection"): the Controls tab's launcher-only row (vr.ini left_handed), same switch.
    // Unticked is the game's default (Off): the key leaves vr.ini, unless the file already holds an
    // off value (the Controls tab's Off writes "0"), which is kept; ticked keeps the file's own on
    // word ("true") the same way: no edit for the same thing (VR::Mirror::OnWord, as the game reads).
    void LeftHandedOption(VR::PlayStyle::Style style) {
        static const Setting *row = [] {
            for (const auto &s : Settings())
                if (s.keys.front() == "left_handed")
                    return &s;
            return static_cast<const Setting *>(nullptr);
        }();
        const bool motion = style == VR::PlayStyle::Style::Motion;
        bool on = LeftHandedTicked();
        ImGui::BeginDisabled(!motion);
        if (Checkbox("Left-handed", &on))
            Try([&] {
                RequireStopped();
                const auto file = VR::Ini::ValueOf(ini, "left_handed");
                const bool file_on = file && VR::Mirror::OnWord(*file);
                Edit("left_handed", on        ? (file_on ? file : std::optional<std::string>("1"))
                                    : file && !file_on ? file
                                                       : std::nullopt);
            });
        ImGui::EndDisabled();
        Details(!motion ? "Gamepad play styles are right-handed. Pick First Person / Motion to "
                          "play left-handed."
                : row   ? row->details
                        : nullptr);
    }
    void LaunchOptions() {
        if (Checkbox("Fullscreen desktop mirror", &profile.fullscreen)) {
            profile.fullscreen_chosen = true; // the player's own choice from now on
            SaveChange();
        }
        // The desktop window is a light 30 fps view of one eye (vr.ini desktop_mirror), there to
        // show the game is running: recordings come from the runtime's own mirror. Small and
        // muted, under the row, so it does not move the layout around it.
        ImGui::PushFont(sidebar_tag_font);
        ImGui::TextDisabled("To record or stream, use the SteamVR or Oculus mirror. "
                            "This window is a light 30 fps view of one eye.");
        ImGui::PopFont();
    }
#if BBVR_BRIDGE
    // Updates continue under the new installer: one button installs it (settings and saves come
    // along, they are outside both installations) and removes this copy. A portable copy gets the
    // new portable ZIP instead.
    void BridgeNotice() {
        ImGui::Dummy({0, S(4)});
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - S(130)); // clear of the motif
        if (installed_copy)
            ImGui::TextColored(gold, "Bldbrne-VR has a new installer. Install it once to keep updating; settings and saves come along.");
        else
            ImGui::TextColored(gold, "Bldbrne-VR has a new download. Get the new portable ZIP to keep updating.");
        ImGui::PopTextWrapPos();
        ImGui::BeginDisabled(running || Busy());
        if (installed_copy) {
            if (Button(moving.valid() ? "Installing the new version..."
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
        } else if (Button("Get the new portable ZIP")) {
            Try([&] { OpenPath(NewPortableUrl); });
        }
        ImGui::EndDisabled();
        if (!move_error.empty())
            ImGui::TextWrapped("%s", move_error.c_str());
    }
#endif
    void ValidatePlaySetup() {
        setup_issue.clear();
        game_folder_needed = true;
        try {
            if (profile.game.empty())
                throw std::runtime_error("Choose the extracted Bloodborne 1.09 folder containing eboot.bin.");
            const auto game = GameExecutable(Path(profile.game));
            const auto time = fs::last_write_time(game);
            // Let CheckGameBuild explain missing or unsupported metadata.
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
            if (!build_issue.empty()) {
                setup_issue = build_issue;
                return;
            }
            game_folder_needed = false;
            ValidateSetup(profile);
        } catch (const std::exception &ex) {
            setup_issue = ex.what();
        }
    }
    void HuntStatus() {
        const bool ready = setup_issue.empty() && !Working() && !running;
        const char* message = running ? "The hunt is in progress"
            : game_folder_needed ? (profile.game.empty() ? "Game folder needed" : "Check your game folder")
            : !setup_issue.empty() ? setup_issue.c_str()
            : pending_game_launch ? "Detecting your headset's refresh rate."
            : benchmark_task.valid() ? "PERF SCAN is running. Wait for it to finish before playing."
            : downloading ? "Wait for the update to finish downloading before playing."
            : Working() ? "Wait for the current launcher task to finish before playing."
            : "Ready to play";
        if (available) {
            const auto version = ReleaseVersion::Display(available->TargetFullRelease.Version);
            const bool can_download = !downloaded && !Busy();
            const std::string label = downloading ? "Downloading " + version + "  /  View progress"
                : downloaded ? "Update " + version + " ready  /  Review update"
                : can_download ? "NEW VERSION AVAILABLE " + version + " - Click here to download"
                : "View update " + version;
            ImGui::PushFont(brand_font);
            if (UpdateButton(label.c_str(), {-1,S(34)})) {
                page = 2;
                if (can_download)
                    Download();
            }
            ImGui::PopFont();
            if (!ready)
                Hint(message);
            return;
        }

        const auto p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        auto* d = ImGui::GetWindowDrawList();
        ImGui::PushFont(brand_font);
        const float version_width = ImGui::CalcTextSize(BBVR_VERSION).x;
        const float text_width = std::max(S(1), width-version_width-S(78));
        const float height = std::max(S(30),
            ImGui::CalcTextSize(message,nullptr,false,text_width).y + S(10));
        const ImVec2 bottom{p.x+width,p.y+height};
        d->AddRectFilled({p.x+S(4),p.y}, {bottom.x-S(4),bottom.y}, IM_COL32(12,15,16,255),S(4));
        HunterTheme::PlaqueFrame(d,p,bottom,theme_art,S(1));
        const auto color = ready ? IM_COL32(141,160,114,255) : IM_COL32(179,147,91,255);
        d->AddCircleFilled({p.x+S(22),p.y+S(15)}, S(4), color, 12);
        d->AddCircle({p.x+S(22),p.y+S(15)}, S(6), IM_COL32(95,91,77,255), 12);
        d->AddCircleFilled({p.x+S(21),p.y+S(14)}, S(1.5f), IM_COL32(225,222,189,190), 8);
        ImGui::SetCursorScreenPos({p.x+S(39),p.y+S(5)});
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+text_width);
        ImGui::TextUnformatted(message);
        ImGui::PopTextWrapPos();
        d->AddText({p.x+width-version_width-S(23),p.y+S(5)}, ImGui::GetColorU32(ImGuiCol_Text), BBVR_VERSION);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy({width,height});
    }
    void ChooseGameFolder() {
        Try([&] {
            if (const auto folder = PickPath(window_handle, true, L"Game folder", L"*.*")) {
                profile.game = PathText(*folder);
                dlc_checked = false;
                ValidatePlaySetup();
                SaveProfile();
                repaint = 3;
            }
        });
    }
    void GameFolderPanel() {
        const auto top = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float inset = S(20);
        const float field_width = width - 2.f*inset;
        auto* draw = ImGui::GetWindowDrawList();
        ImDrawListSplitter layers;
        layers.Split(draw, 2);
        layers.SetCurrentChannel(draw, 1);
        ImGui::Indent(inset);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(13));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {S(10), S(6)});
        ImGui::PushFont(brand_font);
        ImGui::TextColored({.91f,.82f,.67f,1.f}, "Game folder");
        if (!game_folder_needed) {
            constexpr const char* verified = "Bloodborne 1.09 verified";
            const float right = ImGui::GetCursorPosX() + field_width;
            ImGui::SameLine(right - ImGui::CalcTextSize(verified).x);
            ImGui::TextColored({.70f,.77f,.58f,1.f}, "%s", verified);
        }
        ImGui::BeginDisabled(running || Busy());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{S(12),S(7)});
        ImGui::SetNextItemWidth(field_width-S(98));
        const bool game_typed =
            TextField("##game-folder", profile.game, "Path to your Bloodborne folder");
        const bool game_done = ImGui::IsItemDeactivatedAfterEdit(); // typed, then left
        if (game_typed)
            ValidatePlaySetup();
        if (game_done)
            SaveChange();
        else if (game_typed)
            profile_pending = true;
        ImGui::SameLine();
        if (Button("Browse",{S(88),0}))
            ChooseGameFolder();
        ImGui::PopStyleVar();
        ImGui::EndDisabled();
        ImGui::PopFont();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + field_width);
        if (game_folder_needed && !profile.game.empty())
            ImGui::TextColored(gold, "%s", setup_issue.c_str());
        else
            Hint(profile.game.empty()
                ? "Choose the extracted Bloodborne 1.09 folder containing eboot.bin."
                : "Saves and DLC can be changed under Optional setup.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleVar();
        const float bottom = ImGui::GetItemRectMax().y + S(14);
        ImGui::Unindent(inset);
        layers.SetCurrentChannel(draw, 0);
        HunterTheme::InsetPanel(draw, top, {top.x+width, bottom}, theme_art, S(1));
        if (theme_art.hardware.view) {
            const float center = top.x + width*.5f;
            HunterTheme::HardwarePiece(draw, theme_art, {center-S(32),top.y-S(6)},
                {center+S(32),top.y+S(12)}, {20,100}, {212,154}, IM_COL32(207,188,163,255));
        }
        layers.Merge(draw);
        ImGui::SetCursorScreenPos(top);
        ImGui::Dummy({width, bottom-top.y});
    }
    void PlayPage(float side_inset) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {S(10),S(8)});
        const auto content_start = ImGui::GetCursorScreenPos();
        const auto viewport = ImGui::GetWindowPos(), viewport_size = ImGui::GetWindowSize();
        const ImVec2 hero_pos{viewport.x-side_inset,content_start.y-S(24)};
        const float hero_width = viewport_size.x+2*side_inset;
        const float window_height = ImGui::GetIO().DisplaySize.y/S(1);
        const float section_gap = S(7.f + std::clamp((window_height-650.f)*.08f,0.f,10.f));
        const float extra_height = std::clamp((window_height-675.f)*.20f,0.f,64.f);
        const float hero_height = S(145 + extra_height);
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 hero_end{hero_pos.x + hero_width, hero_pos.y + hero_height};
        const float fade_height = hero_height*.30f;
        const float fade_top = hero_end.y-fade_height;
        const ImVec2 fade_end{hero_end.x,fade_top};
        draw->PushClipRect({hero_pos.x,viewport.y-S(20)},
                          {hero_end.x-S(12),viewport.y+viewport_size.y},false);
        if (theme_art.hero.view) {
            // Crop to cover the banner without stretching its architecture. Keep the sky visible.
            const float source_aspect = float(theme_art.hero.width) / theme_art.hero.height;
            const float target_aspect = hero_width / hero_height;
            ImVec2 uv0{0,0}, uv1{1,1};
            if (target_aspect > source_aspect) uv1.y = source_aspect / target_aspect;
            else {
                const float span = target_aspect / source_aspect;
                uv0.x = (1-span)*.5f;
                uv1.x = 1-uv0.x;
            }
            const float fade_v = uv1.y*(fade_top-hero_pos.y)/hero_height;
            const auto texture = reinterpret_cast<ImTextureID>(theme_art.hero.view.Get());
            draw->AddImage(texture,hero_pos,fade_end,uv0,{uv1.x,fade_v});
            draw->AddRectFilledMultiColor(hero_pos,fade_end,
                IM_COL32(0,0,0,95),IM_COL32(0,0,0,0),IM_COL32(0,0,0,24),IM_COL32(0,0,0,113));
            const int fade_vertex = draw->VtxBuffer.Size;
            draw->AddImage(texture,{hero_pos.x,fade_top},hero_end,{uv0.x,fade_v},uv1);
            // Match the shading at the join and let the leather show through at the bottom.
            draw->VtxBuffer[fade_vertex].col = IM_COL32(142,142,142,255);
            draw->VtxBuffer[fade_vertex+1].col = IM_COL32(231,231,231,255);
            draw->VtxBuffer[fade_vertex+2].col = IM_COL32(231,231,231,0);
            draw->VtxBuffer[fade_vertex+3].col = IM_COL32(142,142,142,0);
        } else {
            constexpr ImU32 sky = IM_COL32(17,24,29,255);
            draw->AddRectFilled(hero_pos,fade_end,sky);
            draw->AddRectFilledMultiColor({hero_pos.x,fade_top},hero_end,sky,sky,0,0);
        }
        draw->PopClipRect();
        const float title_x = content_start.x+S(2);
        const float title_y = hero_pos.y+S(34+extra_height*.4f);
        constexpr const char* title = "The hunt awaits.";
        constexpr const char* subtitle = "Return to Yharnam. See it with your own eyes.";
        ImGui::SetCursorScreenPos({title_x,title_y});
        ImGui::PushFont(display_font);
        const float title_width = ImGui::CalcTextSize(title).x;
        const float divider_y = title_y+ImGui::GetFontSize()+S(8);
        HunterTheme::WeatheredTitle(title,title_width,S(1));
        ImGui::PopFont();
        const float divider_width = title_width*.76f;
        HunterTheme::Divider(draw,{title_x,divider_y},divider_width,S(1));
        ImGui::PushFont(italic_font);
        ImGui::SetCursorScreenPos({title_x,divider_y+S(10)});
        ImGui::TextUnformatted(subtitle);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos({content_start.x, hero_end.y - S(4)});
        ValidatePlaySetup();
        HuntStatus();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + section_gap);
        GameFolderPanel();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + section_gap);
#if BBVR_BRIDGE
        BridgeNotice();
#endif
        const bool resolution_warning = HighResolutionWarning();
        if (resolution_warning)
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + section_gap);
        // Discover only when a selection changes (including reselecting the same folder).
        // Launch always rechecks; there is no directory scan in the UI's steady-state loop.
        if (!dlc_checked || dlc_game != profile.game || dlc_path != profile.dlc) {
            dlc_game = profile.game; dlc_path = profile.dlc; dlc_checked = true;
            dlc_game_found = false;
            try {
                GameExecutable(Path(profile.game));
                dlc_game_found = true;
                dlc_status = ResolveDlc(paths, profile);
            } catch (const std::exception& e) {
                dlc_status = {{}, dlc_game_found
                                      ? std::string("Couldn't check for DLC (the game starts without it): ") + e.what()
                                      : e.what(), false};
            }
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(7));
        ImGui::BeginDisabled(running || Busy());
        SimplifiedRenderingOption();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(7));
        PlayStyleOption();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + section_gap);
        if (reveal_play_setup)
            ImGui::SetNextItemOpen(true);
        const bool setup_open = Header("Optional setup  /  Saves, DLC, runtime and mirror");
        if (reveal_play_setup) {
            ImGui::SetScrollHereY(0.0f);
            reveal_play_setup = false;
        }
        if (setup_open) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5));
            if (Field("Saves folder (optional)",
                      "Leave as is, or pick another shadPS4's home or savedata folder to use its saves.",
                      profile.saves, true))
                ValidatePlaySetup();
            if (dlc_game_found) {
                Hint(dlc_status.message.c_str());
                // Bundled or discovered DLC needs no folder picker unless one was set before.
                if ((!dlc_status.found || !profile.dlc.empty()) &&
                    Field("DLC folder (optional)",
                          "If installed elsewhere, choose the extracted DLC folder or its parent. "
                          "Leave empty for automatic detection.", profile.dlc, true))
                    dlc_checked = false;
            }
            // Always the included runtime (Profile::Load): no other shadPS4 is needed, nor used
            // (another build lacks the VR renderer, its fixes and patches).
            Hint(profile.runtime.empty()
                     ? "The included runtime is missing. Reinstall Bldbrne-VR."
                     : "Uses the included VR runtime. No other shadPS4 needed.");
            if (Button("Open system modules folder"))
                Try([&] {
                    const auto modules = paths.User() / "sys_modules";
                    fs::create_directories(modules);
                    OpenPath(modules);
                });
            Hint("Optional system modules go in user / sys_modules in the data folder.");
            LaunchOptions();
        }
        ImGui::EndDisabled();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {S(10), S(5)});
        const auto section_heading = [section_gap](const char* title) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + section_gap);
            ImGui::Separator();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4));
            ImGui::PushFont(brand_font);
            ImGui::TextUnformatted(title);
            ImGui::PopFont();
        };
        section_heading("System & headset");
        StatusCards();
        section_heading("Texture upgrades");
        DetailRows(sky, "Sharper skies",
                   "Sky, moon and clouds upscaled by AI on this PC, once (a few minutes).",
                   "Makes sharper copies of the game's sky, moon and cloud textures on your PC. "
                   "The one-time job uses your GPU for a few minutes. The game uses the copies "
                   "on its next launch. Remove deletes the generated copies and restores the original skies.",
                   "Sharper skies removed: the game uses its own skies.");
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5));
        DetailRows(weapons, "Sharper weapons",
                   "Every weapon's textures, both hands, upscaled 2x by AI on this PC, once "
                   "(a few minutes).",
                   "Makes textures twice as wide and tall for weapons in both hands. "
                   "The one-time job runs on your GPU for a few minutes and stores about 500 MB on your PC. "
                   "The game uses the copies on its next launch. Remove restores the original textures.",
                   "Sharper weapons removed: the game uses its own weapon textures.");
        ImGui::PopStyleVar();
        if (!resolution_warning)
            NewPlayerGraphicsNote(section_gap);
        ImGui::PopStyleVar();
    }
    void PlayButton(float height) {
        if (!setup_issue.empty() && !running && !Working()) {
            ImGui::PushStyleColor(ImGuiCol_Button, {.30f,.195f,.105f,1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.43f,.28f,.14f,1.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, {.24f,.15f,.085f,1.f});
            ImGui::PushFont(logo_font);
            const char* action = game_folder_needed ? "CHOOSE GAME FOLDER" : "REVIEW SETUP";
            if (Button(action, {-1, height}, HunterTheme::ButtonFace::Gilt)) {
                if (game_folder_needed)
                    ChooseGameFolder();
                else {
                    reveal_play_setup = true;
                    repaint = 3; // open the section next frame, then apply its scroll target
                }
            }
            ImGui::PopFont();
            ImGui::PopStyleColor(3);
            return;
        }
        ImGui::BeginDisabled(running || Working() || !setup_issue.empty());
        ImGui::PushStyleColor(ImGuiCol_Button, {0.23f, 0.035f, 0.032f, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.47f, 0.105f, 0.09f, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.28f, 0.06f, 0.06f, 1});
        ImGui::PushFont(launch_font);
        const float letter_spacing = setup_issue.empty() ? (running ? 6.f : 5.f) : .3f;
        if (HunterTheme::LetteredButton(running               ? "HUNT IN PROGRESS"
                          : setup_issue.empty() ? "BEGIN THE HUNT"
                                                : "COMPLETE SETUP TO PLAY",
                          {-1, height}, theme_art, S(1), S(letter_spacing)))
            Try([&] { StartGame(); });
        ImGui::PopFont();
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        Details(setup_issue.c_str());
        if(pending_game_launch) {
            Hint("Detecting headset refresh rate before launch (90 FPS fallback)...");
            if(Button("Cancel launch")) benchmark_cancel=true;
        }
    }
    void StartGame() {
        if(Working()) throw std::runtime_error("Wait for the current launcher operation to finish.");
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
        dlc_checked = false; // Refresh the picker/status even if a removed DLC folder aborts launch.
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
        out += std::string("Stereo preset ") + StereoLabel(profile.SelectedStereo(system)) + "\n";
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
    // Always visible on Play, below the editable launch options.
    void StatusCards() {
        constexpr double GB = double(1ull << 30);
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, {.56f,.49f,.40f,.32f});
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {S(9), ImGui::GetStyle().CellPadding.y});
        const bool visible = ImGui::BeginTable("setup-status", 3,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (!visible)
            return;
        ImGui::TableNextColumn();
        ImGui::TextColored(gold, "GAME FILES");
        Hint(setup_issue.empty() ? "Game, runtime and save paths are set."
             : game_folder_needed ? "Check the Game folder panel above."
                                  : setup_issue.c_str());
        ImGui::TableNextColumn();
        if (xr_runtime.empty()) {
            ImGui::TextColored(muted, "HEADSET SETUP");
            Hint("Set SteamVR or Meta Quest Link as the active OpenXR runtime.");
        } else {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextColored(gold, "HEADSET  /  %s", xr_runtime.c_str());
            ImGui::PopTextWrapPos();
            Hint("Connect your headset. The game waits for it.");
        }
        ImGui::TableNextColumn();
        ImGui::TextColored(system.notes.empty() ? gold : muted, "GRAPHICS CARD");
        std::string card = system.gpu.empty() ? "Not found" : system.gpu;
        if (!system.gpu.empty()) {
            char memory[40];
            snprintf(memory, sizeof(memory), "%.0f GB VRAM", system.vram / GB);
            std::string specs = memory;
            if (!system.driver.empty()) {
                const std::string driver = "driver " + system.driver;
                const std::string combined = specs + "  /  " + driver;
                specs = ImGui::CalcTextSize(combined.c_str()).x <= ImGui::GetContentRegionAvail().x
                            ? combined : specs + "\n" + driver;
            }
            card += "\n" + specs;
        }
        for (const auto &note : system.notes)
            card += "\n" + note;
        if (profile.SelectedStereo(system) == StereoPreset::Dibr)
            card += "\nRuns in Simplified Rendering (the Play page's option).";
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
        Hint("Saves are backed up at every launch (last 10). Restoring backs up the current saves first.");
        ImGui::PopTextWrapPos();
        InsetList("backup-list",S(backups.empty() ? 58 : 220),theme_art);
        if (backups.empty())
            Hint("No backups yet. One is made when you start the game.");
        for (const auto &backup : backups) {
            ImGui::PushID(PathText(backup.folder.filename()).c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(backup.label.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(70));
            ImGui::BeginDisabled(running);
            if (SmallButton("Restore"))
                restore_target = backup.folder;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        EndInsetList();
        ImGui::BeginDisabled(running);
        if (Button("Back up now",{}))
            Try([&] {
                const auto made = BackupSaves(paths, NormalizeSaveHome(Path(profile.saves)));
                backups = SaveBackups(paths);
                status = made.empty() ? "Your saves match the newest backup."
                                      : "Saves backed up.";
            });
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (Button("Open folder"))
            Try([&] {
                fs::create_directories(SaveBackupRoot(paths));
                OpenPath(SaveBackupRoot(paths));
            });
        ImGui::SameLine();
        if (Button("Close"))
            ImGui::CloseCurrentPopup();
        if (restore_target)
            ImGui::OpenPopup("Restore saves?");
        if (Modal("Restore saves?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Replace your saves with this backup?");
            Hint("Current saves are backed up first.");
            if (Button("Restore")) {
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
            if (Button("Cancel")) {
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
        Hint("Your saves were backed up. Copy the support info into a bug report.");
        ImGui::PopTextWrapPos();
        if (!crash->lines.empty()) {
            ImGui::TextColored(gold, "LAST PROBLEMS IN THE LOG");
            const float log_height = std::clamp(36.f+float(crash->lines.size())*24.f,72.f,150.f);
            InsetList("crash-lines",S(log_height),theme_art,ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto &line : crash->lines)
                Hint(line.c_str());
            EndInsetList();
        }
        if (Button("Try again")) {
            ImGui::CloseCurrentPopup();
            Try([&] { StartGame(); });
        }
        ImGui::SameLine();
        if (Button("Open log"))
            Try([&] {
                const auto log = LogFile(paths);
                OpenPath(log.empty() ? paths.User() / "log" : log);
            });
        ImGui::SameLine();
        if (Button("Copy support info")) {
            ImGui::SetClipboardText(SupportInfo().c_str());
            status = "Support info copied. Paste it into your bug report.";
        }
        ImGui::SameLine();
        if (Button("Close"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    std::optional<std::string> Value(const std::string &key) {
        auto it = edits.find(key);
        return it == edits.end() ? VR::Ini::ValueOf(ini, key) : it->second;
    }
    static void Details(const char *text) {
        if (text && *text && HunterTheme::TooltipHovered())
            HunterTheme::Tooltip(S(1),text);
    }
    static void HelpMarker(const char *text) {
        ImGui::SameLine(0.f, S(8));
        ImGui::TextColored(gold, "(?)");
        Details(text);
    }
    // The VR settings page's headings, drawn above the first row shown under them (a search shows
    // only the tabs and groups it finds): a search result's tab, which opens it, and a group.
    const char *pending_tab{}, *pending_group{}, *shown_group{};
    const Setting *linked_setting{};
    bool scroll_to_linked_setting{};
    std::string needle; // the search, lower case
    int found{};        // rows shown this frame
    static bool SameText(const char *a, const char *b) {
        return a == b || (a && b && std::string_view(a) == b);
    }
    static std::string Lower(std::string t) {
        std::transform(t.begin(), t.end(), t.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return t;
    }
    // A row's group from here on: its heading waits for the first row shown.
    void Group(const char *group) {
        if (!SameText(group, shown_group))
            pending_group = shown_group = group;
    }
    void Headings() {
        if (pending_tab) {
            ImGui::Dummy({0, S(found ? 10 : 2)});
            ImGui::PushFont(brand_font);
            ImGui::PushStyleColor(ImGuiCol_Text, gold);
            const std::string open = std::string(pending_tab) + "  >";
            if (ImGui::Selectable(open.c_str(), false, 0, ImGui::CalcTextSize(open.c_str())))
                OpenTab(pending_tab);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            if (HunterTheme::TooltipHovered()) {
                const std::string tip = std::string("Open the ") + pending_tab + " tab.";
                HunterTheme::Tooltip(S(1),tip.c_str());
            }
            pending_tab = nullptr;
        }
        if (pending_group) {
            ImGui::Dummy({0, S(6)});
            ImGui::PushFont(brand_font);
            ImGui::TextColored(gold, "%s", pending_group);
            ImGui::PopFont();
            ImGui::Separator();
            pending_group = nullptr;
        }
        ++found;
    }
    void OpenTab(std::string_view name) {
        const auto &categories = Categories();
        for (int i = 0; i < int(categories.size()); ++i)
            if (name == categories[i])
                category = i;
        search.clear();
        linked_setting = nullptr;
        scroll_to_linked_setting = false;
    }
    // One settings row: label and help on the left, the control on the right (ValueWidth: what
    // goes there fits it, one item to a line).
    static constexpr float ValueWidth = 205;
    template <class Control>
    void Row(const char *label, const char *help, const char *details, Control control) {
        Row(label, help, details, control, [] {});
    }
    // ... and `below` under the help: what is wider than the control (the Scaling row's sizes).
    template <class Control, class Below>
    void Row(const char *label, const char *help, const char *details, Control control,
             Below below, bool show_help_marker = false) {
        Headings();
        ImGui::PushID(label);
        if (ImGui::BeginTable("row", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, S(ValueWidth));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            Details(details);
            if (show_help_marker)
                HelpMarker(details);
            Hint(help);
            Details(details);
            below();
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
        // The list's own padding (the main window has none): the last choice clear of its edge.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(6), S(6)});
        const bool open = ImGui::BeginCombo("##value", current);
        ImGui::PopStyleVar();
        if (open) {
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
        const bool reset = !value;
        if (!value)
            value = ResetValue(key);
        if (value == VR::Ini::ValueOf(ini, key))
            edits.erase(key);
        else
            edits[key] = value;
        status = edits.empty() ? "Settings match the saved file."
                               : "Unsaved settings. Save or launch to apply.";
        // A reset also removes the keys that would undo it (ResetAlsoRemoves: Light attack stamina's
        // old switch, when on), as the VR page's reset does.
        if (reset)
            for (const std::string &other : ResetAlsoRemoves(key, LinesOf("melee_stamina")))
                Edit(other, std::nullopt);
    }
    // A key's lines for the next launch: a pending edit's value alone (none for a removal: the save
    // writes it so, every line of the key replaced), else the file's, in order.
    std::vector<std::string> LinesOf(const std::string &key) {
        if (const auto it = edits.find(key); it != edits.end())
            return it->second ? std::vector<std::string>{*it->second} : std::vector<std::string>{};
        return VR::Ini::ValuesOf(ini, key);
    }
    GraphicsFields PendingGraphics() {
        return ParseGraphics(Value("graphics_config"));
    }
    // The headset's request from the game's last start, re-read every two seconds (a launch
    // rewrites it).
    const HeadsetRequest *KnownHeadset() {
        if (headset_request_read < 0 || ImGui::GetTime() - headset_request_read > 2) {
            headset_request_read = ImGui::GetTime();
            headset_request = LastHeadsetRequest(paths);
        }
        return headset_request ? &*headset_request : nullptr;
    }
    // Each eye's output and render size for these graphics fields, as the game computes them
    // (graphics_resolution.h).
    std::pair<VR::Graphics::Extent, VR::Graphics::Extent> EyeSizes(const HeadsetRequest &h,
                                                                   const GraphicsFields &f) {
        const VR::Graphics::Extent eye{h.width, h.height};
        const auto output = VR::Graphics::OutputResolution(eye, eye, f[1],
                                                            {h.max_width, h.max_height});
        const auto config = VR::Graphics::Parse(GraphicsText(f));
        const bool upscaled = config && config->upscaler != VR::Graphics::Upscaler::Off &&
                              profile.SelectedStereo(system) == StereoPreset::Native;
        return {output, upscaled ? VR::Graphics::WholeFrameResolution(output, *config) : output};
    }
    // The headset's panel (core/vr/headset_panels.h): the player's pick, else the one the
    // runtime's name says; nullptr when neither.
    const VR::Headsets::Headset *PanelHeadset(const HeadsetRequest &h) const {
        if (!profile.headset.empty())
            if (const auto *picked = VR::Headsets::ById(profile.headset))
                return picked;
        return VR::Headsets::FromSystemName(h.system);
    }
    // The scaling (whole percent) whose output is the panel's own width: the suggested one, as
    // the runtime's request includes its supersampling (PS VR2 on SteamVR at 100%: 3400 wide).
    std::optional<int> PanelScale(const HeadsetRequest &h) const {
        const auto *panel = PanelHeadset(h);
        if (!panel || !h.width)
            return std::nullopt;
        return std::clamp(int(std::lround(panel->width * 100.0 / h.width)) - 100, -50, 50);
    }
    // The suggested scaling: the panel's (else the current one, never above it), lowered in steps
    // of 10 until the estimated graphics memory fits the card, and without a known panel until the
    // output stays within HighEyeWidth too; -50 at most.
    int FittingScale(const HeadsetRequest &h, GraphicsFields f) {
        const auto panel = PanelScale(h);
        if (panel)
            f[1] = std::min(f[1], *panel);
        for (f[1] = std::min(f[1], 50); f[1] > -50; f[1] -= 10)
            if ((panel || EyeSizes(h, f).first.width <= HighEyeWidth) && VramFits(VramMb(h, f)))
                break;
        return std::max(f[1], -50);
    }
    // Scaling row: the headset the panel suggestion follows, to pick when its name does not say
    // (SteamVR names Index, Vive and Beyond alike "lighthouse").
    void HeadsetCombo(const HeadsetRequest &h) {
        const auto *detected = VR::Headsets::FromSystemName(h.system);
        const auto *picked = profile.headset.empty() ? nullptr : VR::Headsets::ById(profile.headset);
        const std::string automatic =
            detected ? "Auto (" + std::string(detected->model) + ")" : "Auto (not recognised)";
        const std::string preview = picked ? std::string(picked->model) : automatic;
        const auto choose = [&](std::string_view id) {
            Try([&] {
                auto updated = profile;
                updated.headset = std::string(id);
                updated.Save(paths);
                profile.headset = updated.headset;
                status = "Headset saved.";
            });
        };
        ImGui::SetNextItemWidth(S(240));
        if (ImGui::BeginCombo("##headset", preview.c_str())) {
            if (ImGui::Selectable(automatic.c_str(), picked == nullptr))
                choose({});
            for (const auto &headset : VR::Headsets::All) {
                const std::string label = std::string(headset.model) + "  " +
                                          std::to_string(headset.width) + " x " +
                                          std::to_string(headset.height);
                if (ImGui::Selectable(label.c_str(), picked == &headset))
                    choose(headset.id);
            }
            ImGui::EndCombo();
        }
    }
    static constexpr ImVec4 high_red{0.93f, 0.33f, 0.28f, 1};
    static constexpr ImVec4 ready_green{141 / 255.f, 160 / 255.f, 114 / 255.f, 1}; // HuntStatus's light
    // The game's estimated graphics memory for these fields (EstimateVramMb), MB; 0 when it does
    // not apply (DIBR renders one view: the fit is for native stereo).
    double VramMb(const HeadsetRequest &h, const GraphicsFields &f) {
        if (profile.SelectedStereo(system) != StereoPreset::Native)
            return 0;
        const auto [output, render] = EyeSizes(h, f);
        return EstimateVramMb(render.width, render.height, output.width, output.height,
                              render.width != output.width);
    }
    // Whether that estimate leaves the headset software and Windows their share of the card.
    bool VramFits(double mb) const {
        return VramFitsCard(mb, system.vram);
    }
    // A framed notice across the page, clear of the emblem (the scan suggestion's look): `ink` is
    // its border, wash and title. EndBanner after it.
    void BeginBanner(const char *id, ImVec4 ink, const char *title) {
        ImGui::Dummy({0, S(8)});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{ink.x, ink.y, ink.z, 0.055f});
        ImGui::PushStyleColor(ImGuiCol_Border, ink);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(14), S(12)});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, S(1));
        ImGui::BeginChild(id, {ImGui::GetContentRegionAvail().x, 0},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushFont(brand_font);
        ImGui::TextColored(ink, "%s", title);
        ImGui::PopFont();
    }
    void EndBanner() {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }
    // Primary actions share the dark crimson enamel treatment.
    bool AccentButton(const char *label) {
        HunterTheme::PushAccent();
        const bool clicked = Button(label,{0,S(36)});
        ImGui::PopStyleColor(4);
        return clicked;
    }
    // Play page, at the top: a framed red banner, hard to miss (2026-10-30, the user: the red line
    // was easy to overlook), when the saved scaling renders each eye wider than HighEyeWidth or
    // may not fit in video memory: the fitting scale, a PERF SCAN (it finds smooth settings for
    // this PC) and the Graphics settings. True when it is shown.
    bool HighResolutionWarning() {
        const auto *h = KnownHeadset();
        if (!h)
            return false;
        auto f = PendingGraphics();
        const auto [output, render] = EyeSizes(*h, f);
        const double vram = VramMb(*h, f);
        const bool high = output.width > HighEyeWidth;
        if (!high && VramFits(vram))
            return false;
        BeginBanner("resolution-warning", high_red,
                    high ? "RESOLUTION TOO HIGH" : "MAY NOT FIT IN GRAPHICS MEMORY");
        if (high) {
            if (render.width != output.width || render.height != output.height)
                ImGui::TextWrapped("Render target per eye: %u x %u, upscaled output: %u x %u.",
                                   render.width, render.height, output.width, output.height);
            else
                ImGui::TextWrapped("Requested output per eye: %u x %u.", output.width, output.height);
            ImGui::TextWrapped("At this output size, demanding scenes may drop frames.");
        }
        if (!VramFits(vram))
            ImGui::TextWrapped("These settings need about %.1f of your card's %.0f GB of video "
                               "memory: expect stutters and missing textures.",
                               vram / 1024, double(system.vram >> 20) / 1024);
        ImGui::TextWrapped("Run a PERF SCAN: in 3-6 minutes it finds the smoothest settings for "
                           "your PC (nothing changes until you press Apply). Or scale it down.");
        ImGui::Dummy({0, S(4)});
        if (AccentButton("RUN PERF SCAN"))
            open_perf_scan = true;
        auto scaled = f;
        scaled[1] = FittingScale(*h, f);
        const auto fitted = EyeSizes(*h, scaled).first;
        const std::string label = "Scale to " + std::to_string(fitted.width) + " x " +
                                  std::to_string(fitted.height);
        if (scaled[1] != f[1]) {
            ImGui::SameLine();
            if (Button(label.c_str(), {0, S(36)}))
                Try([&] {
                    RequireStopped();
                    SetGraphics(scaled);
                    SaveIni();
                    status = "Scaled to " + std::to_string(fitted.width) + " x " +
                             std::to_string(fitted.height) + ".";
                });
        }
        ImGui::SameLine();
        if (Button("Settings", {0, S(36)})) {
            page = 1;
            search.clear();
            const auto &categories = Categories();
            for (int i = 0; i < int(categories.size()); ++i)
                if (std::string_view(categories[i]) == "Graphics")
                    category = i;
        }
        EndBanner();
        return true;
    }
    // Below setup and system details. Urgent resolution warnings have their own spot above.
    void NewPlayerGraphicsNote(float top_gap) {
        if ((new_player_upscaler.empty() && !new_player_shadows_reduced) ||
            (benchmark_report && benchmark_report->error.empty() && !benchmark_report->cancelled))
            return;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + top_gap);
        BeginBanner("new-player-graphics", gold, "GRAPHICS PICKED FOR YOUR CARD");
        if (!new_player_upscaler.empty())
            ImGui::TextWrapped("%s is on for your %s, for a clean, sharp image.",
                               new_player_upscaler.c_str(), system.gpu.c_str());
        if (new_player_shadows_reduced)
            ImGui::TextWrapped("Far shadows are set to Reduced for your %s. "
                               "Change this in Picture > Performance.", system.gpu.c_str());
        ImGui::TextWrapped("Run a PERF SCAN to fit your settings to your PC: in 3-6 minutes it "
                           "finds the smoothest settings, and nothing changes until you press "
                           "Apply.");
        ImGui::Dummy({0, S(4)});
        if (AccentButton("RUN PERF SCAN"))
            open_perf_scan = true;
        ImGui::SameLine();
        if (Button("Later", {0, S(36)})) {
            new_player_upscaler.clear();
            new_player_shadows_reduced = false;
        }
        EndBanner();
    }
    void SetGraphics(const GraphicsFields &fields) {
        const auto saved = VR::Ini::ValueOf(ini, "graphics_config");
        Edit("graphics_config", !saved && fields == DefaultGraphics
                                    ? std::nullopt : std::optional<std::string>(GraphicsText(fields)));
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
    void DetectBenchmarkHeadset() {
        if(Working() || running) return;
        benchmark_headset.reset();
        headset_error.clear();
        const auto runtime=RuntimePath();
        benchmark_cancel=false;
        headset_task=std::async(std::launch::async,[this,runtime] { return Benchmark::ScanHeadset(runtime,&benchmark_cancel); });
    }
    void BenchmarkPanel(bool sidebar = false) {
        const bool clicked = sidebar
            ? Button("PERF SCAN", {S(158), S(37)})
            : Button("PERF SCAN");
        if(clicked) open_perf_scan=true;
        Details("Finds smooth settings for your PC in 3-6 minutes.");
    }
    void BenchmarkWindow() {
        if(open_perf_scan) {
            open_perf_scan=false;
            ImGui::OpenPopup("PERF SCAN");
            if(!preview && !benchmark_report && !benchmark_task.valid()) DetectBenchmarkHeadset();
        }
        const auto display=ImGui::GetIO().DisplaySize;
        const float width = std::min(S(benchmark_report ? 780 : 640),display.x-S(40));
        // Leave room for the launcher's frame and center clasp even on compact windows.
        const float max_height = display.y-S(88);
        ImGui::SetNextWindowPos({display.x*.5f,display.y*.5f},ImGuiCond_Always,{.5f,.5f});
        ImGui::SetNextWindowSize({width,0});
        ImGui::SetNextWindowSizeConstraints({width,0},{width,max_height});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(26), S(24)});
        const bool shown = ImGui::BeginPopupModal("PERF SCAN", nullptr,
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        if (!shown)
            return;

        auto* window = ImGui::GetCurrentWindow();
        HunterTheme::ScanCabinet(window,theme_art,S(1));
        const auto header = ImGui::GetCursorScreenPos();
        const float header_width = ImGui::GetContentRegionAvail().x;
        const float header_height = S(43);
        const float title_size = S(29);
        constexpr const char* title = "Perf scan";
        const float title_width = heading_font->CalcTextSizeA(title_size,FLT_MAX,0,title).x;
        const float center = header.x+header_width*.5f;
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddText(heading_font,title_size,{center-title_width*.5f,header.y},ImGui::GetColorU32(ImGuiCol_Text),title);
        HunterTheme::Divider(draw,{center-S(109),header.y+S(37)},S(218),S(1));
        ImGui::SetCursorScreenPos({header.x+header_width-S(33),header.y});
        if (Button("×##close-scan",{S(33),S(29)},HunterTheme::ButtonFace::Utility))
            ImGui::CloseCurrentPopup();
        Details("Close PERF SCAN");
        ImGui::SetCursorScreenPos(header);
        ImGui::Dummy({header_width,header_height});

        // Scroll the report inside the frame. The heading and Close stay in the outer window.
        const float footer = ImGui::GetFrameHeight() + 2.f*ImGui::GetStyle().ItemSpacing.y;
        const float body_max = max_height - header_height - ImGui::GetStyle().ItemSpacing.y
                             - 2.f*window->WindowPadding.y - footer;
        ImGui::SetNextWindowSizeConstraints({0,0}, {FLT_MAX,body_max});
        if (ImGui::BeginChild("scan-body", {-1,0},
                ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                ImGuiWindowFlags_NoBackground)) {
            Hint(ScanSuggestionInUse() ? "These settings are saved. The game uses them the next time you play."
                 : benchmark_report ? "Review the measured settings. Nothing changes until you press Apply."
                                    : "Finds smooth settings in 3-6 minutes. Nothing changes until you press Apply.");
            ImGui::Separator();
            if (benchmark_apply_task.valid()) {
                Hint(close_after_benchmark_apply ? "Applying settings. The launcher will close when they are saved."
                                                 : "Verifying hardware and headset before applying settings...");
                if (Button("Cancel apply")) benchmark_cancel = true;
            }
            if (!benchmark_report || Header("Headset and scan setup"))
                BenchmarkSetup();
            BenchmarkResults();
            BenchmarkLog();
        }
        ImGui::EndChild();
        ImGui::Separator();
        if(Button(benchmark_task.valid() ? "Close (scan continues)" : "Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    void BenchmarkSetup() {
        if(headset_task.valid()) {
            ImGui::TextUnformatted("Detecting VR headset...");
            if(Button("Cancel detection")) benchmark_cancel=true;
        }
        else if(benchmark_headset) {
            const auto& h=*benchmark_headset;
            ImGui::PushFont(brand_font);
            ImGui::TextColored(gold,"Headset: %s",h.name.c_str());
            ImGui::PopFont();
            if(h.refresh_hz>0) ImGui::Text("FPS cap (active headset refresh): %.0f FPS",h.refresh_hz);
            else Hint("Headset refresh rate unknown: aiming for 90 FPS.");
            ImGui::Text("Headset-requested resolution per eye: left %u x %u, right %u x %u",h.width[0],h.height[0],h.width[1],h.height[1]);
        } else {
            ImGui::TextUnformatted("No headset detected");
            Hint("Connect your headset and set its OpenXR runtime, then retry.");
            if(!headset_error.empty()) Hint(headset_error.c_str());
        }
        if(!benchmark_task.valid()) {
            ImGui::BeginDisabled(Working() || running);
            if(Button("Detect headset again")) DetectBenchmarkHeadset();
            ImGui::EndDisabled();
        }
        if(running) Hint("Stop the game before detecting the headset or running a scan.");
        if (sky.task.valid() || weapons.task.valid())
            Hint("Finish or cancel the texture upgrade before running a scan.");
        const bool available=ScanFilesPresent();
        if(!available) Hint("This build has no scan files.");
        if(!edits.empty()) Hint("Save or discard your edits first.");
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
                const int percent=int(std::clamp<long long>(scan_seconds*100/Benchmark::QuickScanHardSeconds,0,99));
                const auto label=std::to_string(percent)+"% of 6-minute limit";
                ImGui::ProgressBar(float(percent)/100,{ -1,S(20)},label.c_str());
                ImGui::Text("Elapsed: %lld:%02lld",static_cast<long long>(elapsed/60),static_cast<long long>(elapsed%60));
                if(benchmark_cancel) Hint("Cancelling and stopping the test game...");
                else if(!started) Hint("Preparing scan (6 minutes at most).");
                else {
                    const auto remaining=std::max<long long>(0,Benchmark::QuickScanHardSeconds-scan_seconds);
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
                Hint("Tests more settings. Can finish early.");
            }
            Hint("Controls are off during the scan; it plays by itself.");
            if(Button("Cancel scan")) benchmark_cancel=true;
        } else {
            if(benchmark_headset) {
                ImGui::BeginDisabled(Busy());
                Checkbox("Use headset refresh rate (90 FPS fallback)",&benchmark_auto_hz);
                if(!benchmark_auto_hz) { ImGui::SetNextItemWidth(S(180)); ImGui::SliderInt("Target FPS",&benchmark_hz,60,144); }
                if(Header("Advanced scan options"))
                    Checkbox("Thorough scan (30 minutes maximum)",&benchmark_thorough);
                ImGui::EndDisabled();
            }
            if(benchmark_headset) {
                ImGui::BeginDisabled(preview || Working() || running || !available || !edits.empty());
                HunterTheme::PushAccent();
                if(Button(benchmark_report ? "Re-run PERF SCAN" : "RUN PERF SCAN",{S(176),S(38)})) StartScan();
                ImGui::PopStyleColor(4);
                ImGui::EndDisabled();
            }
        }
    }
    fs::path ScanPackage() const { return ExecutableDirectory()/"benchmark"; }
    bool ScanFilesPresent() const { return fs::is_regular_file(ScanPackage()/"manifest.json"); }
    // Why a scan cannot start now (the setup's button and the applied suggestion's), nullptr when
    // it can.
    const char* ScanBlocked() const {
        if(running) return "Close the game to scan again.";
        if(Working()) return "Wait for the current launcher task to finish.";
        if(!edits.empty()) return "Save or discard your setting changes to scan again.";
        if(!benchmark_headset) return "No headset detected. Open Headset and scan setup to detect it.";
        if(!ScanFilesPresent()) return "This build has no scan files.";
        return nullptr;
    }
    void StartScan() {
        if(preview || ScanBlocked()) return;
        Try([&] {
            RequireStopped();
            Benchmark::Request request{profile,paths,ScanPackage(),*benchmark_headset,FullSystem(),benchmark_auto_hz?0:benchmark_hz,150};
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
    }
    // The scan report's suggestion (recommended, else provisional); nullptr without one.
    const Benchmark::Result* ScanSuggestion() const {
        if(!benchmark_report || benchmark_report->cancelled || !benchmark_report->error.empty()) return nullptr;
        const auto& r=*benchmark_report;
        const auto at=r.recommended ? r.recommended : r.provisional;
        return at ? &r.results[*at] : nullptr;
    }
    // Whether the saved settings already hold it: Apply would change nothing.
    bool ScanSuggestionInUse() const {
        const auto* s=ScanSuggestion();
        return s && Benchmark::SettingsInUse(profile,ini,s->candidate,benchmark_report->target_hz);
    }
    // A tick in the launcher's ready green (the Play page's ready light), a line of `font` tall,
    // before the heading on its right.
    void Tick(ImFont* font) {
        const float h=font->FontSize;
        const auto p=ImGui::GetCursorScreenPos();
        const ImVec2 points[]{{p.x+h*.12f,p.y+h*.54f},{p.x+h*.38f,p.y+h*.78f},{p.x+h*.86f,p.y+h*.24f}};
        ImGui::GetWindowDrawList()->AddPolyline(points,3,ImGui::GetColorU32(ready_green),ImDrawFlags_None,S(2.2f));
        ImGui::Dummy({h,h});
        ImGui::SameLine(0,S(8));
    }
    // What a scan suggestion set, one setting a row: its name, what it was where that changed, and
    // its value now, the changed ones in the tick's green. Nothing changed (already in use): the
    // names and values alone.
    void AppliedSettingsTable(const std::vector<Benchmark::AppliedSetting>& rows) {
        constexpr const char* arrow="\xE2\x86\x92"; // from Segoe UI Symbol, merged into the text font
        bool changed=false;
        float name_width=0,before_width=0;
        ImGui::PushFont(sidebar_tag_font);
        const float heading_width=ImGui::CalcTextSize("BEFORE").x;
        ImGui::PopFont();
        for(const auto& r:rows) {
            changed|=!r.before.empty();
            name_width=std::max(name_width,ImGui::CalcTextSize(r.name.c_str()).x);
            before_width=std::max(before_width,ImGui::CalcTextSize(r.before.c_str()).x);
        }
        const auto heading=[&](const char* text) {
            ImGui::PushFont(sidebar_tag_font);
            ImGui::TextColored(muted,"%s",text);
            ImGui::PopFont();
        };
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,{S(6),S(4)});
        if(ImGui::BeginTable("scan-applied",changed?4:2,ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_NoPadOuterX)) {
            ImGui::TableSetupColumn("Setting",ImGuiTableColumnFlags_WidthFixed,name_width+S(28));
            if(changed) {
                ImGui::TableSetupColumn("Before",ImGuiTableColumnFlags_WidthFixed,std::max(before_width,heading_width)+S(10));
                ImGui::TableSetupColumn("Arrow",ImGuiTableColumnFlags_WidthFixed,ImGui::CalcTextSize(arrow).x+S(10));
            }
            ImGui::TableSetupColumn("Now",ImGuiTableColumnFlags_WidthStretch);
            if(changed) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                heading("BEFORE");
                ImGui::TableSetColumnIndex(3);
                heading("NOW");
            }
            for(const auto& r:rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(muted,"%s",r.name.c_str());
                if(changed) {
                    ImGui::TableNextColumn();
                    if(!r.before.empty()) ImGui::TextColored(muted,"%s",r.before.c_str());
                    ImGui::TableNextColumn();
                    if(!r.before.empty()) ImGui::TextColored(muted,"%s",arrow);
                }
                ImGui::TableNextColumn();
                if(r.before.empty()) ImGui::TextUnformatted(r.value.c_str());
                else ImGui::TextColored(ready_green,"%s",r.value.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
    }
    void BenchmarkRecommendationDetails(const Benchmark::Result& winner, int target_hz) {
        const auto& candidate = winner.candidate;
        const int upscaler = candidate.graphics[2];
        ImGui::Text("Stereo: %s | Output scale: %d%% of headset request",
                    StereoLabel(candidate.stereo),
                    100 + candidate.graphics[1]);
        ImGui::Text("Upscaler: %s", upscaler == 1 ? "DLSS" : upscaler == 2 ? "FSR 4" :
                                  upscaler == 3 ? "FSR 3.1.5" : "Off");
        if (upscaler) {
            const int preset = candidate.graphics[upscaler == 1 ? 3 : 4];
            ImGui::Text("Preset: %s", PresetName(preset));
            if (upscaler == 1)
                ImGui::Text("DLSS model: %s", PresetDlssModel(preset));
            if (candidate.upscale_focus == Benchmark::ScanFoveatedUpscale)
                ImGui::TextUnformatted("Foveated upscale: Medium");
        }
        for (const auto& sample : winner.repeats) {
            if (!sample.valid || !sample.input_width)
                continue;
            ImGui::Text("Output per eye: %u x %u", sample.output_width, sample.output_height);
            ImGui::Text("Internal render per eye: %u x %u", sample.input_width, sample.input_height);
            if (upscaler && sample.output_width && sample.output_height) {
                ImGui::Text("Internal size: %.1f%% width / %.1f%% height of output",
                            100.0*sample.input_width/sample.output_width,
                            100.0*sample.input_height/sample.output_height);
                Hint("Rendered smaller, then upscaled.");
            } else {
                Hint("No upscaling: rendered at full size.");
            }
            Hint("Percentages are per side, not total pixels.");
            break;
        }
        ImGui::Text("Target cap: %d FPS | AA: %s", target_hz,
                    upscaler ? "temporal upscaler (yours is bypassed)" : "yours, from the Picture tab");
        ImGui::Text("Slower repeat: %.1f FPS | GPU: %.2f ms | FPS reserve: %+.1f%%",
                    winner.fps, winner.gpu_ms, 100.0*(winner.fps/target_hz-1.0));
    }
    void BenchmarkResults() {
        if(!benchmark_report)return;
        auto& report=*benchmark_report;
        const auto suggestion=report.recommended ? report.recommended : report.provisional;
        const bool provisional=!report.recommended && report.provisional.has_value();
        bool apply=false; // the suggestion box's Apply button, pressed this frame
        // The applied box's Re-run, pressed this frame: the scan starts after this report is drawn
        // (StartScan frees the report that `report`, `winner` and `c` refer to).
        bool rerun=false;
        if(!report.cancelled && report.error.empty())
            ImGui::ProgressBar(1.0f,{-1,S(20)},"100% - Scan complete");
        if(!report.error.empty())ImGui::TextWrapped("%s",report.error.c_str());
        if(report.cancelled)Hint("Cancelled. Nothing applied.");
        ImGui::Text("Measured target: %d FPS",report.target_hz);
        if(suggestion && !report.cancelled && report.error.empty()) {
            const auto& winner=report.results[*suggestion];
            const auto& c=winner.candidate;
            // Once the saved settings hold it, the box says so and lists them (2026-11-25, the user:
            // after Apply the window only asked to apply again). Applied from this report: what each
            // was before; a match without that (or after a later change) is already in use.
            const bool in_use=Benchmark::SettingsInUse(profile,ini,c,report.target_hz);
            if(!in_use) benchmark_applied.reset();
            const bool applied_here=in_use && benchmark_applied &&
                                    benchmark_applied->folder==report.folder && benchmark_applied->id==c.id;
            // The suggestion in a gold box, its summary in the heading font, so it is the first thing
            // seen (2026-10-25, the user: the results easier to see, the Apply button louder).
            ImGui::Dummy({0,S(6)});
            ImGui::PushStyleColor(ImGuiCol_ChildBg,{.084f,.073f,.058f,1});
            ImGui::PushStyleColor(ImGuiCol_Border,in_use?ImVec4{.36f,.42f,.29f,1}:ImVec4{.42f,.35f,.25f,1});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{S(14),S(12)});
            ImGui::BeginChild("scan-suggestion",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x,S(9)});
            if(in_use) {
                ImGui::PushFont(brand_font);
                Tick(brand_font);
                ImGui::TextColored(ready_green,"%s",applied_here?"SETTINGS APPLIED":"ALREADY IN USE");
                ImGui::PopFont();
            } else
                ImGui::TextColored(gold,provisional?"PROVISIONAL SETTINGS - NOT YET CONFIRMED":winner.meets_target?"RECOMMENDED SETTINGS":"LIMITED HEADROOM - FALLBACK ONLY");
            {
                std::string summary = StereoLabel(c.stereo);
                if(c.graphics[2]) summary+=std::string(", ")+(c.graphics[2]==1?"DLSS":c.graphics[2]==2?"FSR 4":"FSR 3.1.5")+" "+PresetName(c.graphics[c.graphics[2]==1?3:4]);
                summary+=", "+std::to_string(100+c.graphics[1])+"% output";
                ImGui::PushFont(heading_font);
                ImGui::TextWrapped("%s",summary.c_str());
                ImGui::PopFont();
                ImGui::Text("%.1f FPS on a %d FPS target (%+.1f%% reserve)",winner.fps,report.target_hz,100.0*(winner.fps/report.target_hz-1.0));
                // The scan measured the player's own picture (Benchmark::PictureSettings).
                Hint(report.picture==Benchmark::PictureSettings(ini)
                         ? "Measured with your Picture settings: shadows, fog, ambient occlusion and the rest."
                         : "Your Picture settings changed since this scan. Re-run it to match them.");
            }
            if(in_use) {
                AppliedSettingsTable(Benchmark::AppliedSettings(applied_here ? benchmark_applied->before : profile,
                                                                applied_here ? benchmark_applied->before_ini : ini,
                                                                c,report.target_hz));
                // What next: scan again, or see them among the launcher's settings. The preview
                // shows the look, never scans.
                const char* blocked=preview ? nullptr : ScanBlocked();
                ImGui::BeginDisabled(blocked!=nullptr);
                if(Button("Re-run PERF SCAN",{S(176),S(38)})) rerun=true;
                ImGui::EndDisabled();
                ImGui::SameLine(0,S(10));
                if(Button("Open Graphics settings",{S(210),S(38)})) {
                    page=1;
                    OpenTab("Graphics");
                    ImGui::CloseCurrentPopup();
                }
                if(blocked) Hint(blocked);
            } else {
                // Apply right under the summary, crimson and larger, as the other buttons that are the
                // one thing to press (the details follow). The preview shows the look, never applies.
                const bool can_apply=!(preview || Working() || running || !edits.empty());
                const bool loud=can_apply || preview;
                ImGui::BeginDisabled(!can_apply);
                if(loud) {
                    HunterTheme::PushAccent();
                }
                apply=Button(provisional?"APPLY PROVISIONAL SETTINGS":winner.meets_target?"APPLY RECOMMENDATION":"APPLY LIMITED-HEADROOM FALLBACK",
                    loud?ImVec2{S(340),S(44)}:ImVec2{0,0});
                if(loud) ImGui::PopStyleColor(4);
                ImGui::EndDisabled();
                if(!can_apply && running) Hint("Close the game to apply.");
                else if(!can_apply && !edits.empty()) Hint("Save or discard your setting changes to apply.");
            }
            if(!winner.meets_target) Hint("Not confirmed smooth. Try a longer scan or a lower refresh rate.");
            if(provisional) Hint(in_use ? "One run passed. Re-run the scan to confirm it."
                                        : "One run passed. Run again to confirm, or try it now.");
            if (Header("Render and timing details")) {
                BenchmarkRecommendationDetails(winner,report.target_hz);
            }
            ImGui::PopStyleVar();
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
        }
        if(!suggestion && !report.cancelled && report.error.empty()) {
            ImGui::TextColored(gold,"NO CONFIRMED RECOMMENDATION");
            Hint("Excluded runs are shown but not used.");
            for(const auto& r:report.results) if(!r.reason.empty()) ImGui::TextWrapped("%s: %s",r.candidate.label.c_str(),r.reason.c_str());
        }
        if(suggestion && !report.cancelled && report.error.empty()) {
            const auto& c=report.results[*suggestion].candidate;
            if(apply) Try([&] {
                RequireStopped();
                const auto hardware=Json::parse(Read(report.folder/"hardware.json"));
                if (hardware.value("intel_sfx_workaround", Json{}) !=
                    Json(profile.intel_sfx_workaround))
                    throw std::runtime_error("Effects protection differs from this scan or was "
                                             "not recorded. Run a fresh benchmark.");
                const auto runtime=RuntimePath(), game=Path(profile.game);
                PendingBenchmarkApply pending{c,hardware.at("target_hz").get<int>(),ini,Read(paths.Profile()),
                                              report.folder,profile};
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
        } else if(!report.cancelled && report.error.empty() && !report.results.empty())
            Hint("Nothing applied. Excluded runs don't mean your PC is too slow.");
        ImGui::TextUnformatted("FPS REPORT AND TESTED SETTINGS");
        if(ImGui::BeginTable("benchmark_results",5,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Configuration",ImGuiTableColumnFlags_WidthStretch,2);ImGui::TableSetupColumn("Game FPS");ImGui::TableSetupColumn("Frame p99");ImGui::TableSetupColumn("Game GPU memory");ImGui::TableSetupColumn("Result",ImGuiTableColumnFlags_WidthStretch,2);ImGui::TableHeadersRow();
            for(auto& r:report.results) {
                ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextWrapped("%s",r.candidate.label.c_str());
                ImGui::TextWrapped("%s | output %d%% | %s",StereoLabel(r.candidate.stereo),100+r.candidate.graphics[1],r.candidate.graphics[2]==1?"DLSS":r.candidate.graphics[2]==3?"FSR 3.1.5":r.candidate.graphics[2]==2?"FSR 4":"Upscaler off");
                if(r.candidate.graphics[2]) {
                    const int preset=r.candidate.graphics[r.candidate.graphics[2]==1?3:4];
                    ImGui::Text("%s",PresetName(preset));
                    if(r.candidate.graphics[2]==1) ImGui::Text("Model %s",PresetDlssModel(preset));
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
        if(report.time_limited) Hint("Time limit reached: single-run results are unconfirmed.");
        Hint("Best confirmed option of the settings tested.");
        Hint("Measured in a test scene: busy areas can be slower. Check the look in VR.");
        if(!report.folder.empty() && Button("Open benchmark report"))OpenPath(report.folder);
        if(rerun) StartScan(); // last: nothing of the old report is read after this
    }
    // A launcher-drawn row of the Graphics or Extras tab under `group`: whether the search (if
    // any) finds it by these words, its group and its tab.
    const char *tab_now{};
    bool Finds(const char *group, std::string words) {
        Group(group);
        return needle.empty() ||
               SearchFinds(needle, Lower(words + " " + (group ? group : "") + " " +
                                         (tab_now ? tab_now : "")));
    }
    // Graphics: the in-game Upscaling tab on graphics_config (vr_page.h graphics rows and
    // GraphicsDraftStore::Set; the page's read-only headset resolution is left out), DLSS 5 in the
    // Upscaler row with the player's own file below it, the stereo and frame-rate launch options,
    // then the Performance rows (Settings, drawn after this).
    void GraphicsTab() {
        if (needle.empty() ||
            SearchFinds(needle, "perf scan benchmark test find smooth settings fps performance")) {
            Headings();
            BenchmarkPanel();
            ImGui::SameLine();
            Hint("Finds smooth settings for your PC in 3-6 minutes.");
        }
        const char *stereo_group = "Stereo & frame rate";
        if (Finds(stereo_group, "stereo rendering simplified rendering dibr native stereo weaker graphics cards one view "
                                "both eyes render"))
            Row("Stereo rendering", "Applies next launch.",
                "Native stereo renders each eye. Simplified Rendering uses one view for both eyes, "
                "reducing GPU load with possible edge artifacts. Auto uses Simplified Rendering on "
                "AMD RX 400, RX 500 and RX 5000 cards and when the driver lacks multiview.",
                [&] {
                    const char *labels[] = {StartupStereo(system) == StereoPreset::Dibr
                                                ? "Auto (Simplified Rendering)"
                                                : "Auto (native stereo)",
                                            StereoLabel(StereoPreset::Dibr), StereoLabel(StereoPreset::Native)};
                    const int selected = static_cast<int>(profile.stereo);
                    if (ImGui::BeginCombo("##stereo", labels[selected])) {
                        for (int i = 0; i < 3; ++i) {
                            if (ImGui::Selectable(labels[i], selected == i)) {
                                Try([&] {
                                    auto updated = profile;
                                    updated.stereo = static_cast<StereoChoice>(i);
                                    updated.Save(paths);
                                    profile.stereo = updated.stereo;
                                    status = "Stereo saved. Applies next launch.";
                                });
                            }
                        }
                        ImGui::EndCombo();
                    }
                });
        if (Finds(stereo_group, "native stereo eyes multiview layers amd white right eye driver"))
            Row("Native stereo eyes", "Applies next launch.",
                "How native stereo draws the second eye. Multiview: the graphics driver repeats each draw. Layers: the game draws each eye itself; this fixes a white right eye on AMD RX 6000 cards. Auto uses layers on AMD RX 400 to RX 6000 cards and multiview on all other cards.",
                [&] {
                    const char *labels[] = {system.auto_layers ? "Auto (layers: this AMD card)"
                                                               : "Auto (multiview)",
                                            "Multiview", "Layers"};
                    const int selected = static_cast<int>(profile.native_views);
                    if (ImGui::BeginCombo("##native-views", labels[selected])) {
                        for (int i = 0; i < 3; ++i) {
                            if (ImGui::Selectable(labels[i], selected == i)) {
                                Try([&] {
                                    auto updated = profile;
                                    updated.native_views = static_cast<NativeViews>(i);
                                    updated.Save(paths);
                                    profile.native_views = updated.native_views;
                                    status = "Native stereo eyes saved. Applies next launch.";
                                });
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (profile.SelectedStereo(system) == StereoPreset::Dibr)
                        Hint("Only used with native stereo.");
                });
        if (Finds(stereo_group, "game frame-rate cap frame rate fps hz refresh rate follow headset "
                                "72 80 90 120 144"))
            Row("Game frame-rate cap", "Applies next launch.",
                "Follow headset uses the headset's refresh rate (90 if unknown). Manual sets a fixed cap. Applying a PERF SCAN sets it too.", [&] {
                    bool follow=profile.follow_headset;
                    if(Checkbox("Follow headset",&follow)) Try([&] {
                        RequireStopped();
                        auto updated=profile; updated.follow_headset=follow;
                        updated.Save(paths); profile=std::move(updated);
                        frame_rate_editing=false;
                        status=follow ? "Frame rate follows the headset next launch."
                                      : "Manual frame-rate mode saved.";
                    });
                    if(profile.follow_headset) {
                        Hint("The headset's rate, or 90 FPS if unknown.");
                        return;
                    }
                    if(!frame_rate_editing) frame_rate_draft=profile.refresh_hz;
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderInt("##game-fps", &frame_rate_draft, MinGameRefreshHz, MaxGameRefreshHz,
                                     "%d FPS", ImGuiSliderFlags_AlwaysClamp);
                    const bool commit=ImGui::IsItemDeactivatedAfterEdit();
                    frame_rate_editing=ImGui::IsItemActive();
                    const bool reset=Button("Reset to 90 FPS", {-1, 0});
                    const int chosen=reset ? 90 : frame_rate_draft;
                    if((commit || reset) && chosen!=profile.refresh_hz) Try([&] {
                        RequireStopped();
                        auto updated=profile; updated.refresh_hz=chosen;
                        updated.Save(paths); profile=std::move(updated);
                        status="Frame-rate cap saved.";
                    });
                });
        const bool dibr = profile.SelectedStereo(system) == StereoPreset::Dibr;
        auto f = PendingGraphics();
        const bool dlss_ok = Available().dlss;
        const bool fsr4_ok = Available().fsr4;
        const bool neural = f[2] == 1 && f[5] == 1;
        const bool rtx50 = system.nvidia && VR::Graphics::Rtx50(0x10de, system.gpu);
        const bool dlss5_included = Available().dlss5;
        const bool dlss5_busy = dlss5_task.valid();
        const bool dlss5_file = dlss5_added && dlss5_added->ok;
        const char *upscale_group = "Resolution & upscaling";
        if (Finds(upscale_group, "scaling resolution supersampling render size sharp sharper per "
                                 "eye panel headset graphics memory vram percent"))
            Row("Scaling", "Output resolution per eye; based on the last VR launch.",
                "Higher is sharper and slower. Launch in VR again after changing headset or runtime resolution.",
                [&] {
                    std::string label = Signed(f[1], "%");
                    if (const auto *h = KnownHeadset()) {
                        const auto output = EyeSizes(*h, f).first;
                        label += " | " + std::to_string(output.width) + " x " +
                                 std::to_string(output.height);
                    }
                    if (StepSlider(f[1], -50, 50, 10, label)) {
                        SetGraphics(f);
                    }
                },
                [&] {
                    // The per-eye size (the headset's request from the game's last start), red
                    // when high; nothing before the first launch.
                    const auto *h = KnownHeadset();
                    if (!h) {
                        ImGui::TextDisabled("Launch in VR to see the per-eye resolution.");
                        return;
                    }
                    const auto [output, render] = EyeSizes(*h, f);
                    const bool high = output.width > HighEyeWidth;
                    ImGui::TextColored(high ? high_red : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                                       "%u x %u per eye", output.width, output.height);
                    if (render.width != output.width) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(renders %u x %u)", render.width, render.height);
                    }
                    if (high)
                        ImGui::TextColored(high_red, "High resolution. Scale it down if it's too slow.");
                    // The headset's own panel, the suggested size: the runtime's request includes
                    // its supersampling.
                    HeadsetCombo(*h);
                    if (const auto *panel = PanelHeadset(*h)) {
                        ImGui::TextDisabled("Panel: %u x %u per eye", panel->width, panel->height);
                        if (const auto scale = PanelScale(*h); scale && *scale != f[1]) {
                            ImGui::SameLine();
                            const std::string label = "Use panel size (" + Signed(*scale, " %") + ")";
                            if (SmallButton(label.c_str())) {
                                f[1] = *scale;
                                SetGraphics(f);
                            }
                        }
                    }
                    if (const double vram = VramMb(*h, f); vram && system.vram) {
                        const bool fits = VramFits(vram);
                        ImGui::TextColored(fits ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                                                : high_red,
                                           "Estimated graphics memory: %.1f of %.0f GB%s", vram / 1024,
                                           double(system.vram >> 20) / 1024,
                                           fits ? "" : ". May not fit.");
                        Details("Rough estimate from short RTX 5080 runs. Actual use varies by GPU, "
                                "area and session length.");
                    }
                });
        const char *no_dlss = !system.nvidia ? "DLSS needs an NVIDIA RTX graphics card."
                                             : "This package has no DLSS files.";
        // DLSS 5 (2026-10-27, the user: one setting, at full strength): NVIDIA's nvngx_dlssnr.dll
        // is never shipped, so the player adds a copy (found on the PC or picked) below.
        const bool dlss5_ok = rtx50 && dlss5_included && dlss5_file && !dibr;
        const char *no_dlss5 = dibr             ? "DLSS 5 needs native stereo."
                               : !rtx50          ? "DLSS 5 needs a GeForce RTX 50-series card."
                               : !dlss5_included ? "This package has no DLSS 5 bridge."
                                                 : "Add your DLSS 5 file first (below).";
        // FSR 4.1 (2026-10-31): AMD's own, on AMD cards only (Upscalers::fsr41).
        const char *no_fsr41 = !system.amd ? "FSR 4.1.2 is AMD's: Radeon cards only."
                                           : "FSR 4.1.2 runtime files are missing.";
        if (Finds(upscale_group, "upscaler upscaling dlss fsr fsr 4 fsr 4.1 fsr 4.1.2 fsr 3.1.5 dlss 5 neural "
                                 "nvidia amd radeon render smaller off"))
            Row("Upscaler", "Render smaller, then upscale.",
                "DLSS needs an NVIDIA RTX card. FSR works on AMD and NVIDIA. Newer isn't always "
                "faster. FSR 4.1.2 (AMD cards, experimental): AMD's own FSR Upscaling 4.1.2 on Radeon "
                "RX 6000 and newer; where AMD offers no 4.1.2 (an older Radeon), the game runs FSR "
                "3.1.5 instead. DLSS 5 (experimental): NVIDIA's neural lighting and detail at full "
                "strength, replacing DLSS and FSR; it needs a GeForce RTX 50-series card, native "
                "stereo and your own DLSS 5 file, and applies after a restart.",
                [&] {
                    const char *selected = neural    ? "DLSS 5"
                                           : f[2] == 1 ? "DLSS"
                                           : f[2] == 2 ? "FSR 4"
                                           : f[2] == 3 ? "FSR 3.1.5"
                                           : f[2] == 4 ? "FSR 4.1.2"
                                                       : "Off";
                    // The in-game Upscaler row's order (vr_page.h graphics_upscaler).
                    const int pick = Combo(
                        selected,
                        {{"Off", f[2] == 0, true, ""},
                         {"DLSS", f[2] == 1 && !neural, dlss_ok, no_dlss},
                         {"FSR 4", f[2] == 2, fsr4_ok, "FSR 4 runtime files are missing."},
                         {"FSR 4.1.2", f[2] == 4, Available().fsr41, no_fsr41},
                         {"FSR 3.1.5", f[2] == 3, Available().fsr31,
                          "FSR 3.1.5 runtime files are missing."},
                         {"DLSS 5", neural, dlss5_ok, no_dlss5}});
                    if (pick == 5) {
                        // graphics_settings.h: Balanced, every strength at 100 (vr_page.h Set).
                        f[2] = 1;
                        f[5] = 1;
                        f[6] = 1;
                        f[7] = f[8] = f[9] = 100;
                        SetGraphics(f);
                    } else if (pick >= 0) {
                        // The picks' providers: Off 0, DLSS 1, FSR 4 2, FSR 4.1 4, FSR 3.1.5 3.
                        constexpr int providers[]{0, 1, 2, 4, 3};
                        f[2] = providers[pick];
                        f[5] = 0;
                        SetGraphics(f);
                    }
                    // AMD (2026-10-25, GitHub #9): FSR 4 is its 8-bit model in compute shaders,
                    // which even an RTX 5080 spent more on than it saved (native 8.76 ms, FSR
                    // 3.1.5 Quality 7.38, FSR 4 Quality 10.1 per frame), and an RX 9070 XT showed
                    // the HUD doubled with FSR 4 but not with FSR 3.1.5. FSR 4.1, AMD's own, has
                    // not been tried on an AMD card here yet (2026-10-31).
                    if (system.amd)
                        Hint("AMD: FSR 3.1.5 Quality or Balanced is the tested choice. FSR 4.1.2 is "
                             "AMD's own (RX 6000 and newer, experimental). FSR 4 is heavier and can "
                             "show a doubled HUD. Simplified Rendering helps on weaker cards.");
                    if (dibr)
                        Hint("DLSS and FSR upscale the single view in Simplified Rendering. "
                             "DLSS 5 needs native stereo.");
                });
        // FSR 4.1 uses the FSR preset field, as FSR 4 and FSR 3.1.5 do (graphics_settings.h IsFsr).
        const bool dlss = f[2] == 1 && !f[5], fsr4 = f[2] == 2, fsr31 = f[2] == 3, fsr41 = f[2] == 4;
        if (Finds(upscale_group, "quality preset ultra performance balanced plus quality ultra "
                                 "quality render size dlss fsr"))
            Row("Quality preset", "Render size before upscaling.",
                "Ultra Performance 40%, Performance 50%, Balanced 58%, Balanced Plus 58% with DLSS's best model (62% with FSR), Quality 67%, Ultra Quality 67% with DLSS's best model (75% with FSR). DLSS 5 has its own: Balanced 58%.",
                [&] {
                    // Listed fastest first; the saved values keep their ids (3 = Ultra
                    // Performance, 5 = Balanced Plus).
                    constexpr int ids[]{3, 0, 1, 5, 2, 4};
                    const int current = (fsr4 || fsr31 || fsr41) ? f[4] : f[3];
                    const int shown = int(std::find(std::begin(ids), std::end(ids), current) - std::begin(ids)) % 6;
                    // DLSS 5 renders at its own profile (graphics_config field 6), never this one.
                    const char *now = neural ? (f[6] == 0 ? "DLSS 5: Performance 50%"
                                                          : "DLSS 5: Balanced 58%")
                                             : PresetName(ids[shown]);
                    ImGui::BeginDisabled(!dlss && !fsr4 && !fsr31 && !fsr41);
                    const int pick = Combo(now,
                                           {{PresetName(ids[0]), shown == 0, true, ""},
                                            {PresetName(ids[1]), shown == 1, true, ""},
                                            {PresetName(ids[2]), shown == 2, true, ""},
                                            {PresetName(ids[3]), shown == 3, true, ""},
                                            {PresetName(ids[4]), shown == 4, true, ""},
                                            {PresetName(ids[5]), shown == 5, true, ""}});
                    ImGui::EndDisabled();
                    if (pick >= 0) {
                        f[(fsr4 || fsr31 || fsr41) ? 4 : 3] = ids[pick];
                        SetGraphics(f);
                    }
                });
        // The player's DLSS 5 file, on every card as the DLSS 5 tab had it (it runs on RTX 50).
        const auto user = paths.User();
        const char *dlss5_group = "DLSS 5 (experimental)";
        std::string state =
            dlss5_busy   ? "Checking..."
            : dlss5_file ? "NVIDIA DLSS 5 " + dlss5_added->version + ", signed by NVIDIA."
                         : "Not added. NVIDIA's file is not included: a game with DLSS 5 has it.";
        if (!rtx50)
            state += " DLSS 5 runs on GeForce RTX 50-series cards.";
        if (Finds(dlss5_group, "dlss 5 file nvngx_dlssnr find on this pc neural add search"))
            Row("DLSS 5 file", state.c_str(),
                "Searches Steam, Epic and Xbox game folders and NVIDIA's driver downloads, and "
                "adds the newest signed copy. Only NVIDIA's signed, unmodified file is accepted.",
                [&] {
                    ImGui::BeginDisabled(dlss5_busy);
                    if (Button(dlss5_busy ? "Working..." : "Find on this PC", {-1, 0}))
                        Dlss5Work([user, runtime = RuntimePath()] {
                            const auto found = FindDlss5(runtime);
                            if (found.empty())
                                return std::string("No DLSS 5 file found on this PC.");
                            const auto &best = found.front();
                            if (!best.check.ok)
                                return std::to_string(found.size()) +
                                       " found; none is NVIDIA's signed file. " + best.check.problem;
                            const auto mine = VR::Dlss5File::Verify(VR::Dlss5File::InUser(user));
                            if (mine.ok && !VR::Dlss5File::Newer(best.check.version, mine.version))
                                return "Your DLSS 5 file is already the newest found (" + mine.version + ").";
                            AddDlss5(Paths{user.parent_path()}, best.file);
                            return "Added DLSS 5 " + best.check.version + " from " + PathText(best.file) + ".";
                        });
                    ImGui::EndDisabled();
                });
        if (Finds(dlss5_group, "dlss 5 file nvngx_dlssnr choose browse pick"))
            Row("Choose the file", "Pick nvngx_dlssnr.dll yourself.",
                "It is copied to the launcher's data folder; the original stays where it is.", [&] {
                    ImGui::BeginDisabled(dlss5_busy);
                    if (Button("Browse", {-1, 0}))
                        Try([&] {
                            if (auto file = PickPath(window_handle, false, L"Choose nvngx_dlssnr.dll",
                                                     L"nvngx_dlssnr.dll;*.dll"))
                                Dlss5Work([user, file = *file] {
                                    const auto added = AddDlss5(Paths{user.parent_path()}, file);
                                    return "Added DLSS 5 " + added.version + ".";
                                });
                        });
                    ImGui::EndDisabled();
                });
        if (dlss5_file && Finds(dlss5_group, "dlss 5 file remove delete"))
            Row("Remove the file", "DLSS 5 turns off.", "Deletes the launcher's copy only.", [&] {
                ImGui::BeginDisabled(dlss5_busy);
                if (Button("Remove", {-1, 0})) {
                    if (neural) {
                        f[2] = 0;
                        f[5] = 0;
                        SetGraphics(f);
                    }
                    Dlss5Work([user] {
                        RemoveDlss5(Paths{user.parent_path()});
                        return std::string("DLSS 5 file removed.");
                    });
                }
                ImGui::EndDisabled();
            });
        if (!dlss5_note.empty() && needle.empty())
            Hint(dlss5_note.c_str());
    }
    // Extras: the launch options outside the game's VR page (Launcher Profile), then the
    // launcher's own vr.ini rows (Settings).
    void ExtrasTab() {
        if (Finds(nullptr, "free camera fly flying sightseeing photo photo mode explore spectator"))
            Row("Free camera", "Sightseeing in VR from the start of the next launch.",
                LeftHandedNow() // left-handed play: the controllers trade jobs
                    ? "Right stick: fly. Left stick: turn / height. The trigger, a grip or the right "
                      "stick pressed: fast (two at once: faster). A: the view back to the hunter. B: "
                      "free camera off. Menu: game menu. In the game: VR page > You > Free camera. "
                      "Enemies ignore the hunter while it is on; distant areas may not load. The "
                      "world does not pause."
                    : "Left stick: fly. Right stick: turn / height. The trigger, a grip or the left "
                      "stick pressed: fast (two at once: faster). X: the view back to the hunter. Y: "
                      "free camera off. Menu: game menu. In the game: VR page > You > Free camera. "
                      "Enemies ignore the hunter while it is on; distant areas may not load. The "
                      "world does not pause.", [&] {
                    bool enabled = profile.free_camera;
                    if (Checkbox("Free camera next launch", &enabled)) Try([&] {
                        RequireStopped();
                        auto updated = profile;
                        updated.free_camera = enabled;
                        updated.Save(paths);
                        profile = std::move(updated);
                        status = enabled ? "Free camera on from the start of the next launch."
                                         : "Normal camera on the next launch.";
                    });
                });
        if (Finds(nullptr, "non-aggro non aggro mode enemies ignore me stealth peaceful passive "
                           "explore"))
            Row("Non-aggro mode", "Enemies ignore you until you attack. Applies next launch.",
                "Kyo's Player Stealth patch: enemies do not see or react to you unless you hit them. "
                "Bosses may still fight. Off by default; only here, not in the game's VR page.", [&] {
                    bool enabled = profile.non_aggro;
                    if (Checkbox("Enemies ignore me", &enabled)) Try([&] {
                        RequireStopped();
                        auto updated = profile;
                        updated.non_aggro = enabled;
                        updated.Save(paths);
                        profile = std::move(updated);
                        status = enabled ? "Non-aggro mode on from the next launch."
                                         : "Non-aggro mode off from the next launch.";
                    });
                });
        if (Finds(nullptr, "effects crash protection skip dangerous sfx intel 12th gen crash "
                           "particles"))
            Row("Effects crash protection", "Recommended. Applies next launch.",
                "Skips effect code that crashes on Intel 12th Gen and newer. Some effects may be missing; turning it off can bring the crashes back.", [&] {
                    bool enabled = profile.intel_sfx_workaround;
                    if (Checkbox("Skip dangerous SFX", &enabled)) Try([&] {
                        RequireStopped();
                        auto updated = profile;
                        updated.intel_sfx_workaround = enabled;
                        updated.Save(paths);
                        profile = std::move(updated);
                        status = enabled ? "Dangerous SFX skipped on the next launch."
                                         : "SFX protection off. Effect crashes may return.";
                    });
                });
    }
    // Sharper skies and sharper weapons (sky_upscale.h): the game's sky, moon and cloud
    // textures, and its weapons' (2x), upscaled by the bundled Real-ESRGAN, once, from the
    // player's own game folder; the game uses them while sky_detail is on (its default), else and
    // without them its own textures (never a filtered copy). One job at a time. Launcher only: the
    // in-game VR page has no row for them.
    void DetailRows(DetailJob &job, const char *label, const char *idle, const char *help,
                    const char *removed) {
        const bool busy = job.task.valid();
        const bool other = (&job == &sky ? weapons : sky).task.valid();
        const bool tool = Skies::ToolIncluded(Skies::ToolFolder());
        const bool made = job.state.images > 0;
        const bool old = made && job.state.recipe != Skies::RecipeFor(job.kind);
        const auto value = Value("sky_detail");
        const bool off = value && *value == "0";
        std::string state =
            busy   ? job.progress.Text()
            : old  ? "Made with an older version: make them again for the newest look."
            : made ? "Made: " + std::to_string(job.state.images) + " textures (" +
                         std::to_string(job.state.bytes >> 20) + " MB)."
                   : idle;
        if (off && made && !busy)
            state += " Sky detail is off in vr.ini, so they are not used.";
        Row(label, state.c_str(), help, [&] {
            if (busy) {
                ImGui::ProgressBar(job.progress.fraction, {-1, 0}, "");
                // The settings list is disabled while a launch or update runs, not this.
                if (Button("Cancel", {-1, 0}))
                    job.progress.cancel = true;
                return;
            }
            // Made: Remove under it, in plain sight (2026-10-30, the user; side by side the
            // narrow column cut both words).
            ImGui::BeginDisabled(!tool || running || Working() || game_folder_needed);
            if (Button(made && !old ? "Check again" : made ? "Make again" : "Make",
                              {-1, 0}))
                Try([&] {
                    RequireStopped();
                    const auto game = Path(profile.game);
                    job.progress.cancel = false;
                    job.progress.fraction = 0;
                    job.progress.Set("Starting");
                    job.note.clear();
                    job.task = std::async(std::launch::async, [this, game, &job] {
                        return Skies::Make(game, paths.User(), Skies::ToolFolder(), job.progress,
                                           job.kind);
                    });
                });
            ImGui::EndDisabled();
            if (!tool)
                Details("This package is missing the texture upscaler.");
            else if (game_folder_needed)
                Details("Choose a supported game folder first.");
            else if (other)
                Details("Wait for the other texture job to finish.");
            else if (Busy())
                Details("Wait for the current launcher task to finish.");
            if (!made)
                return;
            ImGui::BeginDisabled(running || Working());
            if (Button("Remove", {-1, 0}))
                Try([&] {
                    RequireStopped();
                    Skies::Remove(paths.User(), job.kind);
                    job.state = Skies::Look(paths.User(), job.kind);
                    job.note = removed;
                });
            ImGui::EndDisabled();
        }, [] {}, true);
        if (!job.note.empty() && !busy)
            Hint(job.note.c_str());
    }
    // Left-handed play as the file has it (ControlsGuide::LeftHanded): the tooltips, the Move with
    // choices and the notes for the other side.
    bool LeftHandedNow() {
        return ControlsGuide::LeftHanded([this](const std::string &key) { return Value(key); });
    }
    // A row's label and help as the game words them: the buttons of the Controls page's headset
    // and of these settings ("Grip adjust (A+B)").
    ControlsGuide::RowText Words(const Setting &row) {
        if (!row.id)
            return {row.label, row.help};
        const ControlsGuide::Lookup value = [this](const std::string &key) { return Value(key); };
        auto words = ControlsGuide::RowWords(
            row.id, static_cast<ControlsGuide::Platform>(controls_platform), value);
        if (words.label.empty())
            return {row.label, row.help};
        return words;
    }
    // What the search looks through: the tab, the heading, the words, the tooltip, the choices,
    // the vr.ini keys (as written and as words) and the row's own search words.
    static std::string SearchText(const Setting &row, const ControlsGuide::RowText &words) {
        std::string t = std::string(row.category) + " " + (row.group ? row.group : "") + " " +
                        words.label + " " + words.help + " " + row.details + " " + row.words;
        for (const auto &o : row.options)
            t += std::string(" ") + o.label;
        for (const auto &key : row.keys) {
            auto spaced = key;
            std::replace(spaced.begin(), spaced.end(), '_', ' ');
            t += " " + key + " " + spaced;
        }
        return Lower(t);
    }
    void NumberRow(const Setting &row, const ControlsGuide::RowText &words) {
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
        const bool style_locked = LockedByPlayStyle(row);
        if (style_locked)
            text = PlayStyleLockedText;
        const std::string sided = LeftHandedNow() ? LeftHandedDetails(row) : std::string{};
        Row(words.label.c_str(), words.help.c_str(), sided.empty() ? row.details : sided.c_str(), [&] {
            ImGui::BeginDisabled(style_locked);
            const int min = row.zero ? row.min - 1 : row.min;
            int slider = zero ? min : shown;
            if (StepSlider(slider, min, row.max, row.zero && slider < row.min ? 1 : row.step, text))
                Edit(key, row.zero && slider < row.min ? "0" : NumberValue(row, slider));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                Edit(key, std::nullopt);
            ImGui::EndDisabled();
            Details(style_locked ? "The gamepad play style sets this while it plays."
                                 : "Right-click to restore the game default.");
        });
    }
    void SettingRow(const Setting &row, const ControlsGuide::RowText &words) {
        if (row.keys.front() == "dynamic_resolution") {
            const auto& key = row.keys.front();
            const auto& on = row.options[0];
            const auto& off = row.options[1];
            const bool enabled = !SameValue(Value(key).value_or("1"), off.values[0]);
            Row(words.label.c_str(), words.help.c_str(), row.details, [&] {
                const int pick = Combo(enabled ? on.label : off.label,
                                       {{on.label, enabled, true, ""}, {off.label, !enabled, true, ""}});
                if (pick >= 0)
                    Edit(key, row.options[size_t(pick)].values[0]);
            });
            return;
        }
        if (row.Number()) {
            NumberRow(row, words);
            return;
        }
        std::vector<std::optional<std::string>> values, effective, defaults;
        const auto vram = VR::CardMemoryBytes();
        bool any = false, defaults_known = true, by_card = false;
        for (const auto &key : row.keys) {
            values.push_back(ChoiceOverride(key, Value(key)));
            // Light attack stamina as the game reads the lines (StaminaPctAsRead): an old
            // melee_stamina=1 without a melee_stamina_pct line it reads is the full cost; a line it
            // refuses is not a value.
            if (key == "melee_stamina_pct")
                values.back() = StaminaPctAsRead(LinesOf(key), LinesOf("melee_stamina"));
            any |= bool(values.back());
            // A key the file lacks counts as its game default where that is known (a row that
            // sets two keys, one of them set by an older launcher's separate row).
            const auto card_default = CardDefault(key, vram);
            by_card |= card_default.has_value();
            defaults.push_back(card_default ? card_default : CodeDefault(key));
            defaults_known &= bool(defaults.back());
            effective.push_back(values.back() ? values.back() : defaults.back());
        }
        // The first option these values pick, -1 for none. Keep matches any value: the page's
        // snap turns leave the smooth speed as it is.
        const auto match = [&](const std::vector<std::optional<std::string>> &now) {
            for (size_t i = 0; i < row.options.size(); ++i) {
                bool same = true;
                for (size_t j = 0; j < now.size(); ++j)
                    same &= row.options[i].values[j] == Keep ||
                            (now[j] && SameValue(*now[j], row.options[i].values[j]));
                if (same)
                    return int(i);
            }
            return -1;
        };
        // 0: the game default (nothing in the file), i + 1: option i, -1: Custom.
        const int picked = any ? match(effective) : -1;
        const int selected = !any ? 0 : picked >= 0 ? picked + 1 : -1;
        const int default_option = defaults_known ? match(defaults) : -1;
        // Left-handed play: Move with names your thumb (the controllers trade hands); every other
        // row (and right-handed play) shows its own words, nothing made per frame.
        const bool sided_names = row.id && std::string_view(row.id) == "move_with" && LeftHandedNow();
        std::vector<std::string> sided_labels;
        if (sided_names)
            for (const auto &o : row.options)
                sided_labels.push_back(VR::Mirror::SideWords(o.label));
        const auto shown = [&](size_t i) {
            return sided_names ? sided_labels[i].c_str() : row.options[i].label;
        };
        std::string game_default = by_card ? "Auto" : "Game default";
        if (default_option >= 0)
            game_default += std::string(" (") + shown(size_t(default_option)) + ")";
        std::string custom = "Custom";
        for (const auto &v : values)
            custom += " / " + v.value_or("default");
        const auto& key = row.keys[0];
        const bool style_locked = LockedByPlayStyle(row);
        const bool fog_locked = key == "moon_shafts" && FogEnabled(Value("fog_banks"), vram);
        bool locked = style_locked || fog_locked;
        if (!locked && (key == "aa_mode" || key == "upscale_focus")) {
            const auto graphics = PendingGraphics();
            const auto stereo = profile.SelectedStereo(system);
            locked = key == "aa_mode"
                ? UpscalerBypassesAa(graphics, stereo)
                : !FoveatedUpscaleAvailable(graphics, stereo);
        }
        const std::string sided = LeftHandedNow() ? LeftHandedDetails(row) : std::string{};
        Row(words.label.c_str(), words.help.c_str(), sided.empty() ? row.details : sided.c_str(), [&] {
            // An ini-only choice shows when the file holds it but is not offered (vr_page.h).
            std::vector<Pick> picks{{game_default.c_str(), selected == 0, true, ""}};
            std::vector<size_t> offered;
            for (size_t i = 0; i < row.options.size(); ++i)
                if (!row.options[i].ini_only) {
                    picks.push_back({shown(i), selected == int(i) + 1, true, ""});
                    offered.push_back(i);
                }
            ImGui::BeginDisabled(locked);
            const int pick = Combo(fog_locked     ? VR::CardTier::MoonGlowRequired
                                   : style_locked ? PlayStyleLockedText
                                   : selected < 0 ? custom.c_str()
                                   : selected == 0
                                       ? game_default.c_str()
                                       : shown(size_t(selected - 1)),
                                   picks);
            ImGui::EndDisabled();
            if (style_locked)
                Details("The gamepad play style sets this while it plays.");
            if (pick == 0)
                for (const auto &key : row.keys)
                    Edit(key, std::nullopt);
            else if (pick > 0) {
                const auto &option = row.options[offered[size_t(pick - 1)]];
                for (size_t j = 0; j < row.keys.size(); ++j)
                    if (option.values[j] != Keep)
                        Edit(row.keys[j], option.values[j]);
            }
        });
    }
    void CategoryDefaults(const std::string &name) {
        // Anti-aliasing is left alone while an upscaler bypasses it, as the page does (DIBR
        // runs every upscaler but DLSS 5).
        const auto pending = PendingGraphics();
        const bool aa_locked = UpscalerBypassesAa(pending, profile.SelectedStereo(system));
        // Rows a gamepad play style sets are left alone too: they hold the player's motion values,
        // which come back as they were on Motion.
        for (const auto &row : Settings())
            if (name == row.category && !(aa_locked && row.keys[0] == "aa_mode") &&
                !LockedByPlayStyle(row))
                for (const auto &key : row.keys)
                    Edit(key, std::nullopt);
        if (name == "Graphics") {
            // Output scale back to the headset's panel (0 when it is not known), the upscaler off
            // and DLSS 5's own fields at their defaults.
            auto f = PendingGraphics();
            const auto *h = KnownHeadset();
            f[1] = (h ? PanelScale(*h) : std::nullopt).value_or(0);
            f[2] = 0;
            f[3] = f[4] = 1;
            f[5] = 0;
            for (const int i : {6, 7, 8, 9})
                f[i] = DefaultGraphics[i];
            SetGraphics(f);
        }
        status = edits.empty() ? "Already at the game defaults."
                               : "Unsaved settings. Save or launch to apply.";
    }
    // Search results beyond the settings list: where the launcher's other things are.
    void ElsewhereResults() {
        struct Place {
            const char *label, *where, *words;
            int page; // the page to open; -1: the save backups window
        };
        static constexpr Place places[]{
            {"Sharper skies", "On the Play page, at the bottom.",
             "sharper skies sky moon clouds upscale ai real-esrgan textures", 0},
            {"Sharper weapons", "On the Play page, at the bottom.",
             "sharper weapons weapon textures upscale ai real-esrgan 2x", 0},
            {"Save backups", "Back up or restore your saves (the sidebar's Save backups).",
             "save saves backup backups restore savedata", -1},
            {"Controls", "Every control for Quest, PSVR and Frame, and how to play.",
             "controls buttons bindings quest psvr ps vr2 frame how to play gestures", 3},
            {"Mod library", "Import a ZIP, review conflicts or switch installed mods.",
             "mods mod library install import zip conflict enable disable remove", 4},
            {"Updates", "New versions of Bldbrne-VR.", "updates update version download release",
             2}};
        pending_tab = nullptr;
        pending_group = shown_group = nullptr;
        for (const auto &place : places) {
            if (!SearchFinds(needle, Lower(std::string(place.label) + " " + place.words)))
                continue;
            Group("Elsewhere in the launcher");
            Row(place.label, place.where, "", [&] {
                if (Button(place.page < 0 ? "Open" : "Go there", {-1, 0})) {
                    if (place.page < 0)
                        show_backups = true;
                    else
                        page = place.page;
                    search.clear();
                }
            });
        }
    }
    int found_last{};      // rows the search found last frame
    float preview_scroll{-1}, settings_scroll_max{}; // --preview: the settings list's scroll
    bool preview_play_bottom{}; // --preview with BBVR_PREVIEW_PLAY_BOTTOM: the Play page scrolled down
    void PageStatus() const {
        if (status != FirstLaunchNote && status != "Ready when you are.")
            Hint(status.c_str());
    }
    void SettingsPage() {
        Title("P R E F E R E N C E S", "Your way through the night.");
        Hint("The in-game VR menu's tabs, then Graphics and Extras. Applies next launch.");
        PageStatus();
        ImGui::Dummy({0, S(2)});
        const auto &categories = Categories();
        category = std::clamp(category, 0, int(categories.size()) - 1);
        // Wrap at narrow widths so the plaques keep their padding.
        const float available = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {S(12), S(9)});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {S(6), S(7)});
        float x = 0;
        for (int i = 0; i < int(categories.size()); ++i) {
            const float width =
                ImGui::CalcTextSize(categories[i]).x + ImGui::GetStyle().FramePadding.x * 2;
            if (i && x + ImGui::GetStyle().ItemSpacing.x + width <= available) {
                ImGui::SameLine();
                x += ImGui::GetStyle().ItemSpacing.x + width;
            } else
                x = width;
            const bool active = i == category && search.empty();
            if (Button(categories[i],{},HunterTheme::ButtonFace::Utility,active))
                OpenTab(categories[i]);
        }
        ImGui::PopStyleVar(2);
        needle = Lower(search);
        if (needle.find_first_not_of(' ') == std::string::npos)
            needle.clear();
        ImGui::SetNextItemWidth(needle.empty() ? -1 : -S(150));
        if (TextField("##search", search,
                "Search all settings: a name, a choice, or a word like boss, fps, vial, turn...")) {
            linked_setting = nullptr;
            scroll_to_linked_setting = false;
        }
        if (!needle.empty()) {
            ImGui::SameLine();
            const std::string count = found_last == 1 ? "1 found" : std::to_string(found_last) + " found";
            ImGui::TextDisabled("%s", count.c_str());
            ImGui::SameLine();
            if (Button("Clear", {}, HunterTheme::ButtonFace::Utility))
                search.clear();
        }
        const bool settings_locked = running || Busy();
        InsetList("settings-list",-S(100),theme_art);
        // The child creates its scrollbar on entry. Disable the controls inside it.
        ImGui::BeginDisabled(settings_locked);
        if (preview_scroll >= 0)
            ImGui::SetScrollY(preview_scroll);
        found = 0;
        for (const auto *name : categories) {
            if (needle.empty() && name != categories[category])
                continue;
            ImGui::PushID(name);
            tab_now = name;
            pending_group = shown_group = nullptr;
            pending_tab = needle.empty() ? nullptr : name;
            const std::string_view tab = name;
            if (tab == "Graphics")
                GraphicsTab();
            else if (tab == "Extras")
                ExtrasTab();
            for (const auto &row : Settings()) {
                if (tab != row.category)
                    continue;
                Group(row.group);
                const auto words = Words(row);
                if (!needle.empty() && !SearchFinds(needle, SearchText(row, words)))
                    continue;
                const bool linked = &row == linked_setting;
                if (linked && scroll_to_linked_setting) {
                    const float row_y = ImGui::GetCursorScreenPos().y - ImGui::GetWindowPos().y;
                    ImGui::SetScrollFromPosY(row_y, .16f);
                    scroll_to_linked_setting = false;
                }
                if (linked) ImGui::PushStyleColor(ImGuiCol_Text, gold);
                SettingRow(row, words);
                if (linked) ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        if (!needle.empty())
            ElsewhereResults();
        pending_tab = pending_group = shown_group = tab_now = nullptr;
        found_last = needle.empty() ? 0 : found;
        if (!needle.empty() && !found)
            Hint("No matches. Try one word, such as boss, fps, vial, turn or shadow.");
        settings_scroll_max = ImGui::GetScrollMaxY();
        ImGui::EndDisabled();
        EndInsetList();
        ImGui::BeginDisabled(settings_locked);
        const bool can_save = !edits.empty();
        ImGui::BeginDisabled(!can_save);
        if (can_save) HunterTheme::PushAccent();
        if (Button("Save settings",{}))
            Try([&] { SaveIni(); });
        if (can_save) ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (Button("Reload")) {
            if (edits.empty())
                Try([&] {
                    Reload();
                    status = "Settings reloaded.";
                });
            else
                ImGui::OpenPopup("Discard changes?");
        }
        ImGui::SameLine();
        if (Button("Import vr.ini"))
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
        if (Button("Reset defaults"))
            ImGui::OpenPopup("Restore defaults?");
        Details("Restore defaults for all VR settings.");
        ImGui::SameLine();
        const auto category_label = std::string(categories[category]) + " defaults";
        if (Button(category_label.c_str()))
            CategoryDefaults(categories[category]);
        Details("Restore the game defaults for this tab. Save to apply. Launch options stay as they are.");
        ImGui::EndDisabled();
        Hint("Reset restores every VR option.");
        if (Modal("Discard changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Discard unsaved VR settings and reload the file?");
            if (Button("Discard")) {
                Try([&] { Reload(); });
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Button("Keep editing"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (Modal("Restore defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(
                "Reset all VR options, including calibration and custom entries?");
            ImGui::TextUnformatted("A backup is kept. Saves and paths stay.");
            if (Button("Restore defaults")) {
                Try([&] {
                    RequireStopped();
                    Write(paths.Ini(), "", ini);
                    Reload();
                    status = "Defaults restored (backup kept).";
                });
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    // Controls (controls_guide.h, 2026-10-30, the user): every control of the chosen headset as
    // the game's Show VR controls page words it, with the player's VR settings (unsaved edits
    // included); the last tab, VR interactions, the guide to using things with the hands (in the
    // names of the controller tab picked last); one search over the tab shown.
    int controls_platform = 0;
    bool controls_guide = false; // the VR interactions tab is shown
    bool controls_diagram_menus = false;
    ControlsGuide::ControllerArtCache controller_art;
    ControlsGuide::DiagramSelection controls_selection;
    int controls_select = -1; // a tab to select on the next frame (PlatformCount: the guide)
    std::string controls_search;
    int controls_found = 0, controls_found_last = 0;
    // The launcher's row for a VR page row id (its tab and words), nullptr when it has none.
    const Setting *SettingOf(const char *row) {
        if (!row)
            return nullptr;
        for (const auto &s : Settings())
            if (s.id && std::string_view(s.id) == row)
                return &s;
        return nullptr;
    }
    // A section's lines that have the search's words (all of them without a search); how many.
    int ControlsSection(const ControlsGuide::Section &section, bool controls,
                        const std::string &needle) {
        std::vector<const ControlsGuide::Line *> shown;
        for (const auto &line : section.lines)
            if (needle.empty() || ControlsGuide::Finds(line, needle))
                shown.push_back(&line);
        if (shown.empty())
            return 0;
        ImGui::Dummy({0, S(10)});
        ImGui::TextColored(gold, "%s", section.title.c_str());
        ImGui::Separator();
        if (!section.note.empty() && needle.empty())
            Hint(section.note.c_str());
        if (!ImGui::BeginTable(section.title.c_str(), 2, ImGuiTableFlags_SizingFixedFit))
            return int(shown.size());
        ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed, S(190));
        ImGui::TableSetupColumn("does", ImGuiTableColumnFlags_WidthStretch);
        for (const auto *shown_line : shown) {
            const auto &line = *shown_line;
            // The guide is rebuilt every frame, so its row addresses aren't stable IDs.
            ImGui::PushID(int(shown_line - section.lines.data()));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            // A control's name; a gesture or a how-to: its title in the name's place.
            const std::string &left = controls && !line.control.empty() ? line.control : line.title;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(180));
            ImGui::TextColored(gold, "%s", left.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TableSetColumnIndex(1);
            ImGui::PushTextWrapPos(0);
            if (controls && !line.control.empty()) {
                if (line.sub.empty())
                    ImGui::TextUnformatted(line.title.c_str());
                else
                    ImGui::Text("%s  (%s)", line.title.c_str(), line.sub.c_str());
            } else if (!line.sub.empty()) {
                ImGui::TextUnformatted(line.sub.c_str());
            }
            if (!line.help.empty())
                ImGui::TextColored(muted, "%s", line.help.c_str());
            ImGui::PopTextWrapPos();
            // The setting that changes it: its tab and name, a click away.
            if (const Setting *setting = SettingOf(line.row)) {
                const std::string where = std::string("Setting: ") + setting->category + " > " +
                                          Words(*setting).label;
                if (SmallButton(where.c_str())) {
                    page = 1;
                    OpenTab(setting->category);
                    linked_setting = setting;
                    scroll_to_linked_setting = true;
                    repaint = 3;
                }
            }
            ImGui::Dummy({0, S(2)});
            ImGui::PopID();
        }
        ImGui::EndTable();
        return int(shown.size());
    }
    void ControlsPage() {
        Title("C O N T R O L S", "Your hands in Yharnam.");
        PageStatus();
        ImGui::Dummy({0, S(6)});
        constexpr const char* guide_tab = "VR interactions";
        const float gap = S(6);
        float tab_width = ImGui::CalcTextSize(guide_tab).x + gap*ControlsGuide::PlatformCount;
        for (const char* name : ControlsGuide::PlatformNames)
            tab_width += ImGui::CalcTextSize(name).x;
        // Compress horizontal padding before hiding any controller name.
        const float pad = std::clamp((ImGui::GetContentRegionAvail().x-tab_width-S(4)) /
                                     (2.f*(ControlsGuide::PlatformCount+1)), S(10), S(16));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {std::floor(pad), S(11)});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, {gap, ImGui::GetStyle().ItemInnerSpacing.y});
        if (ImGui::BeginTabBar("controls-platform", ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (int p = 0; p <= ControlsGuide::PlatformCount; ++p) {
                const bool guide = p == ControlsGuide::PlatformCount;
                const ImGuiTabItemFlags flags =
                    controls_select == p ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (HunterTheme::Tab(guide ? guide_tab : ControlsGuide::PlatformNames[p],
                                     flags,theme_art,S(1))) {
                    controls_guide = guide;
                    if (!guide)
                        controls_platform = p;
                    ImGui::EndTabItem();
                }
            }
            controls_select = -1;
            ImGui::EndTabBar();
        }
        ImGui::PopStyleVar(2);
        std::string needle = Lower(controls_search);
        if (needle.find_first_not_of(' ') == std::string::npos)
            needle.clear();
        ImGui::SetNextItemWidth(needle.empty() ? -1 : -S(150));
        TextField("##controls-search", controls_search,
                  controls_guide
                      ? "Search the guide: an item, a weapon, or a word like visceral, "
                        "lantern, corpse, unarmed..."
                      : "Search the controls: a button, or a word like dodge, menu, vial, sprint...");
        if (!needle.empty()) {
            ImGui::SameLine();
            const std::string count = controls_found_last == 1
                                          ? "1 found"
                                          : std::to_string(controls_found_last) + " found";
            ImGui::TextDisabled("%s", count.c_str());
            ImGui::SameLine();
            if (Button("Clear", {}, HunterTheme::ButtonFace::Utility))
                controls_search.clear();
        }
        const auto platform = static_cast<ControlsGuide::Platform>(controls_platform);
        const ControlsGuide::Lookup value = [this](const std::string &key) { return Value(key); };
        ImGui::BeginChild("controls-list", {0, 0});
        controls_found = 0;
        ImGui::PushTextWrapPos(0);
        if (needle.empty()) {
            if (controls_guide)
                Hint(("Buttons named for the " + std::string(ControlsGuide::PlatformNames[controls_platform]) +
                      " controllers: pick another controller's tab to rename them.").c_str());
        }
        ImGui::PopTextWrapPos();
        if (!controls_guide && needle.empty() && controls_platform < 4) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {S(16), S(8)});
            for (int mode = 0; mode < 2; ++mode) {
                if (mode) ImGui::SameLine();
                const bool active = controls_diagram_menus == (mode == 1);
                if (Button(mode ? "In menus##diagram" : "While playing##diagram",{},
                           HunterTheme::ButtonFace::Utility,active))
                    controls_diagram_menus = mode == 1;
            }
            ImGui::PopStyleVar();
            Hint("Select a control to highlight it and read its full mapping.");
            ControlsGuide::DrawDiagram(platform,
                ControlsGuide::Diagram(platform, value, controls_diagram_menus, xr_runtime),
                S(1), gold, controller_art.Get(device.Get(), controls_platform), controls_selection, theme_art);
        }
        if (!controls_guide && needle.empty() && Header("Controller and runtime notes")) {
            ImGui::PushTextWrapPos(0);
            Hint(ControlsGuide::PlatformNote(platform, LeftHandedNow()).c_str());
            ImGui::PopTextWrapPos();
        }
        for (const auto &section : controls_guide ? ControlsGuide::Interactions(platform, value, xr_runtime)
                                                  : ControlsGuide::Controls(platform, value, xr_runtime))
            controls_found += ControlsSection(section, !controls_guide, needle);
        if (!needle.empty() && controls_found == 0) {
            ImGui::Dummy({0, S(10)});
            Hint("No matches on this tab.");
        }
        // A search on a controller's tab that the guide answers: one click to it.
        if (!needle.empty() && !controls_guide) {
            int in_guide = 0;
            for (const auto &section : ControlsGuide::Interactions(platform, value, xr_runtime))
                for (const auto &line : section.lines)
                    in_guide += ControlsGuide::Finds(line, needle) ? 1 : 0;
            if (in_guide > 0) {
                ImGui::Dummy({0, S(6)});
                const std::string more = std::to_string(in_guide) + " found in VR interactions";
                if (Button(more.c_str()))
                    controls_select = ControlsGuide::PlatformCount;
            }
        }
        controls_found_last = controls_found;
        ImGui::Dummy({0, S(12)});
        ImGui::EndChild();
    }
    void UpdatesPage() {
        Title("T H E   L A T E S T   B U I L D", "Keep the hunt going.");
        Hint("Check when you want. Install when you are ready.");
        PageStatus();
        ImGui::Dummy({0, S(10)});
        ImGui::Text("Installed version  %s", BBVR_VERSION);
        ImGui::TextWrapped("%s", update_status.c_str());
        if (available)
            ImGui::TextColored(gold, "Version %s  /  %.1f MiB",
                               ReleaseVersion::Display(available->TargetFullRelease.Version).c_str(),
                               double(available->TargetFullRelease.Size) / (1024 * 1024));
        if (Checkbox("Check for updates at startup", &profile.check_updates)) {
            profile.check_updates_chosen = true; // the player's own choice from now on
            Try([&] { profile.Save(paths); });
        }
        ImGui::Dummy({0, S(6)});
        const ImVec2 update_action{0,S(36)};
        ImGui::BeginDisabled(Busy());
        const bool check = !available && !Busy() ? UpdateButton("Check for updates",update_action)
                                                  : Button("Check for updates",update_action);
        if (check && !preview)
            Check();
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool can_download = !Busy() && available && !downloaded;
        ImGui::BeginDisabled(!can_download);
        const bool download = can_download ? UpdateButton("Download update",update_action)
                                           : Button("Download update",update_action);
        if (download)
            Download();
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool installable = !Working() && available && downloaded && !running;
        ImGui::BeginDisabled(!installable);
        const bool install = installable ? UpdateButton("Install & restart",{S(190),update_action.y})
                                         : Button("Install & restart",{S(190),update_action.y});
        if (install && !preview)
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
                         : sky.task.valid() || weapons.task.valid()
                             ? "Finish or cancel the texture upgrade before installing this update."
                         : "Ready. The launcher restarts to install.");
        ImGui::Dummy({0, S(12)});
        ImGui::Separator();
        ImGui::PushFont(brand_font);
        ImGui::TextColored(gold, "Release notes");
        ImGui::PopFont();
        InsetList("release-notes",S(150),theme_art);
        const char *notes = "Release notes appear here after checking.";
        if (available)
            notes = available->TargetFullRelease.NotesMarkdown.empty()
                        ? "No release notes were provided for this version."
                        : available->TargetFullRelease.NotesMarkdown.c_str();
        // Older releases used the original name. Update the displayed prose only.
        static const std::regex old_rendering_name(R"(\bDIBR\b)");
        const auto display_notes = std::regex_replace(notes, old_rendering_name,
                                                     StereoLabel(StereoPreset::Dibr));
        ImGui::TextWrapped("%s", display_notes.c_str());
        EndInsetList();
        Hint("Updates keep your settings and saves. Play works offline.");
        if (HunterTheme::Link("View releases"))
            Try([&] { OpenPath(L"https://github.com/bldbrne-vr/bldbrne-vr/releases"); });
    }
    static ModResult ReviewMod(const fs::path& game, const Mods::Package& pkg, std::string note = {}) {
        ModResult result{pkg, {}, std::move(note)};
        // Keep Disable and Remove reachable even if the imported data cannot be scanned.
        try { result.report = Mods::Scan(game, pkg); }
        catch (const std::exception& e) { result.report.conflicts.emplace_back(e.what()); }
        return result;
    }
    enum class ModAccess { Read, Change };
    template<class F> void StartModTask(std::string note, F action, ModAccess access = ModAccess::Change) {
        if (preview) throw std::runtime_error("Open the regular launcher to change mods.");
        if (game_folder_needed) throw std::runtime_error("On Play, choose your Bloodborne game folder, then open Mods.");
        if (Working() || pending_game_launch)
            throw std::runtime_error("Wait for the current launcher operation to finish, then try again.");
        if (access == ModAccess::Change) RequireStopped();
        mod_task = std::async(std::launch::async, std::move(action));
        mod_note = std::move(note);
        confirm_mod_removal = false;
    }
    void ImportMod(const fs::path& zip) {
        if (game_folder_needed || build_checked.empty())
            throw std::runtime_error("On Play, choose your Bloodborne game folder, then open Mods to import the ZIP.");
        const auto game = Mods::GameRoot(build_checked);
        StartModTask("Importing " + PathText(zip.filename()) + "...", [game, zip] {
            return ReviewMod(game, Mods::Import(game, zip), "Imported and disabled. Review the files before enabling.");
        });
        mod_game = game;
        mod_selection.reset();
    }
    void ModsPage() {
        Title(nullptr, "Mod library");
        if (game_folder_needed) {
            Hint("Choose your Bloodborne game folder on Play to see its installed mods.");
            if (Button("Open Play", {}, HunterTheme::ButtonFace::Gilt)) page = 0;
            return;
        }
        const auto game = Mods::GameRoot(build_checked);
        if (game != mod_game) {
            mod_game = game;
            mod_refresh = true;
            mod_selection.reset();
            mod_note.clear();
        }
        if (mod_refresh) {
            mod_refresh = false;
            mods.clear();
            mod_list_ok = false;
            Try([&] { mods = Mods::List(game); mod_list_ok = true; });
        }
        const bool compact = ImGui::GetIO().DisplaySize.y < S(700);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {S(10), S(compact ? 6 : 8)});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {S(12), S(5)});
        const float action_height = std::max(S(36), ImGui::GetFrameHeight());
        const float review_height = std::max(S(30), ImGui::GetFrameHeight());
        const float cell_pad_y = S(4);
        const float row_content = std::max(review_height, 2 * ImGui::GetTextLineHeight());
        const float row_height = row_content + 2 * cell_pad_y;
        Hint("Drop a mod ZIP here, or choose one to import. Imported mods start disabled.");
        ImGui::BeginDisabled(Working() || pending_game_launch.has_value());
        ImGui::BeginDisabled(running);
        // Lift the label about 1.5 logical pixels to center it inside the gilt rim.
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, {0.5f, 0.35f});
        const bool import_zip = Button("Import ZIP", {S(132), action_height}, HunterTheme::ButtonFace::Gilt);
        ImGui::PopStyleVar();
        if (import_zip)
            Try([&] { if (auto file = PickPath(window_handle, false, L"Bloodborne mod ZIP", L"*.zip")) ImportMod(*file); });
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (Button("Rescan", {S(96), action_height}, HunterTheme::ButtonFace::Utility)) {
            mod_selection.reset();
            mod_note.clear();
            mod_refresh = true;
        }
        ImGui::EndDisabled();
        if (running) Hint("You can review files now. Close the game to import, switch or remove mods.");
        if (!mod_note.empty()) ImGui::TextWrapped("%s", mod_note.c_str());
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(compact ? 4 : 8));
        ImGui::PushFont(brand_font);
        ImGui::TextColored(gold, "Imported mods");
        ImGui::PopFont();
        const auto rows = std::clamp<size_t>(mods.size(), 1, 4);
        // InsetList has a 5 px rim and 4 px of inner padding on each end.
        const float list_padding = 2 * (S(5) + S(4));
        InsetList("mod-library", list_padding + rows * row_height, theme_art);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {ImGui::GetStyle().CellPadding.x, cell_pad_y});
        if (mods.empty())
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((row_height - ImGui::GetTextLineHeight()) * .5f));
        if (!mod_list_ok) Hint("The list could not be read. Check your game folder and select Rescan.");
        else if (mods.empty()) Hint("No mods imported for this game folder yet.");
        else if (ImGui::BeginTable("packages", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, S(132));
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, S(88));
            for (const auto& pkg : mods) {
                const auto name = PathText(pkg.path.stem());
                const bool selected = mod_selection && mod_selection->path == pkg.path;
                const char* state = pkg.Interrupted() ? (pkg.path.extension() == ".importing" ? "Import unfinished" : "Removal unfinished")
                                                     : pkg.enabled ? "Enabled" : "Disabled";
                ImGui::TableNextRow(0, row_height);
                if (selected) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(118,88,50,44));
                ImGui::TableSetColumnIndex(0);
                // A long filename can wrap. Center all three cells against that actual height.
                const float text_height = ImGui::CalcTextSize(name.c_str(), nullptr, false,
                    std::max(1.f, ImGui::GetContentRegionAvail().x)).y;
                const float content_height = std::max(row_content, text_height);
                const auto center_cell = [content_height](float height) {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((content_height - height) * .5f));
                };
                center_cell(text_height);
                ImGui::TextWrapped("%s", name.c_str());
                ImGui::TableSetColumnIndex(1);
                center_cell(ImGui::GetTextLineHeight());
                ImGui::TextColored(pkg.Interrupted() ? ImVec4{.96f,.70f,.50f,1} : pkg.enabled ? gold : muted, "%s", state);
                ImGui::TableSetColumnIndex(2);
                center_cell(review_height);
                ImGui::PushID(PathText(pkg.path).c_str());
                ImGui::BeginDisabled(Working() || pending_game_launch.has_value());
                if (Button("Review", {S(82), review_height}, HunterTheme::ButtonFace::Utility, selected)) Try([&] {
                    StartModTask("Checking " + name + "...", [game,pkg] { return ReviewMod(game,pkg); }, ModAccess::Read);
                    mod_selection.reset();
                });
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
        EndInsetList();
        if (mod_selection) {
            const auto pkg = *mod_selection;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(compact ? 4 : 8));
            const auto title = PathText(pkg.path.stem());
            ImGui::PushFont(brand_font);
            ImGui::PushStyleColor(ImGuiCol_Text, gold);
            ImGui::TextWrapped("%s", title.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            if (pkg.Interrupted())
                ImGui::TextWrapped("This unfinished copy is inactive. Remove it before importing the ZIP again.");
            else {
                if (mod_report.files) {
                    if (!mod_report.has_file_list)
                        Hint("No original file list is available. Reimport the ZIP to check for missing or incomplete files.");
                    const auto overlaps = mod_report.overrides.size();
                    if (overlaps)
                        ImGui::Text("%zu game file%s  /  %zu overlap%s with existing installation files",
                                    mod_report.files, mod_report.files == 1 ? "" : "s",
                                    overlaps, overlaps == 1 ? "" : "s");
                    else if (mod_report.conflicts.empty())
                        ImGui::Text("%zu game file%s  /  No overlaps with enabled mods", mod_report.files, mod_report.files == 1 ? "" : "s");
                    else ImGui::Text("%zu game file%s", mod_report.files, mod_report.files == 1 ? "" : "s");
                }
                if (!mod_report.conflicts.empty())
                    ImGui::TextColored(ImVec4{.96f,.70f,.50f,1}, "Needs attention");
                if (!mod_report.overrides.empty())
                    ImGui::TextWrapped("This mod's files take priority while enabled. Disabling it restores the existing copies.");
                if (!mod_report.shared.empty())
                    Hint("Gameplay or item-text archives are replaced whole. Changes inside them may conflict.");
                const size_t count = mod_report.conflicts.size() + mod_report.overrides.size();
                constexpr size_t per_page = 12;
                if (count > per_page) {
                    ImGui::Text("File checks %zu-%zu of %zu", mod_file_page + 1,
                                std::min(count, mod_file_page + per_page), count);
                    ImGui::BeginDisabled(mod_file_page == 0);
                    if (Button("Previous files")) mod_file_page -= per_page;
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(mod_file_page + per_page >= count);
                    if (Button("Next files")) mod_file_page += per_page;
                    ImGui::EndDisabled();
                }
                if (mod_file_page < count) {
                    const auto visible = std::min<size_t>(count - mod_file_page, 5);
                    InsetList("mod-file-checks", list_padding + ImGui::GetTextLineHeight() +
                        (visible - 1) * ImGui::GetTextLineHeightWithSpacing(), theme_art);
                    for (size_t i = mod_file_page; i < std::min(count, mod_file_page + per_page); ++i) {
                        if (i < mod_report.conflicts.size()) ImGui::TextWrapped("%s", mod_report.conflicts[i].c_str());
                        else ImGui::TextWrapped("%s", mod_report.overrides[i - mod_report.conflicts.size()].c_str());
                    }
                    EndInsetList();
                }
            }
            ImGui::BeginDisabled(running || Working() || pending_game_launch.has_value());
            if (!pkg.Interrupted()) {
                ImGui::BeginDisabled(!pkg.enabled && !mod_report.conflicts.empty());
                const char* action = pkg.enabled ? "Disable" : mod_report.overrides.empty() ? "Enable" : "Enable with these overrides";
                if (Button(action, {0, action_height}, HunterTheme::ButtonFace::Gilt))
                    Try([&] {
                        StartModTask(pkg.enabled ? "Disabling mod..." : "Checking files before enabling...", [game,pkg,report=mod_report,title] {
                            const auto updated = Mods::Toggle(game, pkg, report);
                            return ReviewMod(game, updated, title + (pkg.enabled ? " is disabled." : " is enabled for the next launch."));
                        });
                    });
                ImGui::EndDisabled();
            }
            if (pkg.enabled) Hint("Disable the mod to remove its imported files.");
            if (!pkg.enabled) {
                if (!confirm_mod_removal) {
                    if (!pkg.Interrupted()) ImGui::SameLine();
                    if (Button(pkg.path.extension() == ".removing" ? "Retry removal" : "Remove mod",
                               {S(120), action_height}, HunterTheme::ButtonFace::Standard))
                        confirm_mod_removal = true;
                } else {
                    ImGui::TextWrapped("Delete the imported copy of %s? You can import the ZIP again.", title.c_str());
                    if (Button("Delete imported copy", {0, action_height})) {
                        Try([&] { StartModTask("Removing " + title + "...", [game,pkg,title] {
                            Mods::Remove(game, pkg);
                            return ModResult{{}, {}, "Removed the imported copy of " + title + ". Your ZIP was kept."};
                        }); });
                    }
                    ImGui::SameLine();
                    if (Button("Cancel removal", {0, action_height}, HunterTheme::ButtonFace::Utility))
                        confirm_mod_removal = false;
                }
            }
            ImGui::EndDisabled();
        }
        ImGui::PopStyleVar(2);
    }
    std::string CloseEditSummary() {
        std::vector<std::string> names;
        for (const auto& [key, value] : edits) {
            std::string name = key;
            if (key == "play_style") name = "Play style";
            else if (key == "graphics_config") name = "Scaling and upscaling";
            else {
                for (const auto& row : Settings()) {
                    if (std::find(row.keys.begin(), row.keys.end(), key) != row.keys.end()) {
                        name = Words(row).label;
                        break;
                    }
                }
            }
            if (std::find(names.begin(), names.end(), name) == names.end())
                names.push_back(std::move(name));
        }
        std::string summary;
        for (size_t i = 0; i < names.size() && i < 3; ++i) {
            if (i) summary += ", ";
            summary += names[i];
        }
        if (names.size() > 3)
            summary += " and " + std::to_string(names.size() - 3) + " more";
        return summary;
    }
    void Draw() {
        if (invalid_mod_drop) {
            invalid_mod_drop = false;
            page = 4;
            error = "Drop one mod ZIP at a time. You can also use Import ZIP.";
        }
        if (!dropped_mod.empty()) {
            const auto zip = std::move(dropped_mod);
            dropped_mod.clear();
            page = 4;
            Try([&] { ImportMod(zip); });
        }
        theme_art.Load(device.Get());
        auto size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("Bldbrne-VR", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.035f, 0.038f, 0.04f, 1});
        ImGui::BeginChild("sidebar", {S(205), 0});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {S(10),S(7)});
        auto* sidebar_draw = ImGui::GetWindowDrawList();
        const auto sidebar_pos = ImGui::GetWindowPos();
        HunterTheme::Leather(sidebar_draw, sidebar_pos,
            {sidebar_pos.x + S(205), sidebar_pos.y + size.y}, theme_art, S(1),
            IM_COL32(145,172,192,255),IM_COL32(30,34,40,40));
        HunterTheme::Mullion(sidebar_draw,{sidebar_pos.x+S(200),sidebar_pos.y+S(12)},
                            size.y-S(24),theme_art,S(1));
        ImGui::PushFont(logo_font);
        const float brand_shift = size.y/S(1) < 700 ? 0.f : 16.f;
        // Center the artwork and controls between the outer rail and the inner mullion.
        const float sidebar_center = S(112);
        HunterTheme::Divider(sidebar_draw,{sidebar_pos.x+sidebar_center-S(39),sidebar_pos.y+S(26+brand_shift)},S(78),S(1));
        ImGui::SetCursorPos({sidebar_center-ImGui::CalcTextSize("Bldbrne-VR").x*.5f, S(33+brand_shift)});
        ImGui::TextColored(ImVec4{.85f,.80f,.68f,1}, "Bldbrne-VR");
        ImGui::PopFont();
        ImGui::PushFont(sidebar_tag_font);
        float brand_y=81+brand_shift;
        for (const char* line : {"BLOODBORNE IN", "VIRTUAL REALITY"}) {
            HunterTheme::TrackedText(sidebar_draw,
                {sidebar_pos.x+sidebar_center-HunterTheme::TrackedWidth(line,S(1.25f))*.5f,sidebar_pos.y+S(brand_y)},
                line,S(1.25f),ImGui::GetColorU32(ImVec4{.81f,.76f,.67f,1}));
            brand_y+=14;
        }
        ImGui::PopFont();
        HunterTheme::Divider(sidebar_draw,{sidebar_pos.x+sidebar_center-S(74),sidebar_pos.y+S(119+brand_shift)},S(148),S(1));
        ImGui::SetCursorPosY(S(146+brand_shift));
        // Controls is page 3 (Play 0, VR settings 1, Updates 2 keep their numbers), listed after
        // VR settings.
        const char *pages[]{"Play", edits.empty() ? "VR settings" : "VR settings *",
                            available ? "Updates (NEW)" : "Updates", "Controls", "Mods"};
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,{0.f,.5f});
        const float padding_y = ImGui::GetStyle().FramePadding.y;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{S(21),padding_y});
        ImGui::PushFont(sidebar_nav_font);
        for (const int i : {0, 1, 3, 4, 2}) {
            ImGui::SetCursorPosX(S(33));
            const bool selected = page == i;
            if (selected) HunterTheme::PushAccent();
            const bool long_label = ImGui::CalcTextSize(pages[i]).x > S(125);
            if (long_label) ImGui::PushFont(brand_font);
            if (Button(pages[i], {S(158), S(size.y < S(700) ? 30 : 37)})) {
                if (i == 4 && page != 4) { mod_refresh = true; mod_selection.reset(); }
                page = i;
            }
            if (long_label) ImGui::PopFont();
            if (selected) ImGui::PopStyleColor(4);
        }
        ImGui::SetCursorPosX(S(33));
        BenchmarkPanel(true);
        ImGui::PopFont();
        ImGui::PopStyleVar(2);
        const float utility_inset = sidebar_center-S(71);
        const ImVec2 utility_button{S(142),S(30)};
        const auto utility_face = HunterTheme::ButtonFace::Utility;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{S(10),S(10)});
        ImGui::SetCursorPos({utility_inset, size.y - S(254)});
        ImGui::PushFont(brand_font);
        if (Button("Desktop shortcut", utility_button,utility_face))
            Try([&] {
                CreatePlayShortcut();
                status = "\"Play Bldbrne-VR\" added to your desktop.";
            });
        Details("Starts the game directly from your desktop.");
        ImGui::SetCursorPosX(utility_inset);
        if (Button("Save backups", utility_button,utility_face))
            show_backups = true;
        ImGui::SetCursorPosX(utility_inset);
        if (Button("Data folder", utility_button,utility_face))
            Try([&] { OpenPath(paths.root); });
        ImGui::SetCursorPosX(utility_inset);
        if (Button("Open logs", utility_button,utility_face))
            Try([&] {
                fs::create_directories(paths.User() / "log");
                OpenPath(paths.User() / "log");
            });
        ImGui::PopFont();
        ImGui::Dummy({0, S(6)});
        ImGui::SetCursorPosX(utility_inset);
        Hint("NATIVE / WINDOWS");
        ImGui::SameLine();
        Hint(BBVR_VERSION);
        ImGui::PopStyleVar(2);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("main", {0, 0});
        const auto main_pos = ImGui::GetWindowPos();
        HunterTheme::Leather(ImGui::GetWindowDrawList(), main_pos,
            {main_pos.x + ImGui::GetWindowWidth(), main_pos.y + size.y}, theme_art, S(1),
            IM_COL32(143,167,183,255),IM_COL32(34,39,43,40));
        // The outer trim covers only the window edges. The hero can bleed beneath it.
        const bool play = page == 0;
        auto* trim = ImGui::GetForegroundDrawList();
        HunterTheme::CabinetFrame(trim,size,theme_art,S(1),play);
        const float main_width = ImGui::GetWindowWidth();
        const float content_inset = play ? std::max(S(24),(main_width-S(1080))*.5f) : S(24);
        const float content_width = main_width-2*content_inset;
        ImGui::SetCursorPos({content_inset, S(24)});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0,0,0,0});
        const bool compact = size.y < S(700);
        const float button_height = S(compact ? 55 : 60);
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float action_height = button_height + (pending_game_launch
            ? ImGui::GetTextLineHeight() + ImGui::GetFrameHeight() + 2.f*spacing : 0.f);
        const float action_gap = S(8);
        std::string footer;
        float bottom_reserve = S(24);
        if (play) {
            if (!game_folder_needed && !setup_issue.empty() && !running && !Working())
                footer = setup_issue; // Keep the reason visible when Review Setup scrolls past the status strip.
            else if (profile.game.empty() && (status == FirstLaunchNote || status == "Ready when you are."))
                footer = "Have an existing shadPS4 save? Add it under Optional setup.";
            else
                footer = status == "Ready when you are." ? "Settings and saves stay yours." : status;
            ImGui::PushFont(italic_font);
            const float text_height = ImGui::CalcTextSize(footer.c_str(), nullptr, false, content_width).y;
            ImGui::PopFont();
            const float frame_clearance = S(compact ? 26 : 32);
            bottom_reserve = action_gap + action_height + 2.f*spacing + text_height + frame_clearance;
        }
        ImGuiWindow* play_scroll = nullptr;
        ImGui::BeginChild("content", {content_width,
            ImGui::GetContentRegionAvail().y - bottom_reserve});
        if (play) {
            PlayPage(content_inset);
            ImGui::Dummy({0,S(18)});
            if (preview_play_bottom)
                ImGui::SetScrollY(ImGui::GetScrollMaxY());
            play_scroll = ImGui::GetCurrentWindow();
        } else if (page == 1)
            SettingsPage();
        else if (page == 3)
            ControlsPage();
        else if (page == 4)
            ModsPage();
        else
            UpdatesPage();
        ImGui::EndChild();
        if (play) {
            ImGui::SetCursorPosX(content_inset);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY()+action_gap);
            ImGui::BeginChild("play-action", {content_width, action_height}, 0,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            PlayButton(button_height);
            ImGui::EndChild();
            ImGui::PushFont(italic_font);
            const float text_width = ImGui::CalcTextSize(footer.c_str()).x;
            ImGui::SetCursorPosX(content_inset+std::max(0.f,(content_width-text_width)*.5f));
            if (text_width+S(172) < content_width) {
                const auto pos = ImGui::GetCursorScreenPos();
                const float center_y = pos.y+ImGui::GetFontSize()*.55f;
                auto* draw = ImGui::GetWindowDrawList();
                constexpr ImU32 ornament = IM_COL32(128,113,89,160);
                draw->AddLine({pos.x-S(86),center_y},{pos.x-S(13),center_y},ornament);
                draw->AddLine({pos.x+text_width+S(13),center_y},
                              {pos.x+text_width+S(86),center_y},ornament);
            }
            ImGui::PushTextWrapPos(content_inset+content_width);
            ImGui::TextColored({.76f,.71f,.62f,1},"%s",footer.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
        }
        ImGui::PopStyleColor();
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
            if (Button("OK", {S(100), 0})) {
                error.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (close_requested && profile_pending) {
            SaveChange(); // a folder typed into a field that was never left
            if (profile_pending) {
                // SaveProfile clears the flag only after writing successfully.
                close_after_benchmark_apply = close_requested = false;
            }
        }
        if (close_requested) {
            if (benchmark_apply_task.valid()) {
                close_after_benchmark_apply = true;
                close_requested = false;
                status = "Applying scan settings. The launcher will close when they are saved.";
            } else if (Busy()) {
                if (mod_task.valid()) {
                    page = 4;
                    mod_note = "Please wait for the mod operation to finish before closing the launcher.";
                } else status = "Please wait for the current launcher operation to finish before closing.";
                close_requested = false;
            } else if (edits.empty())
                quit = true;
            else {
                close_edit_summary = CloseEditSummary();
                ImGui::OpenPopup("Unsaved VR settings");
                close_requested = false;
            }
        }
        if (ImGui::IsPopupOpen("Unsaved VR settings")) {
            // The native window border follows the outer edge even as the text wraps or DPI changes.
            ImGui::SetNextWindowSize({S(424), 0});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(20), S(16)});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, S(1));
            ImGui::PushStyleColor(ImGuiCol_Border, {.42f, .36f, .28f, 1});
            ImGui::PushFont(brand_font);
            const bool open = ImGui::BeginPopupModal("Unsaved VR settings", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
            ImGui::PopFont();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            if (open) {
                ImGui::TextUnformatted("Save your changes before closing?");
                Hint(close_edit_summary.c_str());
                ImGui::Separator();
                if (Button("Save & close", {}, HunterTheme::ButtonFace::Gilt))
                    Try([&] {
                        SaveIni();
                        quit = true;
                    });
                ImGui::SameLine();
                if (Button("Discard & close"))
                    quit = true;
                ImGui::SameLine();
                if (Button("Keep editing"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        ImGui::End();
        HunterTheme::PaintScrollbars(theme_art, S(1));
        if (play_scroll)
            HunterTheme::PlayScrollEdge(play_scroll, theme_art, S(1));
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
        if (argc == 3 && std::wstring_view(args[1]) == L"--release-version-file") {
            const auto file = fs::absolute(args[2]);
            LocalFree(args);
            const auto version = ReleaseVersion::Package(BBVR_VERSION);
            if (!version)
                return 1;
            Write(file, Json{{"version", BBVR_VERSION}, {"package_version", *version}}.dump());
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
        // --preview draws at 100%, or at BBVR_PREVIEW_SCALE (1.5: Windows' 150%) to check a
        // scaled screen's layout.
        float preview_scale = 1;
        if (wchar_t text[16]{}; preview && GetEnvironmentVariableW(L"BBVR_PREVIEW_SCALE", text, 16))
            preview_scale = std::clamp(float(_wtof(text)), 1.0f, 3.0f);
        const auto monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        scale = preview ? preview_scale : ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
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
        const bool preview_small = preview && GetEnvironmentVariableW(L"BBVR_PREVIEW_SMALL", nullptr, 0)>0;
        int client_width = preview_small ? 844 : 1024;
        int client_height = preview_small ? 651 : 768;
        // Capture custom client sizes at the same DPI to inspect window resizing.
        if (wchar_t text[32]{}; preview && GetEnvironmentVariableW(L"BBVR_PREVIEW_SIZE",text,32)<32) {
            int width{}, height{};
            wchar_t trailing{};
            if (swscanf_s(text,L"%dx%d%c",&width,&height,&trailing,1u)==2 &&
                width>=844 && width<=2560 && height>=651 && height<=1600) {
                client_width = width;
                client_height = height;
            }
        }
        RECT r{0, 0, LONG(S(client_width)), LONG(S(client_height))};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        LONG window_width = r.right-r.left, window_height = r.bottom-r.top;
        MONITORINFO monitor_info{sizeof(monitor_info)};
        if (!preview && GetMonitorInfoW(monitor, &monitor_info)) {
            const auto& work = monitor_info.rcWork;
            window_width = std::min(window_width, work.right-work.left);
            window_height = std::min(window_height, work.bottom-work.top);
        }
        window_handle = CreateWindowW(wc.lpszClassName, L"Bldbrne-VR", WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, window_width,
                                      window_height, nullptr, nullptr, instance, nullptr);
        BOOL dark = TRUE;
        DragAcceptFiles(window_handle, TRUE);
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
        // The game's button symbols on the Controls page (the in-game Show VR controls' words:
        // ○ □ △ and arrows) from Segoe UI Symbol, merged into the text font.
        if (const auto symbols = fonts / L"seguisym.ttf"; fs::exists(symbols)) {
            static const ImWchar ranges[]{0x2190, 0x21FF, 0x25A0, 0x25FF, 0};
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.GlyphRanges = ranges;
            io.Fonts->AddFontFromFileTTF(PathText(symbols).c_str(), S(16), &merge);
        }
        heading_font = add_font(L"georgia.ttf", 36);
        brand_font = add_font(L"georgia.ttf", 17);
        logo_font = add_font(L"georgia.ttf", 34);
        display_font = add_font(L"georgiai.ttf", 56);
        sidebar_nav_font = add_font(L"georgia.ttf", 18);
        sidebar_tag_font = add_font(L"georgia.ttf", 12);
        italic_font = add_font(L"georgiai.ttf", 18);
        launch_font = add_font(L"georgia.ttf", 31);
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
        // Offline visual review only; never reads or modifies the player's real profile.
        const bool theme_preview = preview && GetEnvironmentVariableW(L"BBVR_PREVIEW_THEME", nullptr, 0)>0;
        wchar_t preview_pointer[16]{};
        if (theme_preview) {
            GetEnvironmentVariableW(L"BBVR_PREVIEW_POINTER", preview_pointer, 16);
        } else if (preview) {
            auto& release = app.available.emplace().TargetFullRelease;
            release.Version = "0.8.9";
            release.Size = 125 * 1024 * 1024;
            release.NotesMarkdown = "Sample release notes for the offline launcher preview.";
            app.update_status = "Version 0.8.9 is available.";
            app.new_player_upscaler = "DLSS Quality";
        }
        app.controls_diagram_menus =
            preview && GetEnvironmentVariableW(L"BBVR_PREVIEW_CONTROLS_MENUS", nullptr, 0) > 0;
        app.preview_play_bottom =
            preview && GetEnvironmentVariableW(L"BBVR_PREVIEW_PLAY_BOTTOM", nullptr, 0) > 0;
        if (!preview && !idle_smoke) {
            ShowWindow(window_handle, SW_SHOWDEFAULT);
            UpdateWindow(window_handle);
            if (app.profile.check_updates)
                app.Check();
        }
        uint64_t last_tick = 0;
        int preview_frames = 0, preview_step = 0;
        // BBVR_PREVIEW_SETTINGS with --preview: every VR settings tab, then searches, each
        // scrolled through a page at a time (settings-<shot>-<what>-<page>.png).
        const bool settings_preview =
            preview && GetEnvironmentVariableW(L"BBVR_PREVIEW_SETTINGS", nullptr, 0) > 0;
        // Offline capture of the Mods page using a fixture beside the preview output.
        const bool mods_preview =
            preview && GetEnvironmentVariableW(L"BBVR_PREVIEW_MODS", nullptr, 0) > 0;
        if (mods_preview) {
            app.page = 4;
            app.mod_game = preview_dir / "game";
            app.build_checked = app.mod_game / "eboot.bin";
            app.game_folder_needed = false;
            const auto packages = Mods::List(app.mod_game);
            if (!packages.empty()) {
                auto result = App::ReviewMod(app.mod_game, packages.front());
                app.mod_selection = std::move(result.selection);
                app.mod_report = std::move(result.report);
            }
        }
        static constexpr const char *preview_searches[]{"boss",  "dlss", "turn",   "vibration",
                                                        "left handed", "fps", "sky", "shadow"};
        int settings_shot = 0, settings_page = 0;
        if (settings_preview) {
            app.page = 1;
            app.preview_scroll = 0;
        }
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
            const bool waiting_for_tooltip = HunterTheme::tooltip_pending && !IsIconic(window_handle);
            if (!preview)
                MsgWaitForMultipleObjectsEx(0, nullptr,
                                            repaint > 0  ? 16
                                            : waiting_for_tooltip ? 24
                                            : app.Working() ? 100
                                                         : 1000,
                                            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT)
                    app.quit = true;
            }
            if (GetTickCount64() - last_tick >= 1000 || app.Working()) {
                app.Tick();
                last_tick = GetTickCount64();
                if (app.Working())
                    repaint = 1;
            }
            if (!preview && (IsIconic(window_handle) ||
                (repaint == 0 && !io.WantTextInput && !HunterTheme::tooltip_pending)))
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
            if (theme_preview && preview_pointer[0]) {
                // Exercise the actual hover/held states without sending OS input or actions.
                if (std::wstring_view(preview_pointer)==L"keyboard") {
                    io.AddKeyEvent(ImGuiKey_Tab, preview_frames==1);
                } else if (std::wstring_view(preview_pointer)==L"launch-hover") {
                    if (preview_step==0)
                        io.AddMousePosEvent(io.DisplaySize.x*.62f,io.DisplaySize.y-S(100));
                } else {
                    io.AddMousePosEvent(S(85), S(198));
                    io.AddMouseButtonEvent(0, preview_frames>=2 &&
                        std::wstring_view(preview_pointer)==L"pressed");
                }
            }
            ImGui::NewFrame();
            HunterTheme::BeginTooltipFrame();
            app.Draw();
            ++rendered_frames;
            ImGui::Render();
            const float clear[]{0.033f, 0.040f, 0.046f, 1};
            auto *rt = target.Get();
            context->OMSetRenderTargets(1, &rt, nullptr);
            context->ClearRenderTargetView(rt, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            if (preview && !theme_preview && !settings_preview &&
                preview_step == 2 && preview_frames % 4 == 1) {
                Capture(preview_dir / "update-available.png");
                app.downloaded = true;
                app.update_status = "Update downloaded. Select Install & Restart when ready.";
            }
            if (preview && ++preview_frames % 4 == 0 && mods_preview) {
                Capture(preview_dir / "mods.png");
                app.quit = true;
            } else if (preview && preview_frames % 4 == 0 && settings_preview) {
                const auto &categories = Categories();
                const int tabs = int(categories.size());
                const int shots = tabs + int(std::size(preview_searches));
                std::string what = settings_shot < tabs
                                       ? std::string(categories[settings_shot])
                                       : "search-" + std::string(preview_searches[settings_shot - tabs]);
                for (auto &c : what)
                    if (!std::isalnum(static_cast<unsigned char>(c)))
                        c = '-';
                char name[96];
                std::snprintf(name, sizeof(name), "settings-%02d-%s-%d.png", settings_shot,
                              what.c_str(), settings_page);
                Capture(preview_dir / name);
                if (app.settings_scroll_max > app.preview_scroll + 1) {
                    app.preview_scroll = std::min(app.settings_scroll_max, app.preview_scroll + S(360));
                    ++settings_page;
                } else if (++settings_shot < shots) {
                    settings_page = 0;
                    app.preview_scroll = 0;
                    if (settings_shot < tabs) {
                        app.search.clear();
                        app.category = settings_shot;
                    } else
                        app.search = preview_searches[settings_shot - tabs];
                } else
                    app.quit = true;
            } else if (preview && preview_frames % 4 == 0) {
                // Each page, then the Graphics tab and the backups and crash windows.
                const char *names[]{"play.png",     "settings.png", "updates.png",
                                    "graphics.png", "controls-quest.png", "controls-psvr2.png",
                                    "controls-frame.png", "controls-index.png", "controls-vive.png",
                                    "controls-wmr.png", "controls-guide.png",
                                    "controls-guide-search.png", "controls-search-more.png",
                                    "benchmark-results.png", "perf-scan-applied.png", "perf-scan-no-headset.png",
                                    "perf-scan-ready.png", "crash.png", "backups.png"};
                Capture(preview_dir / names[preview_step]);
                switch (++preview_step) {
                case 1:
                case 2:
                    app.page = preview_step;
                    break;
                case 3:
                    app.page = 1;
                    app.OpenTab("Graphics");
                    break;
                case 4: // the Controls page: each controller's tab
                    app.page = 3;
                    app.controls_select = 0;
                    break;
                case 5:
                case 6:
                case 7:
                case 8:
                case 9:
                    app.controls_select = preview_step - 4;
                    break;
                case 10: // the VR interactions guide, then a search in it and on a controller's tab
                    app.controls_select = ControlsGuide::PlatformCount;
                    break;
                case 11:
                    app.controls_search = "lantern";
                    break;
                case 12:
                    app.controls_select = 0;
                    app.controls_search = "visceral";
                    break;
                case 13: {
                    app.controls_search.clear();
                    const auto fixture=preview_dir/"benchmark-results.json";
                    if(fs::exists(fixture)) {
                        Benchmark::Report report;
                        for(const auto& item:Json::parse(Read(fixture))) {
                            Benchmark::Result result;result.candidate.label=item.at("label");
                            result.candidate.id=item.at("id");result.candidate.quality=item.at("quality_rank");
                            result.target_hz=item.value("target_hz",90);report.target_hz=result.target_hz;
                            result.candidate.graphics=ParseGraphics(item.at("graphics").get<std::string>());
                            result.candidate.upscale_focus=item.value("upscale_focus",0);
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
                        report.picture=Benchmark::PictureSettings(app.ini); // measured as the preview's
                        app.benchmark_report=std::move(report);
                        app.benchmark_headset=Benchmark::Headset{"Quest 3 (preview)",{1840,1840},{2016,2016},3680,4032,90};
                        app.open_perf_scan=true;
                    }
                    break;
                }
                case 14: // the suggestion applied, in memory only: the preview never writes
                    if (const auto *s = app.ScanSuggestion()) {
                        const int hz = app.benchmark_report->target_hz;
                        app.benchmark_applied = App::AppliedScan{app.benchmark_report->folder,
                                                                 s->candidate.id, app.profile, app.ini};
                        app.ini = VR::Ini::ApplyEdits(app.ini, Benchmark::SettingEdits(s->candidate, hz));
                        app.profile = Benchmark::AppliedProfile(app.profile, s->candidate, hz);
                    }
                    break;
                case 15:
                    if (app.benchmark_applied) {
                        app.profile = app.benchmark_applied->before;
                        app.ini = app.benchmark_applied->before_ini;
                        app.benchmark_applied.reset();
                    }
                    app.benchmark_report.reset();
                    app.benchmark_headset.reset();
                    break;
                case 16:
                    app.benchmark_headset=Benchmark::Headset{"Quest 3 (preview)",{1840,1840},{2016,2016},3680,4032,90};
                    break;
                case 17:
                    app.page = 0;
                    app.crash = App::Crash{0xC0000005, ExitReason(0xC0000005),
                                           {"[Debug] <Critical> signals.cpp:175 Unhandled "
                                            "Exception code 0xC0000005 (preview)"}};
                    app.show_crash = true;
                    break;
                case 18:
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
