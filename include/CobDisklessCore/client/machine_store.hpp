// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "CobDisklessCore/errors/result.hpp"
#include "CobDisklessCore/types/network_address.hpp"

namespace cobdiskless {

/**
 * @brief The single canonical record for one diskless machine.
 *
 * Before this type existed the same physical PC was described three times
 * over in three different files with three different spellings of every
 * field:
 *
 *   stations.json  StationEntry     -- what DHCP actually consults first
 *   clients.conf   ClientRecord     -- what the admin GUI edits ([CLIENT])
 *   clients.json   ClientEntry      -- what /api/enroll writes
 *   boot_states.json ClientBootState -- WinPE staging progress
 *
 * Those four now collapse into one record keyed by normalized MAC, stored
 * once in `machines.json`. Each field below records where it came from so
 * the legacy import can reproduce today's behaviour exactly instead of
 * inventing a new one.
 *
 * Nothing here is derived state: fields a legacy file never populated keep
 * their default, which is how the merge tells "absent" from "explicitly
 * cleared".
 */
struct MachineRecord {
    // --- identity (the map key is mac) -----------------------------------
    std::string mac;   // normalized "AA:BB:CC:DD:EE:FF"
    std::string name;  // stations.json:name | clients.conf:pc_name | clients.json:hostname
    NetworkAddress ip; // shared by all three

    // --- boot assignment --------------------------------------------------
    std::string image;        // stations.json:image | clients.conf:os_image_id | clients.json:base_image
    std::string image_path;   // clients.conf:os_image_path (explicit path override)
    std::string boot_mode{"auto"};       // stations.json ("auto" | "uefi" | "bios")
    std::string uefi_bootloader;         // stations.json (optional iPXE EFI bootloader)
    std::string last_arch;               // runtime architecture detected via DHCP Option 93 ("bios" | "uefi")

    // --- registration lifecycle (stations.json vocabulary) ----------------
    // "active" | "offline" | "error" | "disabled". Deliberately distinct from
    // boot_state below: this is whether the station is supposed to boot at
    // all, not where it is in the boot sequence.
    std::string status{"active"};
    bool enabled{true};       // clients.conf:enabled (stations encode this as status != "disabled")
    std::string group{"Default"}; // clients.conf:group
    std::string comment;      // clients.conf:comment

    // --- live boot state (clients.conf vocabulary) ------------------------
    // "Offline" | "Booting" | "Online".
    std::string boot_state{"Offline"};

    // --- WinPE staging progress (boot_states.json) ------------------------
    // "UNPROFILED" | "STAGING" | "READY" -- see ClientBootStatus.
    std::string provisioning{"UNPROFILED"};
    std::string target_vhd_path; // VHD (or mounted volume root) holding the OS
    std::string target_iqn;      // IQN of the iSCSI target serving that VHD

    // --- timestamps, canonicalized to ISO 8601 by the legacy import -------
    std::string registered_at; // stations.json
    std::string first_seen;    // clients.conf
    std::string last_seen;     // all three; import normalizes "YYYY-MM-DD HH:MM:SS"
    uint64_t first_seen_epoch{0};
    uint64_t last_seen_epoch{0};

    // --- network defaults (clients.conf) ----------------------------------
    NetworkAddress gateway{192, 168, 1, 1};
    NetworkAddress dns{192, 168, 1, 1};

    // --- enrollment (clients.json / clients.conf agree on one flag) -------
    bool super_client{false};

    // --- hardware validation & zero-touch enrollment ----------------------
    std::string uuid;
    bool is_validated{false};
    std::string hardware_id;
    std::string subsystem_id;
    std::string revision;
    std::string device_instance;
    std::string hardware_profile;
    std::string hardware_layer_path;
    std::string pci_topology;

    // --- per-machine storage and cache overrides --------------------------
    // These are operator-facing settings the admin GUI edits per client; the
    // runtime does not size caches from them today. They are carried across
    // verbatim so a migration never silently drops an operator's tuning.
    std::filesystem::path writeback_dir{"./writeback"};
    std::filesystem::path game_dir{"./games"};
    std::filesystem::path cache_dir{"./cache"};
    uint64_t cache_size_mb{512};
    uint64_t ram_cache_mb{512};
    uint64_t ssd_cache_mb{0};
    bool read_cache_enabled{true};
    bool write_cache_enabled{false};
    bool ram_cache_enabled{true};
    bool enable_write_cache{false};

    /// True when this record carries no information beyond its MAC.
    [[nodiscard]] bool is_empty() const noexcept;
};

/// Every machine the server knows, keyed by normalized MAC.
using MachineMap = std::unordered_map<std::string, MachineRecord>;
static_assert(std::is_default_constructible_v<MachineRecord>,
              "MachineRecord must default-construct: legacy imports rely on "
              "unset fields carrying their defaults to tell 'absent' from 'cleared'");

/**
 * @brief Thread-safe, JSON-backed store of every machine the server knows.
 *
 * One file (`machines.json`), one schema version, one lock. Writers persist
 * atomically -- write a sibling .tmp, flush, then rename over the target --
 * so a `taskkill /F` mid-save can never leave a half-written registry that
 * the next boot would parse as empty.
 *
 * Reads take a shared lock and are safe to run from the DHCP hot path;
 * writes take the exclusive lock only for the map update plus the rename.
 */
class MachineStore {
public:
    static constexpr uint32_t kSchemaVersion = 1;
    static constexpr const char* kSchemaName = "cobdiskless.machines";

    explicit MachineStore(std::filesystem::path store_file = "machines.json");

    // --- identity helpers -------------------------------------------------

    /** @brief Normalizes any common MAC spelling to "AA:BB:CC:DD:EE:FF". */
    [[nodiscard]] static std::string normalize_mac(std::string_view mac);
    /** @brief True when normalize_mac() produced a well-formed EUI-48. */
    [[nodiscard]] static bool is_valid_mac(std::string_view mac);

    /**
     * @brief Rewrites "YYYY-MM-DD HH:MM:SS" and bare ISO 8601 into "…Z".
     *
     * The three legacy stores each stamped their own format. This matters
     * because StationManager::parse_iso8601_to_epoch() scans for a literal
     * "T": a timestamp left in the clients.conf spelling parses as 0, which
     * would make every imported station look like it was last seen at the
     * Unix epoch and age it straight out of the TTL. Normalizing on import
     * means every consumer downstream parses exactly one spelling.
     *
     * Already-canonical input is returned unchanged, and anything that does
     * not fully parse is preserved verbatim -- losing the original text would
     * destroy the only copy of an operator's record for no gain.
     */
    [[nodiscard]] static std::string canonicalize_timestamp(std::string_view raw);

    /** @brief Parses either legacy spelling to a Unix epoch, 0 if unknown. */
    [[nodiscard]] static uint64_t timestamp_epoch(std::string_view raw);

    // --- lookups ----------------------------------------------------------

    [[nodiscard]] std::optional<MachineRecord> find(std::string_view mac) const;
    [[nodiscard]] bool contains(std::string_view mac) const;
    [[nodiscard]] std::optional<MachineRecord> find_by_name(std::string_view name) const;
    [[nodiscard]] std::optional<MachineRecord> find_by_ip(const NetworkAddress& ip) const;
    [[nodiscard]] std::vector<MachineRecord> list() const;
    [[nodiscard]] size_t size() const;

    // --- mutations --------------------------------------------------------

    /**
     * @brief Atomically read-modify-write the whole registry.
     *
     * The facades above this store (StationManager, ClientDatabase,
     * ClientManager) all follow the same shape: look a record up, decide
     * something about it, write it back. Splitting that into a find() followed
     * by a put() opens a window in which two DHCP DISCOVERs can both read "no
     * station named PC-07 yet" and both hand it out. mutate() closes it by
     * running `fn` under the store's exclusive lock against a private copy of
     * the map, then swapping the copy in and persisting it.
     *
     * `fn` is invoked as `Result<void> fn(MachineMap&)`. Returning an error
     * abandons the operation: memory and disk are both left exactly as they
     * were. Returning success commits, which always costs one file write --
     * even when `fn` changed nothing. That is deliberate; the alternative
     * (tracking dirtiness) would put a flag in every code path that matters.
     *
     * REENTRANCY: `fn` must use the map it is handed, not this store's public
     * methods. The store's lock is not recursive; calling find()/put() from
     * inside `fn` deadlocks on the first line.
     */
    template <typename Fn>
    Result<void> mutate(Fn&& fn) {
        static_assert(std::is_invocable_r_v<Result<void>, Fn, MachineMap&>,
                      "mutate() callbacks must be callable as Result<void>(MachineMap&)");
        std::unique_lock<std::shared_mutex> lock(mutex_);
        MachineMap scratch = machines_; // discarded wholesale if `fn` reports failure
        if (auto res = fn(scratch); res.is_err()) {
            return res.error();
        }
        machines_ = std::move(scratch);
        return save_locked();
    }

    /** @brief Replaces the record for `rec.mac`, or inserts it if new. */
    Result<void> put(const MachineRecord& rec);
    /** @brief Removes the record for a MAC. Unknown MACs are not an error. */
    Result<void> erase(std::string_view mac);
    /** @brief Drops every record (in memory only; call save() to persist). */
    void clear();

    // --- persistence ------------------------------------------------------

    /** @brief Reads machines.json. A missing file leaves the store empty. */
    Result<void> load();
    /** @brief Atomically rewrites machines.json from the in-memory map. */
    Result<void> save() const;

    /**
     * @brief One-time import of the pre-unification machine stores.
     *
     * Reads, in ascending precedence:
     *   1. clients.json      (lowest -- only /api/enroll ever wrote it)
     *   2. clients.conf      ([CLIENT] blocks -- what the GUI edits)
     *   3. stations.json     (highest -- what DHCP consults first)
     *   4. boot_states.json  (sole owner of staging progress)
     *
     * Each source only overlays the fields it actually defines, so a store
     * that never knew about a field cannot blank one a higher-priority store
     * set. That reproduces the runtime's existing lookup order rather than
     * imposing a new rule.
     *
     * Once the merged result is safely on disk, the legacy machine files are
     * deleted -- but only after `save()` succeeded, so an interrupted import
     * simply runs again on the next start.
     *
     * @return The number of distinct machines that ended up in the store.
     */
    Result<size_t> import_legacy(const std::filesystem::path& directory);

    /**
     * @brief `[DEFAULT_PROFILE]` key/values read from clients.conf.
     *
     * That section is server-wide defaults, not a machine, so it has no place
     * in this store -- but it must not be dropped either. import_legacy()
     * surfaces it here so the caller can fold it into cobdiskless.conf *before*
     * calling drop_legacy_client_conf(); losing it would silently reset every
     * operator-tuned default to the hard-coded fallbacks.
     *
     * Empty until import_legacy() has run.
     */
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& imported_default_profile()
        const noexcept {
        return imported_default_profile_;
    }

    /**
     * @brief Removes clients.conf once its records are in this store and its
     *        [DEFAULT_PROFILE] has been migrated by the caller.
     *
     * Deleting it here instead of in import_legacy() keeps the ordering
     * honest: the file with mixed ownership survives the step that owns only
     * half of it.
     */
    static Result<void> drop_legacy_client_conf(const std::filesystem::path& directory);

    /** @brief True when any pre-unification machine file is still present. */
    [[nodiscard]] static bool legacy_stores_present(const std::filesystem::path& directory);

    [[nodiscard]] const std::filesystem::path& store_file() const noexcept { return store_file_; }

    /** @brief File name this store reads and writes, relative to a directory. */
    [[nodiscard]] static std::filesystem::path path_in(const std::filesystem::path& directory);

private:
    mutable std::shared_mutex mutex_;
    std::filesystem::path store_file_;
    std::unordered_map<std::string, MachineRecord> machines_; // keyed by normalized MAC
    std::vector<std::pair<std::string, std::string>> imported_default_profile_;

    [[nodiscard]] Result<void> load_file_locked(const std::filesystem::path& path);
    [[nodiscard]] Result<void> save_locked() const;
};

} // namespace cobdiskless
