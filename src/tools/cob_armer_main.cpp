// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
// cob-armer - standalone offline SYSTEM hive armer for CobDiskless clients.
//
//   Usage: cob-armer <path_to_SYSTEM_hive>            (inspect, read-only)
//          cob-armer --arm <hive|volume|vhd>          (arm, then re-inspect)
//          cob-armer --self-test                      (synthetic round-trip)
//
// The tool is the human/automation entry point for the same arming pass the
// server runs after WinPE has injected NIC drivers into a client's target VHD.
// It is deliberately thin: all of the registry surgery lives in
// CobDisklessCore (RegBootStart), so the CLI, the HTTP callback and the tests
// can never drift apart.
//
// --arm passes the target to RegBootStart::arm_image_offline_pipeline(), which
// dispatches on what the path actually is: a .vhd/.vhdx is attached through the
// VirtDisk API, a loose regf hive is used as-is, and a mounted volume root is
// resolved to the SYSTEM hive underneath it. When the process is not elevated
// RegLoadKeyW fails and the pipeline falls back to the in-place regf writer, so
// --arm always does *something* useful but does less without Administrator
// rights -- the Stage 15 / CDDB / net-class half needs the hive loaded.
//
// Inspection never writes: no cell is allocated, no header is flushed, no
// transaction log is touched, no virtual disk is left attached.
//
// Exit codes:
//   0  armed and verified (or inspection completed and the hive is healthy)
//   1  unexpected internal failure
//   2  usage / bad arguments
//   3  path missing or not readable
//   4  file is not a regf hive
//   5  arming pass failed
//   6  arming finished but verification failed
//   7  inspection failed, or the hive is readable but not healthy
// ============================================================================

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "CobDisklessCore/config/simple_json.hpp"
#include "CobDisklessCore/image/reg_bootstart.hpp"

namespace {

namespace fs = std::filesystem;

using cobdiskless::Error;
using cobdiskless::HiveInspectionReport;
using cobdiskless::HiveNetBinding;
using cobdiskless::HiveServiceState;
using cobdiskless::JsonValue;
using cobdiskless::RegBootStart;

enum ExitCode {
    kExitOk = 0,
    kExitInternal = 1,
    kExitUsage = 2,
    kExitIo = 3,
    kExitNotHive = 4,
    kExitArmFailed = 5,
    kExitVerifyFailed = 6,
    kExitInspectFailed = 7,
};

void print_usage(std::ostream& out) {
    out << "Usage: cob-armer <path>                       inspect a hive (default)\n"
        << "       cob-armer --inspect <hive|volume|vhd>  read-only report\n"
        << "       cob-armer --arm <hive|volume|vhd>      run the arming pass, re-inspect\n"
        << "       cob-armer --self-test                  arm a synthetic hive and verify\n"
        << "\n"
        << "Options:\n"
        << "  --json         emit the report as JSON on stdout instead of text\n"
        << "  -h, --help     show this message\n"
        << "\n"
        << "Exit codes: 0 ok, 2 usage, 3 unreadable, 4 not regf, 5 arming failed,\n"
        << "            6 verification failed, 7 inspection failed/unhealthy, 1 internal\n";
}

// Map an inspection error onto the documented exit codes: 3 when the file is not
// there, 4 when it is there but is not a regf hive, 7 for anything else.
int inspect_exit_code(const Error& err) {
    const std::string message(err.message());
    if (message.find("not found") != std::string::npos ||
        message.find("No such file") != std::string::npos) {
        return kExitIo;
    }
    if (message.find("regf") != std::string::npos) {
        return kExitNotHive;
    }
    return kExitInspectFailed;
}

// Rows of the fixed table that the report shows as still unmet. This is the
// same bar arm_image_offline_pipeline() is expected to clear, so a pass here
// means the image will actually resolve its iSCSI stack over the network.
std::vector<std::string> unmet_arming(const HiveInspectionReport& report) {
    std::vector<std::string> gaps;
    if (!report.success) {
        gaps.push_back("inspection reports an unhealthy hive: " + report.message);
    }
    if (!report.walkable) {
        gaps.push_back("Services subkey list is not walkable: " + report.walkability_note);
    }

    for (const auto& spec : RegBootStart::boot_start_services()) {
        const HiveServiceState* state = nullptr;
        for (const auto& candidate : report.services) {
            if (candidate.service == spec.service) {
                state = &candidate;
                break;
            }
        }
        if (state == nullptr || !state->present) {
            if (spec.required) {
                gaps.push_back(std::string(spec.service) + ": required by the boot path but not "
                                                                 "carried by this image");
            }
            continue;
        }
        if (!state->has_start || state->start != 0) {
            gaps.push_back(std::string(spec.service) +
                           ": Start is not 0 (SERVICE_BOOT_START)");
            continue;
        }
        if (spec.require_network_boot_flag && !state->has_boot_flags) {
            gaps.push_back(std::string(spec.service) +
                           ": BootFlags (CM_SERVICE_NETWORK_BOOT_LOAD) is absent");
        }
    }
    return gaps;
}

// ---- Report emission --------------------------------------------------------

JsonValue report_to_json(const HiveInspectionReport& report) {
    JsonValue root = JsonValue::make_object();
    root["success"] = report.success;
    root["message"] = report.message;
    root["hive"] = report.hive_path.string();
    root["volume"] = report.volume_path.string();
    root["valid_header"] = report.valid_header;
    root["control_set"] = report.control_set;
    root["hive_dirty_on_entry"] = report.hive_dirty_on_entry;
    root["walkable"] = report.walkable;
    root["walkability_note"] = report.walkability_note;
    root["service_count"] = static_cast<unsigned long long>(report.service_count);

    JsonValue services = JsonValue::make_array();
    for (const auto& state : report.services) {
        JsonValue entry = JsonValue::make_object();
        entry["service"] = state.service;
        entry["present"] = state.present;
        entry["has_start"] = state.has_start;
        entry["start"] = static_cast<int>(state.start);
        entry["has_boot_flags"] = state.has_boot_flags;
        entry["boot_flags"] = static_cast<int>(state.boot_flags);
        entry["group"] = state.group;
        services.push_back(entry);
    }
    root["services"] = services;

    JsonValue bindings = JsonValue::make_array();
    for (const auto& binding : report.net_bindings) {
        JsonValue entry = JsonValue::make_object();
        entry["instance"] = binding.instance;
        entry["driver_desc"] = binding.driver_desc;
        entry["service"] = binding.service;
        bindings.push_back(entry);
    }
    root["net_bindings"] = bindings;

    root["has_crash_dump_enabled"] = report.has_crash_dump_enabled;
    root["crash_dump_enabled"] = static_cast<int>(report.crash_dump_enabled);
    root["paging_files_present"] = report.paging_files_present;
    root["tcpip_offload_disabled"] = report.tcpip_offload_disabled;
    root["tcpip_media_sense_disabled"] = report.tcpip_media_sense_disabled;
    root["wfplwf_disabled"] = report.wfplwf_disabled;
    root["has_disk_timeout"] = report.has_disk_timeout;
    root["disk_timeout"] = static_cast<int>(report.disk_timeout);
    root["mounted_c_present"] = report.mounted_c_present;
    root["cddb_entries"] = static_cast<unsigned long long>(report.cddb_entries);

    JsonValue findings = JsonValue::make_array();
    for (const auto& finding : report.findings) {
        findings.push_back(finding);
    }
    root["findings"] = findings;

    JsonValue unmet = JsonValue::make_array();
    for (const auto& gap : unmet_arming(report)) {
        unmet.push_back(gap);
    }
    root["unmet_arming"] = unmet;
    return root;
}

void emit_report(const HiveInspectionReport& report, bool json, std::ostream& out) {
    if (json) {
        out << report_to_json(report).dump(2) << "\n";
    } else {
        out << report.summary();
    }
}

// ---- Modes ------------------------------------------------------------------

int run_inspect(const fs::path& target, bool json) {
    auto result = RegBootStart::inspect_image(target);
    if (result.is_err()) {
        std::cerr << "[ERROR] " << result.error().message() << "\n";
        return inspect_exit_code(result.error());
    }

    const HiveInspectionReport& report = result.value();
    emit_report(report, json, std::cout);
    return report.success ? kExitOk : kExitInspectFailed;
}

int run_arm(const fs::path& target, bool json) {
    auto armed = RegBootStart::arm_image_offline_pipeline(target);
    if (armed.is_err()) {
        std::cerr << "[ERROR] " << armed.error().message() << "\n";
        return kExitArmFailed;
    }
    if (!armed.value().success) {
        std::cerr << "[ERROR] arming pass reported failure: " << armed.value().message << "\n";
        return kExitArmFailed;
    }
    std::cerr << "[OK] " << armed.value().summary() << "\n";

    // Verification is a fresh read of the same target: if the write did not
    // land (or landed and was rolled back by a transaction log), it shows here
    // rather than in the arming report, which only knows what it *tried*.
    auto after = RegBootStart::inspect_image(target);
    if (after.is_err()) {
        std::cerr << "[ERROR] could not re-inspect after arming: "
                  << after.error().message() << "\n";
        return kExitVerifyFailed;
    }

    const HiveInspectionReport& report = after.value();
    emit_report(report, json, std::cout);

    const std::vector<std::string> gaps = unmet_arming(report);
    if (gaps.empty()) {
        std::cerr << "[OK] verification passed: the iSCSI stack will resolve over the "
                     "network.\n";
        return kExitOk;
    }
    for (const auto& gap : gaps) {
        std::cerr << "[FAIL] " << gap << "\n";
    }
    return kExitVerifyFailed;
}

int run_self_test(bool json) {
    const std::vector<std::pair<std::string, uint32_t>> seeded = {
        {"msiscsi", 3}, {"iScsiPrt", 3}, {"Tcpip", 3}, {"ndis", 1},
    };

    std::vector<uint8_t> hive = RegBootStart::create_minimal_system_hive(1, seeded);
    if (hive.empty()) {
        std::cerr << "[FAIL] create_minimal_system_hive returned an empty buffer\n";
        return kExitInternal;
    }

    const fs::path temp = fs::temp_directory_path() / "cob_armer_selftest_SYSTEM.hive";
    std::error_code ec;
    // A leftover from an interrupted run may have been stamped read-only; make
    // sure the file we are about to write is writable before opening it.
    fs::permissions(temp,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add, ec);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[FAIL] cannot write " << temp << "\n";
            return kExitIo;
        }
        out.write(reinterpret_cast<const char*>(hive.data()),
                  static_cast<std::streamsize>(hive.size()));
        if (!out.good()) {
            std::cerr << "[FAIL] short write to " << temp << "\n";
            return kExitIo;
        }
    }

    int exit_code = kExitOk;
    do {
        auto before = RegBootStart::inspect_offline_hive(temp);
        if (before.is_err()) {
            std::cerr << "[FAIL] inspect before arming: " << before.error().message() << "\n";
            exit_code = kExitInspectFailed;
            break;
        }
        if (!before.value().success || !before.value().walkable) {
            std::cerr << "[FAIL] the synthetic hive is not structurally walkable\n";
            exit_code = kExitInspectFailed;
            break;
        }
        // The seed deliberately starts every service at a non-boot Start value,
        // so an arm pass that did nothing would be indistinguishable from a pass
        // that worked unless the "before" report shows the gap first.
        const std::vector<std::string> pre_arm = unmet_arming(before.value());
        if (pre_arm.empty()) {
            std::cerr << "[FAIL] the synthetic hive already looks armed; the test proves "
                         "nothing\n";
            exit_code = kExitInternal;
            break;
        }
        std::cerr << "[SELF-TEST] pre-arm gaps: " << pre_arm.size() << "\n";

        auto armed = RegBootStart::arm_offline_system_hive(temp);
        if (armed.is_err()) {
            std::cerr << "[FAIL] arming: " << armed.error().message() << "\n";
            exit_code = kExitArmFailed;
            break;
        }
        if (!armed.value().success) {
            std::cerr << "[FAIL] arming pass reported failure: " << armed.value().message << "\n";
            exit_code = kExitArmFailed;
            break;
        }

        auto after = RegBootStart::inspect_offline_hive(temp);
        if (after.is_err()) {
            std::cerr << "[FAIL] inspect after arming: " << after.error().message() << "\n";
            exit_code = kExitVerifyFailed;
            break;
        }

        const std::vector<std::string> gaps = unmet_arming(after.value());
        if (!gaps.empty()) {
            for (const auto& gap : gaps) {
                std::cerr << "[FAIL] " << gap << "\n";
            }
            std::cerr << after.value().summary();
            exit_code = kExitVerifyFailed;
            break;
        }

        // The report is what every caller consumes, so the serialised form is
        // validated too: parse the dump back and read the three fields an
        // automation hook keys off (success, services, unmet_arming).
        const std::string dumped = report_to_json(after.value()).dump(2);
        const std::optional<JsonValue> parsed = JsonValue::parse(dumped);
        if (!parsed.has_value()) {
            std::cerr << "[FAIL] the JSON report does not parse\n";
            exit_code = kExitInternal;
            break;
        }
        if (!(*parsed)["success"].as_bool()) {
            std::cerr << "[FAIL] the JSON report says success=false after a clean arm\n";
            exit_code = kExitInternal;
            break;
        }
        if ((*parsed)["unmet_arming"].size() != 0) {
            std::cerr << "[FAIL] the JSON report still lists unmet_arming entries\n";
            exit_code = kExitInternal;
            break;
        }
        if ((*parsed)["services"].size() != after.value().services.size()) {
            std::cerr << "[FAIL] the JSON report carries "
                      << (*parsed)["services"].size() << " of "
                      << after.value().services.size() << " services\n";
            exit_code = kExitInternal;
            break;
        }
        if (json) {
            std::cout << dumped << "\n";
        }

        std::cerr << "[OK] " << armed.value().summary()
                  << "[SELF-TEST] armed, verified, and the JSON report round-trips.\n";
    } while (false);

    fs::remove(temp, ec);
    return exit_code;
}

} // namespace

int main(int argc, char** argv) {
    bool json = false;
    bool arm = false;
    bool self_test = false;
    fs::path target;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
            return kExitOk;
        }
        if (arg == "--json") {
            json = true;
            continue;
        }
        if (arg == "--self-test") {
            self_test = true;
            continue;
        }
        if (arg == "--arm" || arg == "--inspect") {
            if (i + 1 >= argc) {
                std::cerr << "[ERROR] " << arg << " requires a path\n\n";
                print_usage(std::cerr);
                return kExitUsage;
            }
            arm = (arg == "--arm");
            target = argv[++i];
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "[ERROR] unknown option: " << arg << "\n\n";
            print_usage(std::cerr);
            return kExitUsage;
        }
        if (!target.empty()) {
            std::cerr << "[ERROR] unexpected extra argument: " << arg << "\n\n";
            print_usage(std::cerr);
            return kExitUsage;
        }
        target = arg;
    }

    if (self_test) {
        if (!target.empty()) {
            std::cerr << "[ERROR] --self-test takes no path\n\n";
            print_usage(std::cerr);
            return kExitUsage;
        }
        return run_self_test(json);
    }

    if (target.empty()) {
        print_usage(std::cerr);
        return kExitUsage;
    }

    std::error_code ec;
    if (!fs::exists(target, ec)) {
        std::cerr << "[ERROR] path does not exist: " << target << "\n";
        return kExitIo;
    }

    // `arm` is the only mode that writes; a bare path or --inspect never does.
    if (arm) {
        return run_arm(target, json);
    }
    return run_inspect(target, json);
}
