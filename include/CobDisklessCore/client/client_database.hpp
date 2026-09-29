// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "CobDisklessCore/client/machine_store.hpp"
#include "CobDisklessCore/errors/result.hpp"
#include "CobDisklessCore/types/network_address.hpp"

namespace cobdiskless {

/**
 * @brief One enrolled diskless client persisted in clients.json.
 */
struct ClientEntry {
    std::string mac_address;   // Normalized "AA:BB:CC:DD:EE:FF"
    std::string hostname;      // Uppercase NetBIOS/DNS name, 1-15 chars
    NetworkAddress ip_address; // Static/reserved address from the dynamic pool
    std::string base_image;    // Golden image assigned to this client
    bool super_client{false};  // Super Client (image maintenance) flag
};

/**
 * @brief Thread-safe, store-backed registry of enrolled clients.
 *
 * Responsibilities:
 *  - normalize/validate the two identity keys (MAC and hostname)
 *  - allocate the next free address from the configured pool while never
 *    colliding with the reserved server/gateway addresses or with addresses
 *    already handed out
 *  - persist every mutation so an unregistered MAC is only asked for a name
 *    once
 *
 * The pool itself is configuration, not registry data: it is set by the
 * owner of Configuration (see ClientDatabase::configure_pool) and is no
 * longer read back from the store. That keeps address-policy in one file
 * instead of letting a stale copy in the registry override it.
 */
class ClientDatabase {
public:
    /**
     * @brief Backs this registry with an existing store.
     *
     * Production shares one MachineStore with StationManager and
     * ClientManager, so an enrolled machine and the station it PXE-boots as
     * are one row rather than two files that can disagree about its address.
     */
    explicit ClientDatabase(std::shared_ptr<MachineStore> store);

    /**
     * @brief Convenience for callers that own their registry in isolation
     *        (tests, single-purpose tools): builds a private store at
     *        `db_file`, in the unified schema.
     */
    explicit ClientDatabase(std::filesystem::path db_file = "machines.json");

    // --- identity helpers -------------------------------------------------

    /** @brief Normalizes any common MAC spelling to "AA:BB:CC:DD:EE:FF". */
    [[nodiscard]] static std::string normalize_mac(std::string_view mac);

    /** @brief True when normalize_mac() produced a well-formed EUI-48. */
    [[nodiscard]] static bool is_valid_mac(std::string_view mac);

    /**
     * @brief Trims, upper-cases and validates a Windows computer name.
     *
     * Rules: 1-15 characters, alphanumeric plus hyphen, must start with an
     * alphanumeric character, and must not be a reserved DOS device name.
     *
     * @return the normalized name, or an InvalidConfiguration error.
     */
    [[nodiscard]] static Result<std::string> sanitize_hostname(std::string_view raw);

    // --- pool configuration ------------------------------------------------

    void configure_pool(NetworkAddress start, NetworkAddress end);

    /** @brief Addresses the allocator must never hand out (server, gateway...). */
    void set_reserved_addresses(std::vector<NetworkAddress> addresses);
    void add_reserved_address(NetworkAddress address);

    /**
     * @brief Sanitizes pool_start and existing client entries against reserved server/gateway IPs.
     * Persists changes to clients.json immediately if any collisions were resolved.
     * @return Number of sanitized client entries.
     */
    size_t sanitize_records();

    /** @brief Base image stamped onto newly enrolled clients. */
    void set_default_base_image(std::string image);

    // --- lookups ------------------------------------------------------------

    [[nodiscard]] std::optional<ClientEntry> find_client_by_mac(std::string_view mac) const;
    [[nodiscard]] std::optional<ClientEntry> find_client_by_hostname(std::string_view hostname) const;
    [[nodiscard]] bool is_hostname_taken(std::string_view hostname) const;

    // --- mutations ----------------------------------------------------------

    /**
     * @brief Enrolls an unregistered MAC under the requested hostname.
     *
     * Validates/normalizes both keys, rejects duplicate hostnames, allocates
     * the next free pool address and persists clients.json.
     *
     * @return the stored entry, or an error (invalid input, duplicate name,
     *         unknown/invalid MAC, exhausted pool).
     */
    Result<ClientEntry> enroll_client(std::string_view mac, std::string_view hostname);

    /** @brief Enrolls with an explicit base image instead of the default. */
    Result<ClientEntry> enroll_client(std::string_view mac,
                                      std::string_view hostname,
                                      std::string_view base_image);

    Result<void> set_super_client(std::string_view mac, bool super_client);
    Result<void> remove_client(std::string_view mac);

    // --- snapshots ----------------------------------------------------------

    [[nodiscard]] std::vector<ClientEntry> list_clients() const;
    [[nodiscard]] size_t client_count() const;

    [[nodiscard]] NetworkAddress pool_start() const;
    [[nodiscard]] NetworkAddress pool_end() const;
    [[nodiscard]] std::vector<NetworkAddress> reserved_addresses() const;
    [[nodiscard]] std::string default_base_image() const;

    // --- persistence --------------------------------------------------------

    Result<void> save() const;
    Result<void> load();

    /** @brief The machines.json backing this registry. */
    [[nodiscard]] const std::filesystem::path& db_file() const noexcept { return store_->store_file(); }
    [[nodiscard]] const std::shared_ptr<MachineStore>& store() const noexcept { return store_; }

private:
    // Guards pool configuration only -- the range, the reserved addresses and
    // the default image. Records live in store_, which owns its own lock.
    // Callers snapshot the pool before entering the store so the two mutexes
    // are never held at the same time in either order.
    mutable std::mutex mutex_;
    std::shared_ptr<MachineStore> store_;
    NetworkAddress pool_start_{0, 0, 0, 0};
    NetworkAddress pool_end_{0, 0, 0, 0};
    std::vector<NetworkAddress> reserved_;
    std::string default_base_image_;

    [[nodiscard]] static bool same_ip(const NetworkAddress& a, const NetworkAddress& b) noexcept;
};

} // namespace cobdiskless
