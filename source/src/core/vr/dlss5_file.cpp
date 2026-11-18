// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/dlss5_file.h"

#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust")
#pragma comment(lib, "crypt32")
#pragma comment(lib, "version")
#endif

namespace VR::Dlss5File {

namespace {

std::vector<unsigned long> Parts(const std::string& version) {
    std::vector<unsigned long> out;
    const char* at = version.c_str();
    while (*at) {
        char* end = nullptr;
        out.push_back(std::strtoul(at, &end, 10));
        if (end == at)
            break;
        at = *end == '.' ? end + 1 : end;
    }
    return out;
}

#ifdef _WIN32
// The signer's name when WinVerifyTrust accepts the file's embedded signature, else empty.
// Revocation is not checked (no network); the signature and its timestamp are.
std::wstring TrustedSigner(const std::filesystem::path& file) {
    WINTRUST_FILE_INFO info{sizeof(info)};
    info.pcwszFilePath = file.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &info;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG trusted = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    std::wstring signer;
    if (trusted == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sgnr =
            provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* cert = sgnr ? WTHelperGetProvCertFromChain(sgnr, 0) : nullptr;
        wchar_t name[256]{};
        if (cert && CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                                       name, DWORD(std::size(name))) > 1)
            signer = name;
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return signer;
}

// The file's version and its ProductName (first translation).
bool VersionInfo(const std::filesystem::path& file, std::string& version, std::wstring& product) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (!size)
        return false;
    std::vector<unsigned char> block(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, block.data()))
        return false;
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) || !fixed)
        return false;
    version = std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." +
              std::to_string(LOWORD(fixed->dwFileVersionMS)) + "." +
              std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." +
              std::to_string(LOWORD(fixed->dwFileVersionLS));
    struct Translation {
        WORD language, codepage;
    }* translation = nullptr;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<void**>(&translation), &length) &&
        translation && length >= sizeof(Translation)) {
        wchar_t key[64];
        swprintf(key, std::size(key), L"\\StringFileInfo\\%04x%04x\\ProductName",
                 translation->language, translation->codepage);
        wchar_t* text = nullptr;
        if (VerQueryValueW(block.data(), key, reinterpret_cast<void**>(&text), &length) && text)
            product = text;
    }
    return true;
}
#endif

} // namespace

bool Newer(const std::string& a, const std::string& b) {
    return Parts(a) > Parts(b);
}

Check Verify(const std::filesystem::path& file) {
    Check out;
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
        out.problem = "No DLSS 5 file.";
        return out;
    }
#ifdef _WIN32
    std::wstring product;
    if (!VersionInfo(file, out.version, product) || product != L"NVIDIA DLSSNR") {
        out.problem = "This is not NVIDIA's DLSS 5 file (nvngx_dlssnr.dll).";
        return out;
    }
    if (TrustedSigner(file) != L"NVIDIA Corporation") {
        out.problem = "This copy is not signed by NVIDIA (modified or damaged); it is not used.";
        return out;
    }
    out.ok = true;
#else
    out.problem = "DLSS 5 needs Windows.";
#endif
    return out;
}

} // namespace VR::Dlss5File
