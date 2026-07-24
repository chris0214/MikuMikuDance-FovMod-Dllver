#pragma once

#include <windows.h>

namespace mmd931::timeline_extension {

inline constexpr wchar_t kDllName[] = L"MmdTimelineExtension.dll";
inline constexpr wchar_t kIniName[] = L"MmdTimelineExtension.ini";

struct Exports {
    using InstallFn = BOOL (*)();
    using UninstallFn = BOOL (*)();
    using IsInstalledFn = BOOL (*)();
    using GetStatusJsonWFn = DWORD (*)(wchar_t *, DWORD);

    HMODULE module = nullptr;
    InstallFn install = nullptr;
    UninstallFn uninstall = nullptr;
    IsInstalledFn is_installed = nullptr;
    GetStatusJsonWFn get_status_json = nullptr;

    bool complete() const noexcept {
        return module != nullptr && install != nullptr && uninstall != nullptr &&
            is_installed != nullptr && get_status_json != nullptr;
    }

    void clear() noexcept { *this = {}; }
};

}  // namespace mmd931::timeline_extension

#if defined(MMD931_TIMELINE_EXTENSION_BUILD)
#define MMD931_TIMELINE_API __declspec(dllexport)
#else
#define MMD931_TIMELINE_API __declspec(dllimport)
#endif

extern "C" {
MMD931_TIMELINE_API BOOL MmdTimelineInstall();
MMD931_TIMELINE_API BOOL MmdTimelineUninstall();
MMD931_TIMELINE_API BOOL MmdTimelineIsInstalled();
MMD931_TIMELINE_API DWORD MmdTimelineGetStatusJsonW(
    wchar_t *buffer,
    DWORD character_capacity);
}

#undef MMD931_TIMELINE_API
