// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CobDisklessCore/errors/result.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <string_view>
#include <optional>

namespace cobdiskless {

// Information about a harvested network driver package
struct DriverPackage {
    std::string inf_path;              // Full path to the .inf file
    std::string driver_name;           // Name from the INF (e.g., "Intel PRO/1000 Gigabit")
    std::string provider;              // Provider name from INF
    std::string version;               // Driver version from INF
    std::string class_guid;            // Class GUID (should be {4d36e972-e325-11ce-bfc1-08002be10318})
    std::string class_name;            // Class name (e.g., "Net")
    std::string service_name;          // Service name from INF (e.g., "rt640x64")
    std::vector<std::string> hardware_ids;  // Hardware IDs this driver matches
    std::filesystem::path driver_store_dir;  // Parent directory in DriverStore\FileRepository
    
    // Files in the package
    std::filesystem::path inf_file;
    std::vector<std::filesystem::path> sys_files;
    std::vector<std::filesystem::path> cat_files;
    std::vector<std::filesystem::path> other_files;
    
    // Architecture
    enum class Arch { Unknown, X86, X64, ARM64 };
    Arch architecture = Arch::Unknown;
    
    // Whether this is a boot-start capable network driver
    bool is_boot_capable = false;
};

// Result of harvesting drivers
struct DriverHarvestResult {
    std::vector<DriverPackage> packages;
    size_t total_scanned = 0;
    size_t net_class_found = 0;
    size_t boot_capable = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    
    [[nodiscard]] std::string summary() const;
};

// Configuration for the driver harvester
struct DriverHarvesterConfig {
    // Source directories to scan (default: C:\Windows\System32\DriverStore\FileRepository)
    std::vector<std::filesystem::path> source_dirs;
    
    // Output directory for staged drivers (default: ./drivers/net/)
    std::filesystem::path output_dir;
    
    // Target architecture filter (empty = all)
    std::string target_arch;  // "x64", "x86", "arm64"
    
    // Only harvest drivers with Class = Net
    bool net_class_only = true;
    
    // Recurse into subdirectories
    bool recurse = true;
    
    // Force include unsigned drivers
    bool force_unsigned = true;
};

// Harvester class for extracting network driver packages from DriverStore
class DriverHarvester {
public:
    explicit DriverHarvester(const DriverHarvesterConfig& config = {});
    
    // Harvest all network driver packages from configured source directories
    Result<DriverHarvestResult> harvest() const;
    
    // Harvest from a specific directory
    Result<DriverHarvestResult> harvest_from(const std::filesystem::path& dir) const;
    
    // Stage harvested packages to output directory (copies .inf/.sys/.cat files)
    Result<void> stage_packages(const DriverHarvestResult& result) const;
    
    // Get the default DriverStore path
    static std::filesystem::path default_driver_store_path();
    
    // Get architecture-specific DriverStore path
    static std::filesystem::path driver_store_path_for_arch(const std::string& arch);

private:
    DriverHarvesterConfig config_;
    
    // Parse an INF file to extract driver metadata
    static Result<DriverPackage> parse_inf(const std::filesystem::path& inf_path);
    
    // Check if INF is a network class driver
    static bool is_net_class_inf(const std::filesystem::path& inf_path);
    
    // Extract service name from INF
    static std::string extract_service_name(const std::filesystem::path& inf_path);
    
    // Extract hardware IDs from INF
    static std::vector<std::string> extract_hardware_ids(const std::filesystem::path& inf_path);
    
    // Determine architecture from INF or directory structure
    static DriverPackage::Arch detect_architecture(const std::filesystem::path& inf_path);
    
    // Check if driver is boot-capable
    static bool is_boot_capable(const DriverPackage& pkg);
    
    // Recursively scan directory for INF files
    void scan_directory(const std::filesystem::path& dir, DriverHarvestResult& result) const;
    
    // Copy driver package files to staging directory
    static Result<void> copy_package_files(const DriverPackage& pkg, const std::filesystem::path& dest_dir);
};

// Information about a mounted VHD
struct MountedVhdInfo {
    std::filesystem::path vhd_path;
    std::filesystem::path mount_point;  // e.g., "Z:" or "C:\mnt\vhd"
    std::wstring volume_path;           // e.g., "\\?\Volume{guid}\"
    bool read_only = false;
    bool is_vhdx = false;
};

// VHD Mounter using Virtual Disk API (virtdisk.lib)
class VhdMounter {
public:
    // Mount a VHD/VHDX file
    // If mount_point is empty, uses a temporary directory
    // If read_only is true, mounts read-only
    static Result<MountedVhdInfo> mount(const std::filesystem::path& vhd_path,
                                         const std::filesystem::path& mount_point = {},
                                         bool read_only = false);
    
    // Unmount a VHD
    static Result<void> unmount(const MountedVhdInfo& info);
    
    // Unmount by mount point
    static Result<void> unmount_by_mount_point(const std::filesystem::path& mount_point);
    
    // List all currently mounted VHDs
    static std::vector<MountedVhdInfo> list_mounted();

private:
    // Open virtual disk
    static Result<HANDLE> open_virtual_disk(const std::filesystem::path& vhd_path, bool read_only);
    
    // Attach virtual disk
    static Result<void> attach_virtual_disk(HANDLE vhd_handle, bool read_only);
    
    // Get volume path for attached disk
    static Result<std::wstring> get_volume_path(HANDLE vhd_handle);
    
    // Assign drive letter or mount point
    static Result<std::filesystem::path> assign_mount_point(const std::wstring& volume_path,
                                                             const std::filesystem::path& mount_point);
    
    // Detach virtual disk
    static Result<void> detach_virtual_disk(HANDLE vhd_handle);
    
    // Remove drive letter / mount point
    static Result<void> remove_mount_point(const std::filesystem::path& mount_point);
};

// DISM driver injection wrapper
class DismDriverInjector {
public:
    struct InjectionResult {
        bool success = false;
        int exit_code = -1;
        std::string stdout_output;
        std::string stderr_output;
        size_t drivers_injected = 0;
        std::vector<std::string> injected_drivers;
        std::vector<std::string> failed_drivers;
    };
    
    // Inject drivers into a mounted image
    // image_path: mount point of the VHD (e.g., "Z:" or "C:\mnt\vhd")
    // driver_dir: directory containing staged drivers (with .inf files)
    static Result<InjectionResult> inject_drivers(const std::filesystem::path& image_path,
                                                   const std::filesystem::path& driver_dir,
                                                   bool force_unsigned = true,
                                                   bool recurse = true);
    
    // Get DISM path (checks common locations)
    static std::filesystem::path find_dism_path();
    
    // Verify DISM is available
    static bool is_dism_available();

private:
    static std::string build_dism_command(const std::filesystem::path& image_path,
                                           const std::filesystem::path& driver_dir,
                                           bool force_unsigned,
                                           bool recurse);
};

// Full automated PnP driver injection pipeline
class PnpDriverPipeline {
public:
    struct PipelineConfig {
        // VHD to process
        std::filesystem::path vhd_path;
        
        // Driver source directory (empty = auto-detect from DriverStore)
        std::filesystem::path driver_source_dir;
        
        // Driver staging directory (default: ./drivers/net/)
        std::filesystem::path staging_dir = "./drivers/net";
        
        // Mount point for VHD (empty = auto-assign)
        std::filesystem::path mount_point;
        
        // Target architecture (empty = auto-detect from VHD)
        std::string target_arch;
        
        // Read-only mount (safer, but DISM needs write access for driver injection)
        bool read_only_mount = false;
        
        // Force unsigned drivers
        bool force_unsigned = true;
        
        // Recurse driver directories
        bool recurse_drivers = true;
        
        // Run offline registry arming after injection
        bool run_offline_arming = true;
        
        // Dry run (don't actually modify VHD)
        bool dry_run = false;
    };
    
    struct PipelineResult {
        bool success = false;
        std::string vhd_path;
        std::string mount_point;
        size_t drivers_harvested = 0;
        size_t drivers_staged = 0;
        size_t drivers_injected = 0;
        bool offline_arming_done = false;
        std::string error_message;
        std::vector<std::string> warnings;
        
        [[nodiscard]] std::string summary() const;
    };
    
    explicit PnpDriverPipeline(const PipelineConfig& config = {});
    
    // Run the full pipeline: harvest -> stage -> mount -> inject -> arm -> unmount
    Result<PipelineResult> run();
    
    // Run individual steps
    Result<DriverHarvestResult> harvest();
    Result<void> stage(const DriverHarvestResult& harvest);
    Result<MountedVhdInfo> mount();
    Result<DismDriverInjector::InjectionResult> inject(const MountedVhdInfo& mounted, const DriverHarvestResult& harvest);
    Result<PnpDriverArmReport> arm(const MountedVhdInfo& mounted);
    Result<void> unmount(const MountedVhdInfo& mounted);
    
    // Get current config
    const PipelineConfig& config() const { return config_; }

private:
    PipelineConfig config_;
    DriverHarvester harvester_;
    VhdMounter mounter_;
    DismDriverInjector injector_;
    
    // Helper to detect architecture from mounted VHD
    Result<std::string> detect_arch_from_mounted(const MountedVhdInfo& mounted);
};

} // namespace cobdiskless