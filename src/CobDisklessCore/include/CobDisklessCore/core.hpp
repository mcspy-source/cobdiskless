#pragma once

#include <string_view>

namespace cobdiskless {

/**
 * @brief CobDisklessCore engine lifecycle skeleton.
 *
 * Represents the central lifecycle foundation of the CobDiskless platform.
 * Subsystems (DHCP, TFTP, Bootloader, Storage, and Caching) will be wired into
 * this core in subsequent phases.
 */
class Core {
public:
    Core();
    ~Core();

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    Core(Core&&) noexcept = default;
    Core& operator=(Core&&) noexcept = default;

    /**
     * @brief Initializes the CobDiskless engine lifecycle.
     * @return true if initialization was successful, false otherwise.
     */
    bool initialize();

    /**
     * @brief Shuts down the engine and releases resources.
     */
    void shutdown();

    /**
     * @brief Queries whether the engine is currently initialized.
     */
    [[nodiscard]] bool is_initialized() const noexcept;

    /**
     * @brief Returns the version string of the CobDiskless platform.
     */
    [[nodiscard]] static constexpr std::string_view version() noexcept {
        return "0.1.0-skeleton";
    }

private:
    bool initialized_{false};
};

} // namespace cobdiskless
