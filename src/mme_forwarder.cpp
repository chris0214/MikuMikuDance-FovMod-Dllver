#include "mmd_931_mme.hpp"
#include "mmd_931_exports.hpp"
#include "mmd_931_timeline_extension.hpp"
#include "float_fov_patch.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace {

HMODULE g_self = nullptr;
INIT_ONCE g_load_once = INIT_ONCE_STATIC_INIT;
INIT_ONCE g_host_exports_once = INIT_ONCE_STATIC_INIT;
mmd931::mme::Callbacks g_downstream;
mmd931::Exports g_host_exports;

struct UiExtension {
    using InstallFn = BOOL (*)(HWND);
    using UninstallFn = BOOL (*)();
    using IsInstalledFn = BOOL (*)();

    HMODULE module = nullptr;
    InstallFn install = nullptr;
    UninstallFn uninstall = nullptr;
    IsInstalledFn is_installed = nullptr;

    bool complete() const {
        return module != nullptr && install != nullptr && uninstall != nullptr &&
            is_installed != nullptr;
    }

    void clear() {
        module = nullptr;
        install = nullptr;
        uninstall = nullptr;
        is_installed = nullptr;
    }
};

UiExtension g_ui_extension;
mmd931::timeline_extension::Exports g_timeline_extension;

struct CallbackCounts {
    std::atomic<std::uint64_t> initialize{0};
    std::atomic<std::uint64_t> cleanup{0};
    std::atomic<std::uint64_t> create_model{0};
    std::atomic<std::uint64_t> delete_model{0};
    std::atomic<std::uint64_t> begin_scene{0};
    std::atomic<std::uint64_t> end_scene{0};
    std::atomic<std::uint64_t> draw_primitive{0};
    std::atomic<std::uint64_t> draw_indexed_primitive{0};
    std::atomic<std::uint64_t> lost_device{0};
    std::atomic<std::uint64_t> reset_device{0};
    std::atomic<std::uint64_t> clear{0};
};

CallbackCounts g_counts;

void count(std::atomic<std::uint64_t> &value) {
    value.fetch_add(1, std::memory_order_relaxed);
}

bool module_sibling_path(const wchar_t *filename, wchar_t (&output)[MAX_PATH]) {
    const DWORD length = GetModuleFileNameW(g_self, output, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    wchar_t *separator = std::wcsrchr(output, L'\\');
    if (separator == nullptr) {
        return false;
    }
    separator[1] = L'\0';
    const std::size_t directory_length = std::wcslen(output);
    const std::size_t filename_length = std::wcslen(filename);
    if (directory_length + filename_length >= MAX_PATH) {
        return false;
    }
    std::wmemcpy(output + directory_length, filename, filename_length + 1);
    return true;
}

void log_line(const char *message) {
    wchar_t path[MAX_PATH] = {};
    if (!module_sibling_path(L"MMEffect.forwarder.log", path)) {
        return;
    }
    HANDLE file = CreateFileW(
        path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
    static constexpr char newline[] = "\r\n";
    WriteFile(file, newline, sizeof(newline) - 1, &written, nullptr);
    CloseHandle(file);
}

unsigned long long read_count(const std::atomic<std::uint64_t> &value) {
    return static_cast<unsigned long long>(value.load(std::memory_order_relaxed));
}

void log_callback_summary() {
    char line[640] = {};
    std::snprintf(
        line,
        sizeof(line),
        "callback counts: Initialize=%llu Cleanup=%llu OnCreateModel=%llu "
        "OnDeleteModel=%llu OnBeginScene=%llu OnEndScene=%llu "
        "OnDrawPrimitive=%llu OnDrawIndexedPrimitive=%llu OnLostDevice=%llu "
        "OnResetDevice=%llu OnClear=%llu",
        read_count(g_counts.initialize),
        read_count(g_counts.cleanup),
        read_count(g_counts.create_model),
        read_count(g_counts.delete_model),
        read_count(g_counts.begin_scene),
        read_count(g_counts.end_scene),
        read_count(g_counts.draw_primitive),
        read_count(g_counts.draw_indexed_primitive),
        read_count(g_counts.lost_device),
        read_count(g_counts.reset_device),
        read_count(g_counts.clear));
    log_line(line);
}

void log_float_fov_status(const char *action) {
    const auto status = mmd931::float_fov_patch::status();
    char line[256] = {};
    std::snprintf(
        line,
        sizeof(line),
        "Float FOV patch %s: error=%s profile=%s site=%zu system_error=%lu",
        action,
        mmd931::float_fov_patch::error_name(status.error),
        status.profile != nullptr ? status.profile : "none",
        status.site_index,
        status.system_error);
    log_line(line);
}

void log_viewport_transaction_summary() {
    const auto status = mmd931::float_fov_patch::transaction_status();
    char line[192] = {};
    std::snprintf(
        line,
        sizeof(line),
        "viewport transaction: active=%d starts=%llu restores=%llu stale=%llu",
        status.active ? 1 : 0,
        static_cast<unsigned long long>(status.starts),
        static_cast<unsigned long long>(status.restores),
        static_cast<unsigned long long>(status.stale_recoveries));
    log_line(line);
}

void apply_float_fov_projection(IDirect3DDevice9 *device) {
    float fov_degrees = 0.0f;
    if (device == nullptr ||
        !mmd931::float_fov_patch::active_viewport_fov(fov_degrees)) {
        return;
    }

    D3DVIEWPORT9 viewport{};
    D3DMATRIX projection{};
    if (FAILED(device->GetViewport(&viewport)) || viewport.Height == 0 ||
        FAILED(device->GetTransform(D3DTS_PROJECTION, &projection))) {
        return;
    }
    if (std::fabs(projection._34) < 0.5f || std::fabs(projection._44) > 0.001f) {
        return;
    }

    float x_scale = 0.0f;
    float y_scale = 0.0f;
    const float aspect =
        static_cast<float>(viewport.Width) / static_cast<float>(viewport.Height);
    if (!mmd931::float_fov_patch::detail::perspective_scales(
            fov_degrees, aspect, x_scale, y_scale)) {
        return;
    }
    projection._11 = std::copysign(x_scale, projection._11);
    projection._22 = std::copysign(y_scale, projection._22);
    device->SetTransform(D3DTS_PROJECTION, &projection);
}

template <typename FunctionPointer>
bool resolve_export(HMODULE module, const char *name, FunctionPointer &output) {
    static_assert(sizeof(FunctionPointer) == sizeof(FARPROC));
    FARPROC raw = GetProcAddress(module, name);
    std::memcpy(&output, &raw, sizeof(output));
    return output != nullptr;
}

void install_ui_extension() {
    if (g_ui_extension.complete()) {
        return;
    }
    wchar_t path[MAX_PATH] = {};
    if (!module_sibling_path(L"MmdUiExtension.dll", path)) {
        log_line("MmdUiExtension path construction failed");
        return;
    }
    HMODULE module = LoadLibraryW(path);
    if (module == nullptr) {
        log_line("MmdUiExtension.dll is absent; runtime UI extension skipped");
        return;
    }
    g_ui_extension.module = module;
    const bool resolved =
        resolve_export(module, "MmdUiInstall", g_ui_extension.install) &&
        resolve_export(module, "MmdUiUninstall", g_ui_extension.uninstall) &&
        resolve_export(module, "MmdUiIsInstalled", g_ui_extension.is_installed);
    if (!resolved || !g_ui_extension.complete()) {
        log_line("MmdUiExtension.dll is missing required exports");
        FreeLibrary(module);
        g_ui_extension.clear();
        return;
    }
    if (g_ui_extension.install(nullptr) && g_ui_extension.is_installed()) {
        log_line("MmdUiExtension installed");
        return;
    }
    log_line("MmdUiExtension install failed");
    FreeLibrary(module);
    g_ui_extension.clear();
}

void uninstall_ui_extension() {
    if (!g_ui_extension.complete()) {
        return;
    }
    if (g_ui_extension.uninstall()) {
        log_line("MmdUiExtension uninstalled");
    }
    else {
        log_line("MmdUiExtension uninstall failed");
    }
    HMODULE module = g_ui_extension.module;
    g_ui_extension.clear();
    FreeLibrary(module);
}

void log_timeline_status() {
    if (g_timeline_extension.get_status_json == nullptr) return;
    const DWORD required = g_timeline_extension.get_status_json(nullptr, 0);
    if (required == 0 || required > 16384) return;
    std::vector<wchar_t> wide(required, L'\0');
    if (g_timeline_extension.get_status_json(wide.data(), required) != required) return;
    const int utf8_size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 1) return;
    std::string line(static_cast<std::size_t>(utf8_size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), -1, line.data(), utf8_size,
            nullptr, nullptr) <= 0) {
        return;
    }
    line.resize(static_cast<std::size_t>(utf8_size - 1));
    log_line(line.c_str());
}

void install_timeline_extension() {
    if (g_timeline_extension.complete()) return;
    wchar_t path[MAX_PATH] = {};
    if (!module_sibling_path(mmd931::timeline_extension::kDllName, path)) {
        log_line("MmdTimelineExtension path construction failed");
        return;
    }
    HMODULE module = LoadLibraryW(path);
    if (module == nullptr) {
        log_line("MmdTimelineExtension.dll is absent; timeline expansion skipped");
        return;
    }
    g_timeline_extension.module = module;
    const bool resolved =
        resolve_export(module, "MmdTimelineInstall", g_timeline_extension.install) &&
        resolve_export(module, "MmdTimelineUninstall", g_timeline_extension.uninstall) &&
        resolve_export(module, "MmdTimelineIsInstalled", g_timeline_extension.is_installed) &&
        resolve_export(
            module, "MmdTimelineGetStatusJsonW",
            g_timeline_extension.get_status_json);
    if (!resolved || !g_timeline_extension.complete()) {
        log_line("MmdTimelineExtension.dll is missing required exports");
        FreeLibrary(module);
        g_timeline_extension.clear();
        return;
    }
    if (g_timeline_extension.install() && g_timeline_extension.is_installed()) {
        log_line("MmdTimelineExtension installed");
        log_timeline_status();
        return;
    }
    log_line("MmdTimelineExtension install rejected; original MMD remains unmodified");
    log_timeline_status();
    FreeLibrary(module);
    g_timeline_extension.clear();
}

void uninstall_timeline_extension() {
    if (!g_timeline_extension.complete()) return;
    if (!g_timeline_extension.uninstall() || g_timeline_extension.is_installed()) {
        log_line("MmdTimelineExtension uninstall failed; module retained for safety");
        log_timeline_status();
        return;
    }
    log_line("MmdTimelineExtension uninstalled");
    HMODULE module = g_timeline_extension.module;
    g_timeline_extension.clear();
    FreeLibrary(module);
}

BOOL CALLBACK load_downstream(PINIT_ONCE, PVOID, PVOID *) {
    wchar_t path[MAX_PATH] = {};
    if (!module_sibling_path(L"MMEffect.original.dll", path)) {
        log_line("downstream path construction failed");
        return TRUE;
    }
    HMODULE module = LoadLibraryW(path);
    if (module == nullptr) {
        log_line("LoadLibraryW(MMEffect.original.dll) failed");
        return TRUE;
    }
    if (!g_downstream.resolve(module)) {
        log_line("MMEffect.original.dll is missing one or more required callbacks");
        return TRUE;
    }
    log_line("MMEffect.original.dll loaded and all 11 callbacks resolved");
    return TRUE;
}

bool ensure_downstream() {
    InitOnceExecuteOnce(&g_load_once, load_downstream, nullptr, nullptr);
    return g_downstream.complete();
}

BOOL CALLBACK load_host_exports(PINIT_ONCE, PVOID, PVOID *) {
    if (g_host_exports.load()) {
        log_line("host exports resolved for runtime material observation");
    }
    else {
        log_line("host export resolution failed");
    }
    return TRUE;
}

bool ensure_host_exports() {
    InitOnceExecuteOnce(&g_host_exports_once, load_host_exports, nullptr, nullptr);
    return g_host_exports.host != nullptr &&
        g_host_exports.ExpGetPmdNum != nullptr &&
        g_host_exports.ExpGetPmdID != nullptr &&
        g_host_exports.ExpGetPmdMatNum != nullptr &&
        g_host_exports.ExpGetPmdMaterial != nullptr &&
        g_host_exports.ExpGetAcsNum != nullptr &&
        g_host_exports.ExpGetAcsID != nullptr &&
        g_host_exports.ExpGetAcsMatNum != nullptr &&
        g_host_exports.ExpGetAcsMaterial != nullptr;
}

void log_material(
    const char *object_type,
    int object_index,
    std::uint32_t object_id,
    std::uint32_t material_count,
    const mmd931::Material &material) {
    char line[640] = {};
    std::snprintf(
        line,
        sizeof(line),
        "%s material bridge index=%d id=%u count=%u "
        "Diffuse=(%.6g,%.6g,%.6g,%.6g) Ambient=(%.6g,%.6g,%.6g,%.6g) "
        "Specular=(%.6g,%.6g,%.6g,%.6g) Emissive=(%.6g,%.6g,%.6g,%.6g) Power=%.6g",
        object_type,
        object_index,
        object_id,
        material_count,
        material.Diffuse.r,
        material.Diffuse.g,
        material.Diffuse.b,
        material.Diffuse.a,
        material.Ambient.r,
        material.Ambient.g,
        material.Ambient.b,
        material.Ambient.a,
        material.Specular.r,
        material.Specular.g,
        material.Specular.b,
        material.Specular.a,
        material.Emissive.r,
        material.Emissive.g,
        material.Emissive.b,
        material.Emissive.a,
        material.Power);
    log_line(line);
}

void observe_first_host_material(
    mmd931::mme::ObjectId id,
    mmd931::mme::ModelKind kind) {
    if (!ensure_host_exports()) {
        return;
    }

    if (kind == mmd931::mme::ModelKind::PmdOrPmx) {
        const int object_count = g_host_exports.ExpGetPmdNum();
        for (int index = 0; index < object_count; ++index) {
            if (g_host_exports.ExpGetPmdID(index) != id) {
                continue;
            }
            const std::uint32_t material_count = g_host_exports.ExpGetPmdMatNum(index);
            if (material_count != 0) {
                mmd931::Material material{};
                g_host_exports.ExpGetPmdMaterial(&material, index, 0);
                log_material("PMD/PMX", index, id, material_count, material);
            }
            return;
        }
        log_line("PMD/PMX material bridge could not map callback ID to host index");
        return;
    }

    const int object_count = g_host_exports.ExpGetAcsNum();
    for (int index = 0; index < object_count; ++index) {
        if (g_host_exports.ExpGetAcsID(index) != id) {
            continue;
        }
        const std::uint32_t material_count = g_host_exports.ExpGetAcsMatNum(index);
        if (material_count != 0) {
            mmd931::Material material{};
            g_host_exports.ExpGetAcsMaterial(&material, index, 0);
            log_material("Accessory", index, id, material_count, material);
        }
        return;
    }
    log_line("Accessory material bridge could not map callback ID to host index");
}

}  // namespace

extern "C" __declspec(dllexport) int Initialize(IDirect3DDevice9 *device) {
    count(g_counts.initialize);
    if (!ensure_downstream()) {
        log_line("Initialize failed: downstream unavailable");
        return 1;
    }
    const int result = g_downstream.Initialize(device);
    if (result == 0) {
        if (mmd931::float_fov_patch::install()) {
            log_float_fov_status("installed");
        }
        else {
            log_float_fov_status("install rejected");
        }
        install_timeline_extension();
        install_ui_extension();
    }
    char line[96] = {};
    std::snprintf(line, sizeof(line), "Initialize forwarded: result=%d", result);
    log_line(line);
    return result;
}

extern "C" __declspec(dllexport) void Cleanup(IDirect3DDevice9 *device) {
    count(g_counts.cleanup);
    uninstall_ui_extension();
    uninstall_timeline_extension();
    mmd931::float_fov_patch::restore_viewport_transaction();
    if (mmd931::float_fov_patch::uninstall()) {
        log_float_fov_status("uninstalled");
    }
    else {
        log_float_fov_status("uninstall failed");
    }
    log_viewport_transaction_summary();
    if (ensure_downstream()) {
        g_downstream.Cleanup(device);
    }
    log_callback_summary();
    log_line("Cleanup forwarded");
}

extern "C" __declspec(dllexport) void OnCreateModel(
    IDirect3DDevice9 *device,
    mmd931::mme::ObjectId id,
    const char *filename,
    mmd931::mme::ModelKind kind,
    std::uint32_t material_count,
    void *reserved0,
    IUnknown *reserved1) {
    count(g_counts.create_model);
    if (!ensure_downstream()) {
        return;
    }
    char line[160] = {};
    std::snprintf(
        line,
        sizeof(line),
        "OnCreateModel id=%u kind=%d materials=%u",
        id,
        static_cast<int>(kind),
        material_count);
    log_line(line);
    observe_first_host_material(id, kind);
    g_downstream.OnCreateModel(
        device, id, filename, kind, material_count, reserved0, reserved1);
}

extern "C" __declspec(dllexport) void OnDeleteModel(
    IDirect3DDevice9 *device,
    mmd931::mme::ObjectId id) {
    count(g_counts.delete_model);
    if (!ensure_downstream()) {
        return;
    }
    char line[64] = {};
    std::snprintf(line, sizeof(line), "OnDeleteModel id=%u", id);
    log_line(line);
    g_downstream.OnDeleteModel(device, id);
}

extern "C" __declspec(dllexport) void OnBeginScene(IDirect3DDevice9 *device) {
    count(g_counts.begin_scene);
    apply_float_fov_projection(device);
    if (ensure_downstream()) {
        g_downstream.OnBeginScene(device);
    }
}

extern "C" __declspec(dllexport) void OnEndScene(IDirect3DDevice9 *device) {
    count(g_counts.end_scene);
    if (ensure_downstream()) {
        g_downstream.OnEndScene(device);
    }
    mmd931::float_fov_patch::restore_viewport_transaction();
}

extern "C" __declspec(dllexport) HRESULT OnDrawPrimitive(
    IDirect3DDevice9 *device,
    D3DPRIMITIVETYPE primitive_type,
    UINT start_vertex,
    UINT primitive_count) {
    count(g_counts.draw_primitive);
    return ensure_downstream()
        ? g_downstream.OnDrawPrimitive(device, primitive_type, start_vertex, primitive_count)
        : D3D_OK;
}

extern "C" __declspec(dllexport) HRESULT OnDrawIndexedPrimitive(
    IDirect3DDevice9 *device,
    D3DPRIMITIVETYPE primitive_type,
    INT base_vertex_index,
    UINT min_vertex_index,
    UINT vertex_count,
    UINT start_index,
    UINT primitive_count) {
    count(g_counts.draw_indexed_primitive);
    return ensure_downstream()
        ? g_downstream.OnDrawIndexedPrimitive(
              device,
              primitive_type,
              base_vertex_index,
              min_vertex_index,
              vertex_count,
              start_index,
              primitive_count)
        : D3D_OK;
}

extern "C" __declspec(dllexport) void OnLostDevice(IDirect3DDevice9 *device) {
    count(g_counts.lost_device);
    mmd931::float_fov_patch::restore_viewport_transaction();
    if (ensure_downstream()) {
        g_downstream.OnLostDevice(device);
    }
}

extern "C" __declspec(dllexport) void OnResetDevice(IDirect3DDevice9 *device) {
    count(g_counts.reset_device);
    mmd931::float_fov_patch::restore_viewport_transaction();
    if (ensure_downstream()) {
        g_downstream.OnResetDevice(device);
    }
}

extern "C" __declspec(dllexport) HRESULT OnClear(
    IDirect3DDevice9 *device,
    DWORD rect_count,
    const D3DRECT *rects,
    DWORD flags,
    D3DCOLOR color,
    float z,
    DWORD stencil,
    BOOL is_main_target) {
    count(g_counts.clear);
    return ensure_downstream()
        ? g_downstream.OnClear(
              device, rect_count, rects, flags, color, z, stencil, is_main_target)
        : D3D_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
