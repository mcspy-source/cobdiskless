// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <cctype>
#include <string>
#include <string_view>

namespace cobdiskless {

/**
 * @brief Canonical normalizer for boot_mode strings.
 * Maps aliases (csm, legacy, mbr, pcat, pcbios -> bios; uefi, efi -> uefi; rest -> auto).
 */
[[nodiscard]] inline std::string normalize_boot_mode(std::string_view raw) {
    std::string s;
    for (char c : raw) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (s == "bios" || s == "csm" || s == "legacy" || s == "mbr" || s == "pcat" || s == "pcbios") return "bios";
    if (s == "uefi" || s == "efi" || s == "efi64" || s == "efi32") return "uefi";
    return "auto";
}

/**
 * @brief Strongly typed byte size abstraction with unit helpers.
 */
class ByteSize {
public:
    constexpr ByteSize() noexcept = default;
    constexpr explicit ByteSize(uint64_t bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] constexpr uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] constexpr double kibibytes() const noexcept { return static_cast<double>(bytes_) / 1024.0; }
    [[nodiscard]] constexpr double mebibytes() const noexcept { return static_cast<double>(bytes_) / (1024.0 * 1024.0); }
    [[nodiscard]] constexpr double gibibytes() const noexcept { return static_cast<double>(bytes_) / (1024.0 * 1024.0 * 1024.0); }

    constexpr auto operator<=>(const ByteSize&) const noexcept = default;
    constexpr bool operator==(const ByteSize&) const noexcept = default;

private:
    uint64_t bytes_{0};
};

// User-defined literals for ByteSize
constexpr ByteSize operator""_KiB(unsigned long long int val) noexcept {
    return ByteSize(val * 1024ULL);
}

constexpr ByteSize operator""_MiB(unsigned long long int val) noexcept {
    return ByteSize(val * 1024ULL * 1024ULL);
}

constexpr ByteSize operator""_GiB(unsigned long long int val) noexcept {
    return ByteSize(val * 1024ULL * 1024ULL * 1024ULL);
}

/**
 * @brief Strongly typed Client Identifier.
 */
class ClientId {
public:
    ClientId() = default;
    explicit ClientId(std::string id) : id_(std::move(id)) {}

    [[nodiscard]] const std::string& value() const noexcept { return id_; }
    [[nodiscard]] bool empty() const noexcept { return id_.empty(); }

    auto operator<=>(const ClientId&) const noexcept = default;
    bool operator==(const ClientId&) const noexcept = default;

private:
    std::string id_;
};

/**
 * @brief Strongly typed Disk Image Identifier.
 */
class ImageId {
public:
    ImageId() = default;
    explicit ImageId(std::string id) : id_(std::move(id)) {}

    [[nodiscard]] const std::string& value() const noexcept { return id_; }
    [[nodiscard]] bool empty() const noexcept { return id_.empty(); }

    auto operator<=>(const ImageId&) const noexcept = default;
    bool operator==(const ImageId&) const noexcept = default;

private:
    std::string id_;
};

/**
 * @brief Strongly typed Boot Profile Identifier.
 */
class BootProfileId {
public:
    BootProfileId() = default;
    explicit BootProfileId(std::string id) : id_(std::move(id)) {}

    [[nodiscard]] const std::string& value() const noexcept { return id_; }
    [[nodiscard]] bool empty() const noexcept { return id_.empty(); }

    auto operator<=>(const BootProfileId&) const noexcept = default;
    bool operator==(const BootProfileId&) const noexcept = default;

private:
    std::string id_;
};

} // namespace cobdiskless
