#include "float_fov_patch.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using mmd931::float_fov_patch::detail::ProfileDefinition;
using mmd931::float_fov_patch::detail::StubKind;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

std::uintptr_t jump_destination(
    const std::vector<std::uint8_t> &bytes,
    std::uintptr_t instruction_address,
    std::size_t jump_offset) {
    require(bytes.at(jump_offset) == 0xe9, "missing relative jump opcode");
    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes.data() + jump_offset + 1, sizeof(displacement));
    return instruction_address + jump_offset + 5 + displacement;
}

void place_profile(std::vector<std::uint8_t> &image, const ProfileDefinition &profile) {
    for (const auto &site : profile.sites) {
        std::copy_n(site.expected.begin(), site.expected_size, image.begin() + site.rva);
    }
}

void test_profile_selection() {
    const auto &profiles = mmd931::float_fov_patch::detail::profiles();
    std::vector<std::uint8_t> image(0x70000, 0xcc);
    place_profile(image, profiles[0]);
    require(
        mmd931::float_fov_patch::detail::select_profile(image.data(), image.size()) ==
            &profiles[0],
        "9.31 profile was not selected");
    image[profiles[0].sites[2].rva] ^= 0xff;
    require(
        mmd931::float_fov_patch::detail::select_profile(image.data(), image.size()) == nullptr,
        "corrupt profile was accepted");

    std::fill(image.begin(), image.end(), static_cast<std::uint8_t>(0xcc));
    place_profile(image, profiles[1]);
    require(
        mmd931::float_fov_patch::detail::select_profile(image.data(), image.size()) ==
            &profiles[1],
        "CE-table profile was not selected");

    std::fill(image.begin(), image.end(), static_cast<std::uint8_t>(0xcc));
    place_profile(image, profiles[2]);
    require(
        mmd931::float_fov_patch::detail::select_profile(image.data(), image.size()) ==
            &profiles[2],
        "9.26 profile was not selected");
}

void test_stub_encoding() {
    constexpr std::uintptr_t stub_address = 0x10000000;
    constexpr std::uintptr_t return_address = 0x10001234;
    std::vector<std::uint8_t> bytes;

    require(
        mmd931::float_fov_patch::detail::build_stub(
            StubKind::RenderFov, 0x000a1900, stub_address, return_address, bytes),
        "render FOV stub encoding failed");
    require(bytes.size() == 32, "render FOV stub has unexpected size");
    require(bytes[0] == 0x8b && bytes[6] == 0x25 && bytes[17] == 0xc7,
        "render FOV stub lost the CE-table operations");
    require(jump_destination(bytes, stub_address, bytes.size() - 5) == return_address,
        "render FOV return jump is wrong");

    require(
        mmd931::float_fov_patch::detail::build_stub(
            StubKind::RenderDistance, 0x000a1900, stub_address, return_address, bytes),
        "render distance stub encoding failed");
    require(bytes.size() == 17, "render distance stub has unexpected size");
    require(jump_destination(bytes, stub_address, bytes.size() - 5) == return_address,
        "render distance return jump is wrong");

    require(
        mmd931::float_fov_patch::detail::build_stub(
            StubKind::ViewportFov, 0x000a1900, stub_address, return_address, bytes),
        "viewport stub encoding failed");
    require(bytes.size() == 24 && bytes.front() == 0x50 && bytes[18] == 0x58,
        "viewport stub does not preserve RAX");
    require(jump_destination(bytes, stub_address, bytes.size() - 5) == return_address,
        "viewport return jump is wrong");

    constexpr std::uintptr_t transaction_state = 0x123456789abcdef0;
    require(
        mmd931::float_fov_patch::detail::build_stub(
            StubKind::ViewportFrameTransaction, 0x000a1900, stub_address,
            return_address, bytes, transaction_state),
        "viewport frame transaction stub encoding failed");
    require(bytes.size() == 122 && bytes[0] == 0x50 && bytes[1] == 0x51 &&
            bytes[2] == 0x52 && bytes[106] == 0x5a && bytes[108] == 0x58,
        "viewport frame transaction does not preserve scratch registers");
    std::uintptr_t encoded_transaction_state = 0;
    std::memcpy(
        &encoded_transaction_state, bytes.data() + 16,
        sizeof(encoded_transaction_state));
    require(encoded_transaction_state == transaction_state,
        "viewport frame transaction state address is wrong");
    std::uint32_t transaction_distance_offset = 0;
    std::memcpy(&transaction_distance_offset, bytes.data() + 43,
        sizeof(transaction_distance_offset));
    require(transaction_distance_offset == 0x000a1900,
        "stale transaction restore encoded the wrong distance offset");
    require(bytes[109] == 0x45 && bytes[110] == 0x38,
        "viewport frame transaction did not replay the original comparison");
    require(jump_destination(bytes, stub_address, bytes.size() - 5) == return_address,
        "viewport frame transaction return jump is wrong");

    require(
        mmd931::float_fov_patch::detail::build_stub(
            StubKind::RenderFov, 0x000a18f8, stub_address, return_address, bytes),
        "9.26 render FOV stub encoding failed");
    std::uint32_t encoded_offset = 0;
    std::memcpy(&encoded_offset, bytes.data() + 2, sizeof(encoded_offset));
    require(encoded_offset == 0x000a18f8, "9.26 distance offset was not encoded");
}

void test_transaction_restore() {
    std::vector<std::uint8_t> host(0x000a1904, 0xcc);
    constexpr std::uint32_t raw_bits = 0xc2099ac6;
    std::uint32_t zero = 0;
    std::memcpy(host.data() + 0x000a1900, &zero, sizeof(zero));

    mmd931::float_fov_patch::detail::ViewportTransactionState state{};
    state.active = 1;
    state.raw_distance_bits = raw_bits;
    state.host_state = reinterpret_cast<std::uintptr_t>(host.data());
    state.distance_offset = 0x000a1900;
    require(mmd931::float_fov_patch::detail::restore_transaction(state),
        "active transaction was not restored");
    std::uint32_t restored = 0;
    std::memcpy(&restored, host.data() + 0x000a1900, sizeof(restored));
    require(restored == raw_bits && state.active == 0 && state.restores == 1,
        "transaction restore did not preserve the raw carrier bits");
    require(!mmd931::float_fov_patch::detail::restore_transaction(state),
        "inactive transaction reported a second restore");

    state.active = 1;
    state.distance_offset = 0x1234;
    require(!mmd931::float_fov_patch::detail::restore_transaction(state) &&
            state.active == 0,
        "invalid transaction offset was not rejected and cleared");
}

void test_perspective_scales() {
    float x_scale = 0.0f;
    float y_scale = 0.0f;
    require(mmd931::float_fov_patch::detail::perspective_scales(
            60.0f, 16.0f / 9.0f, x_scale, y_scale),
        "valid perspective scales were rejected");
    require(std::fabs(y_scale - 1.7320508f) < 0.00001f &&
            std::fabs(x_scale - 0.9742786f) < 0.00001f,
        "perspective scales are numerically wrong");
    require(!mmd931::float_fov_patch::detail::perspective_scales(
            0.0f, 1.0f, x_scale, y_scale) &&
            !mmd931::float_fov_patch::detail::perspective_scales(
                180.0f, 1.0f, x_scale, y_scale) &&
            !mmd931::float_fov_patch::detail::perspective_scales(
                30.0f, 0.0f, x_scale, y_scale),
        "unsafe perspective input was accepted");
}

void test_patch_encoding() {
    constexpr std::uintptr_t target = 0x140059d17;
    constexpr std::uintptr_t stub = 0x140200000;
    std::vector<std::uint8_t> patch;
    require(
        mmd931::float_fov_patch::detail::build_patch(target, stub, 8, patch),
        "site patch encoding failed");
    require(patch.size() == 8 && patch[5] == 0x90 && patch[7] == 0x90,
        "site patch padding is wrong");
    require(jump_destination(patch, target, 0) == stub, "site patch jump is wrong");
    require(
        !mmd931::float_fov_patch::detail::build_patch(target, 0x7fff00000000, 8, patch),
        "out-of-range relative jump was accepted");
}

std::vector<std::uint8_t> map_pe_image(const std::string &path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "could not open MMD sample");
    std::vector<std::uint8_t> file{
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    require(file.size() >= sizeof(IMAGE_DOS_HEADER), "MMD sample is too small");
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(file.data());
    require(dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0,
        "MMD sample has an invalid DOS header");
    const auto nt_offset = static_cast<std::size_t>(dos->e_lfanew);
    require(nt_offset + sizeof(IMAGE_NT_HEADERS64) <= file.size(),
        "MMD sample has a truncated NT header");
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(file.data() + nt_offset);
    require(
        nt->Signature == IMAGE_NT_SIGNATURE &&
            nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC,
        "MMD sample is not a PE32+ image");
    std::vector<std::uint8_t> image(nt->OptionalHeader.SizeOfImage, 0);
    const std::size_t header_size = std::min<std::size_t>(
        nt->OptionalHeader.SizeOfHeaders, file.size());
    std::copy_n(file.begin(), header_size, image.begin());
    const auto *section = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const std::size_t source = section[index].PointerToRawData;
        const std::size_t destination = section[index].VirtualAddress;
        const std::size_t available = source < file.size() ? file.size() - source : 0;
        const std::size_t capacity = destination < image.size() ? image.size() - destination : 0;
        const std::size_t count = std::min<std::size_t>(
            section[index].SizeOfRawData, std::min(available, capacity));
        std::copy_n(file.begin() + source, count, image.begin() + destination);
    }
    return image;
}

void test_analyzed_mmd(const char *path, const char *expected_profile) {
    const auto image = map_pe_image(path);
    const auto *profile =
        mmd931::float_fov_patch::detail::select_profile(image.data(), image.size());
    require(profile != nullptr, "analyzed MMD sample did not match a supported profile");
    require(std::string(profile->name) == expected_profile,
        "analyzed MMD sample matched the wrong profile");
}

}  // namespace

int main(int argc, char **argv) {
    try {
        test_profile_selection();
        test_stub_encoding();
        test_patch_encoding();
        test_transaction_restore();
        test_perspective_scales();
        if (argc > 1) test_analyzed_mmd(argv[1], "MMD 9.31 x64 analyzed");
        if (argc > 2) test_analyzed_mmd(argv[2], "MMD 9.26 x64");
        std::cout << "float_fov_patch_fixture: PASS\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "float_fov_patch_fixture: FAIL: " << error.what() << '\n';
        return 1;
    }
}
