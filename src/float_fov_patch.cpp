#include "float_fov_patch.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace mmd931::float_fov_patch {
namespace {

constexpr std::size_t kStubStride = 128;
constexpr std::size_t kSiteCount = 7;

constexpr std::array<std::uint8_t, 8> kRenderFovExpected = {
    0xf3, 0x0f, 0x11, 0x87, 0xb4, 0xf0, 0x09, 0x00};
constexpr std::array<std::uint8_t, 8> kRenderDistanceExpected931 = {
    0x89, 0x87, 0x00, 0x19, 0x0a, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 8> kRenderDistanceExpected926 = {
    0x89, 0x87, 0xf8, 0x18, 0x0a, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 8> kViewportCurrentExpected = {
    0xf3, 0x0f, 0x11, 0x86, 0xb4, 0xf0, 0x09, 0x00};
constexpr std::array<std::uint8_t, 8> kViewportInterpolationExpected = {
    0xf3, 0x0f, 0x11, 0x8e, 0xb4, 0xf0, 0x09, 0x00};
constexpr std::array<std::uint8_t, 8> kViewportFrameTransactionExpected = {
    0x45, 0x38, 0xbc, 0x24, 0x54, 0x03, 0x00, 0x00};

constexpr detail::ProfileDefinition make_profile(
    const char *name,
    std::uint32_t offset_adjustment,
    std::uint32_t distance_offset,
    const std::array<std::uint8_t, 8> &render_distance_expected) {
    return {
        name,
        distance_offset,
        {{
            {"render-current", 0x69c5a + offset_adjustment,
             detail::StubKind::RenderFov, kRenderFovExpected, 8},
            {"render-interpolation", 0x69ebc + offset_adjustment,
             detail::StubKind::RenderFov, kRenderFovExpected, 8},
            {"render-last", 0x69b9d + offset_adjustment,
             detail::StubKind::RenderFov, kRenderFovExpected, 8},
            {"render-distance-source", 0x69cd6 + offset_adjustment,
             detail::StubKind::RenderDistance, render_distance_expected, 6},
            {"viewport-current", 0x59d17 + offset_adjustment,
             detail::StubKind::ViewportFov, kViewportCurrentExpected, 8},
            {"viewport-interpolation", 0x5a5b1 + offset_adjustment,
             detail::StubKind::ViewportFov, kViewportInterpolationExpected, 8},
            {"viewport-frame-transaction", 0x27e96,
             detail::StubKind::ViewportFrameTransaction,
             kViewportFrameTransactionExpected, 8},
        }}};
}

constexpr std::array<detail::ProfileDefinition, 3> kProfiles = {{
    make_profile("MMD 9.31 x64 analyzed", 0, 0x000a1900, kRenderDistanceExpected931),
    make_profile("Float FOV 2 CE-table layout", 0x10, 0x000a1900, kRenderDistanceExpected931),
    make_profile("MMD 9.26 x64", 0x40, 0x000a18f8, kRenderDistanceExpected926),
}};

struct InstalledSite {
    std::uint8_t *target = nullptr;
    std::array<std::uint8_t, 8> original{};
    std::array<std::uint8_t, 8> patch{};
    std::uint8_t size = 0;
};

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    bool installed = false;
    Status status{};
    void *stub_block = nullptr;
    std::size_t stub_block_size = 0;
    std::array<InstalledSite, kSiteCount> sites{};
};

State g_state;
detail::ViewportTransactionState g_viewport_transaction;

static_assert(offsetof(detail::ViewportTransactionState, active) == 0);
static_assert(offsetof(detail::ViewportTransactionState, raw_distance_bits) == 4);
static_assert(offsetof(detail::ViewportTransactionState, host_state) == 8);
static_assert(offsetof(detail::ViewportTransactionState, distance_offset) == 16);
static_assert(offsetof(detail::ViewportTransactionState, starts) == 24);
static_assert(offsetof(detail::ViewportTransactionState, restores) == 32);
static_assert(offsetof(detail::ViewportTransactionState, stale_recoveries) == 40);

class ExclusiveLock {
public:
    explicit ExclusiveLock(SRWLOCK &lock) : lock_(lock) {
        AcquireSRWLockExclusive(&lock_);
    }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }

    ExclusiveLock(const ExclusiveLock &) = delete;
    ExclusiveLock &operator=(const ExclusiveLock &) = delete;

private:
    SRWLOCK &lock_;
};

bool relative_displacement(
    std::uintptr_t instruction_end,
    std::uintptr_t destination,
    std::int32_t &output) {
    const auto distance = static_cast<std::int64_t>(destination) -
        static_cast<std::int64_t>(instruction_end);
    if (distance < std::numeric_limits<std::int32_t>::min() ||
        distance > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    output = static_cast<std::int32_t>(distance);
    return true;
}

void append(std::vector<std::uint8_t> &output, const std::uint8_t *bytes, std::size_t size) {
    output.insert(output.end(), bytes, bytes + size);
}

bool append_jump(
    std::vector<std::uint8_t> &output,
    std::uintptr_t stub_address,
    std::uintptr_t destination) {
    const std::uintptr_t instruction = stub_address + output.size();
    std::int32_t displacement = 0;
    if (!relative_displacement(instruction + 5, destination, displacement)) {
        return false;
    }
    output.push_back(0xe9);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&displacement);
    append(output, bytes, sizeof(displacement));
    return true;
}

bool valid_host_image(
    HMODULE module,
    std::uint8_t *&base,
    std::size_t &image_size) {
    if (module == nullptr) return false;
    base = reinterpret_cast<std::uint8_t *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SizeOfImage == 0) {
        return false;
    }
    image_size = nt->OptionalHeader.SizeOfImage;
    return true;
}

std::uintptr_t align_down(std::uintptr_t value, std::uintptr_t alignment) {
    return value & ~(alignment - 1);
}

std::uintptr_t align_up(std::uintptr_t value, std::uintptr_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void *allocate_near(std::uintptr_t image_base, std::size_t image_size, std::size_t size) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto minimum = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
    const auto radius = static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max());
    const auto lower_bound = image_base > radius ? std::max(minimum, image_base - radius) : minimum;
    const auto upper_bound = std::min(maximum, image_base + radius);
    const auto upper_start = align_up(image_base + image_size, granularity);
    const auto lower_start = align_down(image_base, granularity);

    for (std::uintptr_t offset = 0; offset <= radius; offset += granularity) {
        if (upper_start <= upper_bound && offset <= upper_bound - upper_start) {
            const std::uintptr_t candidate = upper_start + offset;
            if (candidate <= upper_bound && candidate <= maximum - size) {
                if (void *memory = VirtualAlloc(
                        reinterpret_cast<void *>(candidate), size,
                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                    return memory;
                }
            }
        }
        if (lower_start >= lower_bound && offset <= lower_start - lower_bound) {
            const std::uintptr_t candidate = lower_start - offset;
            if (candidate >= lower_bound && candidate <= maximum - size) {
                if (void *memory = VirtualAlloc(
                        reinterpret_cast<void *>(candidate), size,
                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                    return memory;
                }
            }
        }
        if (offset > radius - granularity) break;
    }
    return nullptr;
}

bool write_code(void *destination, const void *source, std::size_t size) {
    DWORD old_protection = 0;
    if (!VirtualProtect(destination, size, PAGE_EXECUTE_READWRITE, &old_protection)) {
        return false;
    }
    std::memcpy(destination, source, size);
    FlushInstructionCache(GetCurrentProcess(), destination, size);
    DWORD ignored = 0;
    VirtualProtect(destination, size, old_protection, &ignored);
    return true;
}

bool restore_installed_sites(std::size_t count) {
    bool restored = true;
    while (count != 0) {
        --count;
        const auto &site = g_state.sites[count];
        if (!write_code(site.target, site.original.data(), site.size)) {
            restored = false;
        }
    }
    return restored;
}

}  // namespace

namespace detail {

const std::array<ProfileDefinition, 3> &profiles() {
    return kProfiles;
}

const ProfileDefinition *select_profile(const std::uint8_t *image, std::size_t image_size) {
    if (image == nullptr) return nullptr;
    for (const auto &profile : kProfiles) {
        bool matches = true;
        for (const auto &site : profile.sites) {
            const std::size_t end = static_cast<std::size_t>(site.rva) + site.expected_size;
            if (end > image_size ||
                std::memcmp(image + site.rva, site.expected.data(), site.expected_size) != 0) {
                matches = false;
                break;
            }
        }
        if (matches) return &profile;
    }
    return nullptr;
}

bool build_stub(
    StubKind kind,
    std::uint32_t distance_offset,
    std::uintptr_t stub_address,
    std::uintptr_t return_address,
    std::vector<std::uint8_t> &output,
    std::uintptr_t transaction_state_address) {
    output.clear();
    static constexpr std::uint8_t render_fov_template[] = {
        0x8b, 0x87, 0x00, 0x19, 0x0a, 0x00,
        0x25, 0xff, 0xff, 0xff, 0x7f,
        0x89, 0x87, 0xb4, 0xf0, 0x09, 0x00,
        0xc7, 0x87, 0x00, 0x19, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr std::uint8_t render_distance_template[] = {
        0x89, 0x87, 0x00, 0x19, 0x0a, 0x00,
        0x89, 0x87, 0xb4, 0xf0, 0x09, 0x00};
    static constexpr std::uint8_t viewport_fov_template[] = {
        0x50,
        0x8b, 0x86, 0x00, 0x19, 0x0a, 0x00,
        0x25, 0xff, 0xff, 0xff, 0x7f,
        0x89, 0x86, 0xb4, 0xf0, 0x09, 0x00,
        0x58};

    switch (kind) {
    case StubKind::RenderFov: {
        append(output, render_fov_template, sizeof(render_fov_template));
        std::memcpy(output.data() + 2, &distance_offset, sizeof(distance_offset));
        std::memcpy(output.data() + 19, &distance_offset, sizeof(distance_offset));
        break;
    }
    case StubKind::RenderDistance: {
        append(output, render_distance_template, sizeof(render_distance_template));
        std::memcpy(output.data() + 2, &distance_offset, sizeof(distance_offset));
        break;
    }
    case StubKind::ViewportFov: {
        append(output, viewport_fov_template, sizeof(viewport_fov_template));
        std::memcpy(output.data() + 3, &distance_offset, sizeof(distance_offset));
        break;
    }
    case StubKind::ViewportFrameTransaction: {
        if (transaction_state_address == 0) return false;

        static constexpr std::uint8_t prefix[] = {
            0x50, 0x51, 0x52,
            0x41, 0x80, 0xbc, 0x24, 0x54, 0x03, 0x00, 0x00, 0x00,
            0x75, 0x00,
            0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x83, 0x38, 0x00,
            0x74, 0x00,
            0x48, 0x8b, 0x50, 0x08,
            0x48, 0x85, 0xd2,
            0x74, 0x00,
            0x8b, 0x48, 0x04,
            0x89, 0x8a, 0x00, 0x00, 0x00, 0x00,
            0xf0, 0x48, 0xff, 0x40, 0x28};
        static constexpr std::uint8_t capture[] = {
            0x41, 0x8b, 0x8c, 0x24, 0x00, 0x00, 0x00, 0x00,
            0x89, 0x48, 0x04,
            0x4c, 0x89, 0x60, 0x08,
            0x8b, 0xd1,
            0x81, 0xe2, 0xff, 0xff, 0xff, 0x7f,
            0x41, 0x89, 0x94, 0x24, 0xb4, 0xf0, 0x09, 0x00,
            0x41, 0xc7, 0x84, 0x24, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0xc7, 0x00, 0x01, 0x00, 0x00, 0x00,
            0xf0, 0x48, 0xff, 0x40, 0x18};
        static constexpr std::uint8_t suffix[] = {
            0x5a, 0x59, 0x58,
            0x45, 0x38, 0xbc, 0x24, 0x54, 0x03, 0x00, 0x00};

        append(output, prefix, sizeof(prefix));
        const std::size_t capture_offset = output.size();
        append(output, capture, sizeof(capture));
        const std::size_t suffix_offset = output.size();
        append(output, suffix, sizeof(suffix));

        std::memcpy(output.data() + 16, &transaction_state_address,
            sizeof(transaction_state_address));
        std::memcpy(output.data() + 43, &distance_offset, sizeof(distance_offset));
        std::memcpy(output.data() + capture_offset + 4,
            &distance_offset, sizeof(distance_offset));
        std::memcpy(output.data() + capture_offset + 35,
            &distance_offset, sizeof(distance_offset));

        const auto encode_short_jump = [&output](
            std::size_t displacement_offset, std::size_t destination) {
            const auto displacement = static_cast<std::ptrdiff_t>(destination) -
                static_cast<std::ptrdiff_t>(displacement_offset + 1);
            if (displacement < std::numeric_limits<std::int8_t>::min() ||
                displacement > std::numeric_limits<std::int8_t>::max()) {
                return false;
            }
            output[displacement_offset] = static_cast<std::uint8_t>(
                static_cast<std::int8_t>(displacement));
            return true;
        };
        if (!encode_short_jump(13, suffix_offset) ||
            !encode_short_jump(28, capture_offset) ||
            !encode_short_jump(37, capture_offset)) {
            return false;
        }
        break;
    }
    }
    return append_jump(output, stub_address, return_address) && output.size() <= kStubStride;
}

bool restore_transaction(ViewportTransactionState &state) {
    auto *active = reinterpret_cast<volatile LONG *>(&state.active);
    if (InterlockedCompareExchange(active, 1, 1) == 0) return false;

    const bool valid = state.host_state != 0 &&
        (state.distance_offset == 0x000a1900 ||
         state.distance_offset == 0x000a18f8);
    if (valid) {
        auto *destination = reinterpret_cast<void *>(
            state.host_state + state.distance_offset);
        std::memcpy(destination, &state.raw_distance_bits,
            sizeof(state.raw_distance_bits));
        InterlockedIncrement64(
            reinterpret_cast<volatile LONG64 *>(&state.restores));
    }
    InterlockedExchange(active, 0);
    return valid;
}

bool perspective_scales(
    float fov_degrees,
    float aspect,
    float &x_scale,
    float &y_scale) {
    if (!std::isfinite(fov_degrees) || !std::isfinite(aspect) ||
        fov_degrees <= 0.01f || fov_degrees >= 179.9f || aspect <= 0.0f) {
        return false;
    }
    constexpr float kPi = 3.14159265358979323846f;
    const float tangent = std::tan(fov_degrees * kPi / 360.0f);
    if (!std::isfinite(tangent) || tangent <= 0.0f) return false;
    y_scale = 1.0f / tangent;
    x_scale = y_scale / aspect;
    return std::isfinite(x_scale) && std::isfinite(y_scale);
}

bool build_patch(
    std::uintptr_t target_address,
    std::uintptr_t stub_address,
    std::size_t patch_size,
    std::vector<std::uint8_t> &output) {
    output.clear();
    if (patch_size < 5) return false;
    std::int32_t displacement = 0;
    if (!relative_displacement(target_address + 5, stub_address, displacement)) return false;
    output.assign(patch_size, 0x90);
    output[0] = 0xe9;
    std::memcpy(output.data() + 1, &displacement, sizeof(displacement));
    return true;
}

}  // namespace detail

bool install() {
    ExclusiveLock guard(g_state.lock);
    if (g_state.installed) return true;
    g_state.status = {};

    if (sizeof(void *) != 8) {
        g_state.status.error = Error::UnsupportedArchitecture;
        return false;
    }

    std::uint8_t *image = nullptr;
    std::size_t image_size = 0;
    if (!valid_host_image(GetModuleHandleW(nullptr), image, image_size)) {
        g_state.status.error = Error::InvalidHostImage;
        return false;
    }
    const auto *profile = detail::select_profile(image, image_size);
    if (profile == nullptr) {
        g_state.status.error = Error::UnsupportedHostVersion;
        return false;
    }
    g_state.status.profile = profile->name;
    g_viewport_transaction = {};
    g_viewport_transaction.distance_offset = profile->distance_offset;

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const std::size_t allocation_size = std::max<std::size_t>(
        system_info.dwPageSize, kStubStride * profile->sites.size());
    void *stub_block = allocate_near(
        reinterpret_cast<std::uintptr_t>(image), image_size, allocation_size);
    if (stub_block == nullptr) {
        g_state.status.error = Error::StubAllocationFailed;
        g_state.status.system_error = GetLastError();
        return false;
    }

    std::array<std::vector<std::uint8_t>, kSiteCount> stubs;
    std::array<std::vector<std::uint8_t>, kSiteCount> patches;
    for (std::size_t index = 0; index < profile->sites.size(); ++index) {
        const auto &definition = profile->sites[index];
        const auto target = reinterpret_cast<std::uintptr_t>(image + definition.rva);
        const auto stub = reinterpret_cast<std::uintptr_t>(stub_block) + index * kStubStride;
        if (!detail::build_stub(
                definition.kind, profile->distance_offset, stub,
                target + definition.expected_size, stubs[index],
                reinterpret_cast<std::uintptr_t>(&g_viewport_transaction)) ||
            !detail::build_patch(
                target, stub, definition.expected_size, patches[index])) {
            VirtualFree(stub_block, 0, MEM_RELEASE);
            g_state.status.error = Error::StubEncodingFailed;
            g_state.status.site_index = index;
            return false;
        }
        std::memcpy(
            static_cast<std::uint8_t *>(stub_block) + index * kStubStride,
            stubs[index].data(), stubs[index].size());
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(stub_block, allocation_size, PAGE_EXECUTE_READ, &old_protection)) {
        g_state.status.error = Error::StubProtectionFailed;
        g_state.status.system_error = GetLastError();
        VirtualFree(stub_block, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stub_block, allocation_size);

    std::size_t installed_count = 0;
    for (std::size_t index = 0; index < profile->sites.size(); ++index) {
        const auto &definition = profile->sites[index];
        auto &installed = g_state.sites[index];
        installed.target = image + definition.rva;
        installed.size = definition.expected_size;
        std::copy_n(installed.target, installed.size, installed.original.begin());
        std::copy_n(patches[index].begin(), installed.size, installed.patch.begin());
        if (!write_code(installed.target, installed.patch.data(), installed.size)) {
            const DWORD write_error = GetLastError();
            const bool rolled_back = restore_installed_sites(installed_count);
            if (rolled_back) {
                VirtualFree(stub_block, 0, MEM_RELEASE);
            }
            else {
                // A surviving branch still targets this allocation. Retain it until exit.
                g_state.stub_block = stub_block;
                g_state.stub_block_size = allocation_size;
                g_state.installed = true;
            }
            g_state.status.error = rolled_back ? Error::PatchWriteFailed : Error::RestoreFailed;
            g_state.status.site_index = index;
            g_state.status.system_error = write_error;
            return false;
        }
        ++installed_count;
    }

    g_state.stub_block = stub_block;
    g_state.stub_block_size = allocation_size;
    g_state.installed = true;
    g_state.status.error = Error::None;
    return true;
}

bool uninstall() {
    ExclusiveLock guard(g_state.lock);
    if (!g_state.installed) return true;

    detail::restore_transaction(g_viewport_transaction);

    for (std::size_t index = 0; index < g_state.sites.size(); ++index) {
        const auto &site = g_state.sites[index];
        if (std::memcmp(site.target, site.patch.data(), site.size) != 0) {
            g_state.status.error = Error::PatchChanged;
            g_state.status.site_index = index;
            return false;
        }
    }
    for (std::size_t remaining = g_state.sites.size(); remaining != 0; --remaining) {
        const std::size_t index = remaining - 1;
        const auto &site = g_state.sites[index];
        if (write_code(site.target, site.original.data(), site.size)) continue;

        const DWORD restore_error = GetLastError();
        bool rolled_back = true;
        for (std::size_t rollback = index + 1; rollback < g_state.sites.size(); ++rollback) {
            const auto &restored_site = g_state.sites[rollback];
            if (!write_code(
                    restored_site.target, restored_site.patch.data(), restored_site.size)) {
                rolled_back = false;
            }
        }
        g_state.status.error = rolled_back ? Error::RestoreFailed : Error::PatchChanged;
        g_state.status.site_index = index;
        g_state.status.system_error = restore_error;
        return false;
    }
    VirtualFree(g_state.stub_block, 0, MEM_RELEASE);
    g_state.stub_block = nullptr;
    g_state.stub_block_size = 0;
    g_state.sites = {};
    g_state.installed = false;
    g_state.status.error = Error::None;
    return true;
}

bool restore_viewport_transaction() {
    return detail::restore_transaction(g_viewport_transaction);
}

TransactionStatus transaction_status() {
    TransactionStatus result{};
    result.active = InterlockedCompareExchange(
        reinterpret_cast<volatile LONG *>(&g_viewport_transaction.active), 1, 1) != 0;
    result.starts = static_cast<std::uint64_t>(InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64 *>(&g_viewport_transaction.starts), 0, 0));
    result.restores = static_cast<std::uint64_t>(InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64 *>(&g_viewport_transaction.restores), 0, 0));
    result.stale_recoveries = static_cast<std::uint64_t>(InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64 *>(
            &g_viewport_transaction.stale_recoveries), 0, 0));
    return result;
}

bool active_viewport_fov(float &fov_degrees) {
    if (InterlockedCompareExchange(
            reinterpret_cast<volatile LONG *>(&g_viewport_transaction.active), 1, 1) == 0) {
        return false;
    }
    const std::uint32_t absolute_bits =
        g_viewport_transaction.raw_distance_bits & 0x7fffffffU;
    float value = 0.0f;
    std::memcpy(&value, &absolute_bits, sizeof(value));
    float ignored_x = 0.0f;
    float ignored_y = 0.0f;
    if (!detail::perspective_scales(value, 1.0f, ignored_x, ignored_y)) {
        return false;
    }
    fov_degrees = value;
    return true;
}

bool is_installed() {
    ExclusiveLock guard(g_state.lock);
    return g_state.installed;
}

Status status() {
    ExclusiveLock guard(g_state.lock);
    return g_state.status;
}

const char *error_name(Error error) {
    switch (error) {
    case Error::None: return "none";
    case Error::UnsupportedArchitecture: return "unsupported-architecture";
    case Error::InvalidHostImage: return "invalid-host-image";
    case Error::UnsupportedHostVersion: return "unsupported-host-version";
    case Error::StubAllocationFailed: return "stub-allocation-failed";
    case Error::StubEncodingFailed: return "stub-encoding-failed";
    case Error::StubProtectionFailed: return "stub-protection-failed";
    case Error::PatchWriteFailed: return "patch-write-failed";
    case Error::PatchChanged: return "patch-changed";
    case Error::RestoreFailed: return "restore-failed";
    }
    return "unknown";
}

}  // namespace mmd931::float_fov_patch
