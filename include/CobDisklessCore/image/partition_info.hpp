// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include "CobDisklessCore/image/image_format.hpp"

namespace cobdiskless {

/**
 * @brief Detailed metadata for a detected disk partition.
 */
struct PartitionInfo {
    uint32_t index{0};
    uint64_t start_lba{0};
    uint64_t sector_count{0};
    uint64_t start_byte{0};
    uint64_t byte_size{0};

    uint8_t mbr_type{0};
    std::string gpt_type_guid;
    std::string gpt_unique_guid;
    std::string name;

    bool is_bootable{false};
    bool is_esp{false};

    FilesystemType filesystem{FilesystemType::Unknown};
};

} // namespace cobdiskless
