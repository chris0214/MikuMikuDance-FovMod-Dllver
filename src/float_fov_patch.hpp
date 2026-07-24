#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mmd931::float_fov_patch {

enum class Error {
    None,
    UnsupportedArchitecture,
    InvalidHostImage,
    UnsupportedHostVersion,
    StubAllocationFailed,
    StubEncodingFailed,
    StubProtectionFailed,
    PatchWriteFailed,
    PatchChanged,
    RestoreFailed,
};

struct Status {
    Error error = Error::None;
    const char *profile = nullptr;
    std::size_t site_index = 0;
    unsigned long system_error = 0;
};

bool install();
bool uninstall();
bool is_installed();
Status status();
const char *error_name(Error error);

namespace detail {

enum class StubKind {
    RenderFov,
    RenderDistance,
    ViewportFov,
    ViewportFrameTransaction,
};

struct alignas(8) ViewportTransactionState {
    std::int32_t active = 0;
    std::uint32_t raw_distance_bits = 0;
    std::uintptr_t host_state = 0;
    std::uint32_t distance_offset = 0;
    std::uint32_t reserved = 0;
    std::uint64_t starts = 0;
    std::uint64_t restores = 0;
    std::uint64_t stale_recoveries = 0;
};

struct SiteDefinition {
    const char *name;
    std::uint32_t rva;
    StubKind kind;
    std::array<std::uint8_t, 8> expected;
    std::uint8_t expected_size;
};

struct ProfileDefinition {
    const char *name;
    std::uint32_t distance_offset;
    std::array<SiteDefinition, 7> sites;
};

const std::array<ProfileDefinition, 3> &profiles();
const ProfileDefinition *select_profile(const std::uint8_t *image, std::size_t image_size);
bool build_stub(
    StubKind kind,
    std::uint32_t distance_offset,
    std::uintptr_t stub_address,
    std::uintptr_t return_address,
    std::vector<std::uint8_t> &output,
    std::uintptr_t transaction_state_address = 0);
bool build_patch(
    std::uintptr_t target_address,
    std::uintptr_t stub_address,
    std::size_t patch_size,
    std::vector<std::uint8_t> &output);

bool restore_transaction(ViewportTransactionState &state);
bool perspective_scales(
    float fov_degrees,
    float aspect,
    float &x_scale,
    float &y_scale);

}  // namespace detail

struct TransactionStatus {
    bool active = false;
    std::uint64_t starts = 0;
    std::uint64_t restores = 0;
    std::uint64_t stale_recoveries = 0;
};

bool restore_viewport_transaction();
TransactionStatus transaction_status();
bool active_viewport_fov(float &fov_degrees);

}  // namespace mmd931::float_fov_patch
