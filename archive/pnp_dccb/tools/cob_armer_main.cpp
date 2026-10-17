// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
// cob-armer - standalone offline SYSTEM hive armer for CobDiskless clients.
//
//   Usage: cob-armer <path_to_SYSTEM_hive>
//
// The tool is the human/automation entry point for the exact same arming pass
// that POST /api/client/staged runs automatically after WinPE has injected NIC
// drivers into a client's target VHD. It is deliberately thin: all of the
// registry surgery lives in CobDisklessCore so the CLI, the HTTP callback and
// the tests can never drift apart.
//
// The arming pass is native-image only. It flips the boot flags of the drivers
// this Windows image already carries (its own Net class binding and the iSCSI
// core stack), applies the boot Stage 15 neutralisation, the disk timeout and
// the Tcpip offload kill switch, and stops. It never demotes a service, never
// writes CriticalDeviceDatabase, never injects NDIS advanced properties and
// never copies a .sys in from a DriverStore package or a host depot.
//
// Exit codes:
//   0  armed and verified (or inspection completed)
//   2  usage / bad arguments
//   3  hive missing or not readable/writable
//   4  file is not a regf hive
//   5  arming pass failed
//   6  arming finished but verification failed
//   7  inspection failed / inspected hive was not healthy
//   1  unexpected internal failure
// ============================================================================

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "CobDisklessCore/config/simple_json.hpp"
#include "CobDisklessCore/image/pnp_driver_armer.hpp"

namespace {

using cobdiskless::JsonValue;
using cobdiskless::PnpDriverArmReport;
using cobdiskless::PnpDriverArmer;
using cobdiskless::PnpHiveInspectionReport;

constexpr int kExitOk = 0;
constexpr int kExitUnexpected = 1;
constexpr int kExitUsage = 2;
constexpr int kExitIo = 3;
constexpr int kExitNotAHive = 4;
constexpr int kExitArmingFailed = 5;
constexpr int kExitVerificationFailed = 6;
constexpr int kExitInspectFailed = 7;

const char* kUsage =
    "Usage: cob-armer <path_to_SYSTEM_hive> [options]\n"
    "       cob-armer --inspect <hive|volume|vhd> [--volume <root>]\n"
    "       cob-armer --self-test\n"
    "\n"
    "Arms an offline Windows SYSTEM hive for a 100% native Windows 10/11 iSCSI boot.\n"
    "A native image (disk2vhd of the machine it boots on) already carries its own NIC\n"
    "driver in System32\\drivers, so this pass ONLY:\n"
    "  * repairs the registry structure first: subkey lists that are out of name order\n"
    "    or carry a hash Windows cannot reproduce, and nk cells left with a zero where\n"
    "    Windows expects a cell offset. A key behind such a list parses fine and is\n"
    "    still invisible to Cm, so nothing below is trustworthy until this is fixed\n"
    "  * arms the NIC service bound under Control\\Class\\{4d36e972-...}\\<instance>\\Service\n"
    "    with Start = 0 and BootFlags = 0x00000001 (CM_SERVICE_NETWORK_BOOT_LOAD)\n"
    "  * arms the iSCSI boot stack: Tcpip and ndis must already exist and are forced to\n"
    "    Start = 0; msiscsi gets Start = 0 + BootFlags = 0x1 and is created with a full\n"
    "    definition when the image lacks it; iscsiprt is armed only when present\n"
    "  * neutralises Boot Stage 15: CrashControl dump stack off, PagingFiles emptied,\n"
    "    Services\\Tcpip\\Parameters\\DisableTaskOffload = 1\n"
    "  * raises Services\\disk\\TimeOutValue to 60s\n"
    "\n"
    "It never demotes a service to Start=3, never writes CriticalDeviceDatabase,\n"
    "never injects NDIS advanced properties and never copies a .sys into the image.\n"
    "\n"
    "Options:\n"
    "  --inspect <path>       read-only diagnostic dump of a SYSTEM hive, a mounted\n"
    "                         volume root, or a raw .vhd/.vhdx: first whether the hive's\n"
    "                         subkey lists are ones Windows can binary-search at all\n"
    "                         (reported as NOT WALKABLE, exit 7, when they are not),\n"
    "                         then Services\\<drv> Start / Group / BootFlags / Parameters,\n"
    "                         every binding under Control\\Class\\{4d36e972-...}, and the\n"
    "                         SHA-256 + PE version of the staged vs DriverStore copies\n"
    "                         of rt640x64.sys/e1d68x64.sys\n"
    "  --volume <root>        mount point that owns <path> (optional; derived from the\n"
    "                         hive path when it sits in the usual \\Windows\\System32\\config tree)\n"
    "  --self-test            arm a generated synthetic hive and verify the result\n"
    "  -h, --help             show this message\n"
    "\n"
    "Exit codes: 0 success, 2 usage, 3 I/O error, 4 not a regf hive,\n"
    "            5 arming failed, 6 verification failed, 7 inspection failed,\n"
    "            1 unexpected error\n";

/// Core iSCSI boot stack every fully armed image must list.
const std::vector<std::string>& required_arming_services() {
    static const std::vector<std::string> services = {
        "msiscsi", "Tcpip", "ndis",
    };
    return services;
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool contains_ci(const std::vector<std::string>& haystack, const std::string& needle) {
    const std::string wanted = lowercase(needle);
    return std::any_of(haystack.begin(), haystack.end(), [&](const std::string& entry) {
        return lowercase(entry) == wanted;
    });
}

/// True when the file exists, is a regular file and is both readable and writable.
bool check_openable(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        std::cerr << "[ERROR] Hive does not exist: " << path.string() << "\n";
        return false;
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        std::cerr << "[ERROR] Not a regular file: " << path.string() << "\n";
        return false;
    }
    if (std::filesystem::file_size(path, ec) < 0x1000 || ec) {
        std::cerr << "[ERROR] File is too small to be a SYSTEM hive: " << path.string() << "\n";
        return false;
    }

    // std::filesystem has no portable access() equivalent, so probe both modes.
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            std::cerr << "[ERROR] Hive is not readable (check permissions): " << path.string()
                      << "\n";
            return false;
        }
    }
    {
        std::fstream out(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!out.is_open()) {
            std::cerr << "[ERROR] Hive is not writable (check permissions): " << path.string()
                      << "\n";
            return false;
        }
    }
    return true;
}

/// Validates the 4-byte "regf" signature every Windows registry hive starts with.
bool check_regf_header(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    char signature[4] = {};
    if (!in.read(signature, sizeof(signature))) {
        std::cerr << "[ERROR] Cannot read hive header from: " << path.string() << "\n";
        return false;
    }
    if (std::string_view(signature, sizeof(signature)) != "regf") {
        std::cerr << "[ERROR] Not a Windows registry hive (missing 'regf' header): "
                  << path.string() << "\n";
        return false;
    }
    return true;
}

JsonValue string_array(const std::vector<std::string>& items) {
    JsonValue array = JsonValue::make_array();
    for (const auto& item : items) {
        array.push_back(JsonValue(item));
    }
    return array;
}

/// Serializes the full PnpDriverArmReport as machine-readable JSON.
JsonValue report_to_json(const PnpDriverArmReport& report) {
    JsonValue root = JsonValue::make_object();
    root["success"] = report.success;
    root["target"] = report.target_path.string();
    root["control_set"] = report.control_set;
    root["boot_flags_armed"] = report.boot_flags_armed;
    root["iscsi_stack_armed"] = report.iscsi_stack_armed;
    root["armed_services"] = string_array(report.armed_services);
    // "updated_services" is the operator-facing alias for every service touched.
    root["updated_services"] = string_array(report.armed_services);
    root["modified_services"] = string_array(report.modified_services);
    root["missing_services"] = string_array(report.missing_services);
    root["crash_dump_disabled"] = report.crash_dump_disabled;
    root["paging_files_cleared"] = report.paging_files_cleared;
    root["tcpip_offload_disabled"] = report.tcpip_offload_disabled;
    root["disk_timeout_armed"] = report.disk_timeout_armed;
    root["hive_dirty_on_entry"] = report.hive_dirty_on_entry;
    root["hive_cleaned_on_exit"] = report.hive_cleaned_on_exit;
    root["guest_arch"] = report.guest_arch;
    root["guest_os_family"] = report.guest_os_family;
    root["warnings"] = string_array(report.warnings);
    root["message"] = report.message;
    root["summary"] = report.summary();
    return root;
}

/// Prints the human-readable report block on stderr, the JSON document on stdout.
void emit_report(const PnpDriverArmReport& report) {
    std::cerr << "[INFO] " << report.summary() << "\n";

    std::cerr << "[INFO] Armed services (" << report.armed_services.size() << "):";
    if (report.armed_services.empty()) {
        std::cerr << " <none>";
    }
    for (const auto& service : report.armed_services) {
        std::cerr << " " << service;
    }
    std::cerr << "\n";

    std::cerr << "[INFO] Missing services (" << report.missing_services.size() << "):";
    if (report.missing_services.empty()) {
        std::cerr << " <none>";
    }
    for (const auto& service : report.missing_services) {
        std::cerr << " " << service;
    }
    std::cerr << "\n";

    std::cerr << "[INFO] Guest OS family: " << (report.guest_os_family.empty() ? "unknown" : report.guest_os_family)
              << ", arch: " << (report.guest_arch.empty() ? "unknown" : report.guest_arch)
              << " (informational only -- this pass injects nothing)\n";

    std::cerr << "[INFO] boot_flags_armed=" << (report.boot_flags_armed ? "true" : "false")
              << " iscsi_stack_armed=" << (report.iscsi_stack_armed ? "true" : "false")
              << " stage15(crash_dump=" << (report.crash_dump_disabled ? "true" : "false")
              << ", paging_files=" << (report.paging_files_cleared ? "true" : "false")
              << ", tcpip_offload=" << (report.tcpip_offload_disabled ? "true" : "false")
              << ") disk_timeout=" << (report.disk_timeout_armed ? "true" : "false") << "\n";

    if (report.hive_dirty_on_entry) {
        std::cerr << "[INFO] Hive arrived dirty; header forced clean on flush ("
                  << (report.hive_cleaned_on_exit ? "sequence numbers synchronized"
                                                  : "sequence numbers NOT synchronized")
                  << ")\n";
    }

    for (const auto& warning : report.warnings) {
        std::cerr << "[WARN] " << warning << "\n";
    }

    std::cout << report_to_json(report).dump(2) << std::endl;
}

/// Serializes the read-only inspection report as machine-readable JSON.
JsonValue inspection_to_json(const PnpHiveInspectionReport& report) {
    JsonValue root = JsonValue::make_object();
    root["success"] = report.success;
    root["hive"] = report.hive_path.string();
    root["volume"] = report.volume_path.string();
    root["control_set"] = report.control_set;
    root["guest_os_family"] = report.guest_os_family;
    root["message"] = report.message;

    JsonValue services = JsonValue::make_array();
    for (const auto& service : report.nic_services) {
        JsonValue entry = JsonValue::make_object();
        entry["service"] = service.service_name;
        entry["present"] = service.present;
        entry["start"] = service.start;
        entry["group"] = service.group;
        entry["has_boot_flags"] = service.has_boot_flags;
        entry["boot_flags"] = service.boot_flags;
        entry["has_parameters"] = service.has_parameters;
        entry["parameter_values"] = string_array(service.parameter_values);
        entry["parameter_subkeys"] = string_array(service.parameter_subkeys);
        services.push_back(entry);
    }
    root["services"] = services;

    JsonValue adapters = JsonValue::make_array();
    for (const auto& adapter : report.net_adapters) {
        JsonValue entry = JsonValue::make_object();
        entry["subkey"] = adapter.subkey;
        entry["driver_desc"] = adapter.driver_desc;
        entry["matching_device_id"] = adapter.matching_device_id;
        entry["component_id"] = adapter.component_id;
        adapters.push_back(entry);
    }
    root["net_class_bindings"] = adapters;

    JsonValue drivers = JsonValue::make_array();
    for (const auto& driver : report.driver_files) {
        JsonValue entry = JsonValue::make_object();
        entry["service"] = driver.service_name;
        entry["file"] = driver.driver_file;
        entry["deployed"] = driver.deployed.string();
        entry["deployed_present"] = driver.deployed_present;
        entry["deployed_sha256"] = driver.deployed_sha256;
        entry["deployed_version"] = driver.deployed_version;
        entry["source"] = driver.source.string();
        entry["source_present"] = driver.source_present;
        entry["source_sha256"] = driver.source_sha256;
        entry["source_version"] = driver.source_version;
        entry["identical"] = driver.identical;
        drivers.push_back(entry);
    }
    root["driver_files"] = drivers;

    root["findings"] = string_array(report.findings);
    root["summary"] = report.summary();
    return root;
}

/// Prints the human-readable inspection block on stderr, the JSON document on stdout.
void emit_inspection(const PnpHiveInspectionReport& report) {
    std::cerr << "[INFO] " << report.summary() << "\n";
    for (const auto& line : report.findings) {
        if (line.rfind("FATAL:", 0) == 0 || line.rfind("MISMATCH", 0) != std::string::npos) {
            std::cerr << "[WARN] " << line << "\n";
        }
    }
    std::cout << inspection_to_json(report).dump(2) << std::endl;
}

/// Verifies the guarantees the pipeline depends on: the iSCSI core stack is boot-start,
/// every armed network service carries BootFlags = 0x1, and Boot Stage 15 plus the disk
/// timeout / offload tweaks landed.
bool verify_report(const PnpDriverArmReport& report, std::string& reason) {
    if (!report.success) {
        reason = report.message.empty() ? "arming pass reported failure" : report.message;
        return false;
    }
    if (!report.boot_flags_armed) {
        reason = "BootFlags = 0x1 (CM_SERVICE_NETWORK_BOOT_LOAD) missing on a network service";
        return false;
    }
    if (!report.iscsi_stack_armed) {
        reason = "iSCSI boot stack (msiscsi/Tcpip/ndis) is not boot-start";
        return false;
    }
    for (const auto& service : required_arming_services()) {
        if (!contains_ci(report.armed_services, service)) {
            reason = "service was not armed: " + service;
            return false;
        }
    }
    if (!report.crash_dump_disabled) {
        reason = "Boot Stage 15 crash dump stack could not be neutralised";
        return false;
    }
    if (!report.paging_files_cleared) {
        reason = "Boot Stage 15 paging file could not be cleared on the boot volume";
        return false;
    }
    if (!report.tcpip_offload_disabled) {
        reason = "Services\\Tcpip\\Parameters\\DisableTaskOffload = 1 was not applied";
        return false;
    }
    if (!report.disk_timeout_armed) {
        reason = "Services\\disk\\TimeOutValue = 60 was not applied";
        return false;
    }
    return true;
}

int run_arm(const std::filesystem::path& target_path) {
    const std::string ext = lowercase(target_path.extension().string());
    if (ext == ".vhd" || ext == ".vhdx") {
        std::cerr << "[INFO] Arming offline VHD image: " << target_path.string() << "\n";
        auto armed = PnpDriverArmer::arm_vhd_image(target_path);
        if (armed.is_err()) {
            std::cerr << "[ERROR] Arming failed: " << armed.error().message() << "\n";
            return kExitArmingFailed;
        }
        const PnpDriverArmReport& report = armed.value();
        emit_report(report);
        std::string reason;
        if (!verify_report(report, reason)) {
            std::cerr << "[ERROR] Verification failed: " << reason << "\n";
            return kExitVerificationFailed;
        }
        std::cerr << "[INFO] VHD image armed and verified.\n";
        return kExitOk;
    }

    if (!check_openable(target_path)) {
        return kExitIo;
    }
    if (!check_regf_header(target_path)) {
        return kExitNotAHive;
    }

    std::cerr << "[INFO] Arming offline SYSTEM hive: " << target_path.string() << "\n";

    auto armed = PnpDriverArmer::arm_offline_system_hive(target_path);
    if (armed.is_err()) {
        std::cerr << "[ERROR] Arming failed: " << armed.error().message() << "\n";
        return kExitArmingFailed;
    }

    const PnpDriverArmReport& report = armed.value();
    emit_report(report);

    std::string reason;
    if (!verify_report(report, reason)) {
        std::cerr << "[ERROR] Verification failed: " << reason << "\n";
        return kExitVerificationFailed;
    }

    std::cerr << "[INFO] Hive armed and verified.\n";
    return kExitOk;
}

/// Read-only diagnostic dump: services, Net class bindings, driver binary identity.
int run_inspect(const std::filesystem::path& target, const std::filesystem::path& volume) {
    std::cerr << "[INFO] Inspecting offline image: " << target.string() << "\n";

    auto inspected = volume.empty() ? PnpDriverArmer::inspect_vhd_image(target)
                                    : PnpDriverArmer::inspect_offline_hive(target, volume);
    if (inspected.is_err()) {
        std::cerr << "[ERROR] Inspection failed: " << inspected.error().message() << "\n";
        return kExitInspectFailed;
    }

    const PnpHiveInspectionReport& report = inspected.value();
    emit_inspection(report);
    if (!report.success) {
        std::cerr << "[ERROR] Inspection completed but the hive is not healthy: "
                  << report.message << "\n";
        return kExitInspectFailed;
    }
    std::cerr << "[INFO] Inspection complete.\n";
    return kExitOk;
}

/// Generates a synthetic SYSTEM hive, arms it, and verifies the result end to end.
int run_self_test() {
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() / "cob_armer_self_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);
    if (ec) {
        std::cerr << "[ERROR] Cannot create temp directory: " << temp_dir.string() << "\n";
        return kExitUnexpected;
    }

    const std::filesystem::path hive_path = temp_dir / "SYSTEM";

    // Every service starts disabled/demand so the armer has to promote all of them.
    // A Net class adapter instance bound to rt640x64 stands in for the NIC the image
    // itself installed -- the one binding the native armer reads.
    std::vector<std::pair<std::string, uint32_t>> services = {
        {"msiscsi", 3}, {"Tcpip", 3}, {"ndis", 3},
    };
    PnpDriverArmer::SyntheticNetAdapter adapter;
    adapter.subkey_name = "0000";
    adapter.driver_desc = "Realtek PCIe GbE Family Controller";
    adapter.matching_device_id = "pci\\ven_10ec&dev_8168";
    adapter.service = "rt640x64";
    services.emplace_back("rt640x64", 3);

    std::vector<uint8_t> hive =
        PnpDriverArmer::create_synthetic_system_hive(1, services, {adapter});
    if (hive.empty()) {
        std::cerr << "[ERROR] Failed to generate a synthetic SYSTEM hive\n";
        return kExitUnexpected;
    }

    {
        std::ofstream out(hive_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[ERROR] Cannot write synthetic hive: " << hive_path.string() << "\n";
            return kExitIo;
        }
        out.write(reinterpret_cast<const char*>(hive.data()),
                  static_cast<std::streamsize>(hive.size()));
        out.flush();
        if (!out.good()) {
            std::cerr << "[ERROR] Short write while creating synthetic hive\n";
            return kExitIo;
        }
    }

    const int exit_code = run_arm(hive_path);

    std::filesystem::remove_all(temp_dir, ec);
    if (exit_code == kExitOk) {
        std::cerr << "[INFO] Self test passed.\n";
    }
    return exit_code;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    std::filesystem::path target;
    std::filesystem::path volume;
    bool has_target = false;
    bool inspect = false;
    bool self_test = false;

    // An option "takes" the next token when that token does not itself look like a flag,
    // so `cob-armer --inspect <path>` and `cob-armer <path> --inspect` both parse the way
    // an operator expects.
    auto takes_value = [&](size_t index) {
        return index + 1 < args.size() && (args[index + 1].empty() || args[index + 1][0] != '-');
    };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (arg == "-h" || arg == "--help") {
            std::cout << kUsage;
            return kExitOk;
        }
        if (arg == "--self-test") {
            self_test = true;
            continue;
        }
        if (arg == "--arm") {
            if (takes_value(i)) {
                target = args[++i];
                has_target = true;
            }
            continue;
        }
        if (arg == "--inspect") {
            inspect = true;
            if (takes_value(i)) {
                target = args[++i];
                has_target = true;
            }
            continue;
        }
        if (arg == "--volume") {
            if (!takes_value(i)) {
                std::cerr << "cob-armer: --volume requires a directory argument\n\n" << kUsage;
                return kExitUsage;
            }
            volume = args[++i];
            continue;
        }
        if (!arg.empty() && arg[0] == '-' && arg != "-") {
            std::cerr << "cob-armer: unknown option '" << arg << "'\n\n" << kUsage;
            return kExitUsage;
        }

        if (has_target) {
            std::cerr << "cob-armer: unexpected extra argument '" << arg << "'\n\n" << kUsage;
            return kExitUsage;
        }
        target = arg;
        has_target = true;
    }

    if (self_test) {
        return run_self_test();
    }

    if (!has_target) {
        std::cerr << "cob-armer: missing required argument <path_to_SYSTEM_hive>\n\n" << kUsage;
        return kExitUsage;
    }

    try {
        if (inspect) {
            return run_inspect(target, volume);
        }
        return run_arm(target);
    } catch (const std::exception& ex) {
        std::cerr << "[ERROR] Unexpected failure: " << ex.what() << "\n";
        return kExitUnexpected;
    }
}
