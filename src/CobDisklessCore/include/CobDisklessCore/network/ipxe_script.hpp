#pragma once

#include <cstdint>
#include <string>

namespace cobdiskless::ipxe {

/**
 * @brief Content type used for every dynamically generated iPXE script.
 *
 * iPXE sniffs the "#!ipxe" magic rather than trusting the header, but the
 * correct type keeps intermediate proxies and logs readable.
 */
inline constexpr const char* kScriptContentType = "text/plain; charset=utf-8";

/**
 * @brief Options for the SANBoot script returned to an already-enrolled MAC.
 *
 * The initiator identity is derived from the NIC MAC so a single golden image
 * can serve every diskless client while each one still presents a unique IQN
 * to the iSCSI target (which keys its COW writeback session on it).
 */
struct SanbootOptions {
    /// Server IP, hostname, or native iPXE variable (defaults to "${next-server}").
    std::string server_ip{"${next-server}"};
    /// Prefix for the synthesized initiator IQN.
    std::string initiator_prefix{"iqn.2026-09.cobdiskless:"};
    /// iPXE setting expanded at boot time (colon-delimited hex MAC).
    std::string initiator_suffix{"${net0/mac:hex}"};
    /// IQN of the iSCSI target to hook.
    std::string target_iqn{"iqn.2026-09.com.cobdiskless:storage"};
};

/**
 * @brief Builds the script handed to a client for SAN booting.
 *
 * Minimal and clean: attaches and boots the primary OS drive.
 * Secondary game drives are mounted inside Windows via Microsoft iSCSI Initiator.
 *
 * Emits strictly:
 * #!ipxe
 * set keep-san 1
 * sanboot iscsi:<server_ip>::::<target_iqn>
 */
[[nodiscard]] inline std::string sanboot_script(const SanbootOptions& options = {}) {
    std::string script;
    script += "#!ipxe\n";
    script += "set keep-san 1\n";
    std::string srv = options.server_ip.empty() ? std::string("${next-server}") : options.server_ip;
    if (srv == "${server_ip}") {
        srv = "${next-server}";
    }
    const std::string iqn = options.target_iqn.empty() ? std::string("iqn.2026-09.com.cobdiskless:storage") : options.target_iqn;
    script += "sanboot iscsi:" + srv + "::::" + iqn + "\n";
    return script;
}

/**
 * @brief Options for the interactive pre-boot enrollment prompt.
 */
struct EnrollmentScriptOptions {
    /// Absolute enroll endpoint, e.g. "http://192.168.1.10:8080/api/enroll".
    std::string enroll_url;
    /// iPXE setting that expands to the client MAC inside the URL.
    std::string mac_setting{"${net0/mac}"};
};

/**
 * @brief Builds the script served to an unknown/unregistered MAC.
 *
 * Uses the native iPXE `read` command to collect the computer name and then
 * chains to /api/enroll, which answers with the SANBoot script on success so
 * the machine continues booting without a power cycle.
 *
 * A rejected name (HTTP 4xx) makes `chain` return non-zero, which lands on
 * :enroll_failed and re-prompts instead of leaving a dead console.
 */
[[nodiscard]] inline std::string enrollment_script(const EnrollmentScriptOptions& options) {
    std::string script;
    script += "#!ipxe\n";
    script += "echo ========================================================\n";
    script += "echo        CobDiskless: Unregistered PC Detected!\n";
    script += "echo        MAC Address: " + options.mac_setting + "\n";
    script += "echo ========================================================\n";
    script += ":ask\n";
    script += "echo -n Enter Computer Name (e.g. PC-01): \n";
    script += "read pc_name || goto ask\n";
    script += "echo\n";
    script += "echo Enrolling ${pc_name} with CobDiskless Server...\n";
    script += "chain " + options.enroll_url + "?mac=" + options.mac_setting +
              "&name=${pc_name} || goto enroll_failed\n";
    script += "goto ask\n";
    script += ":enroll_failed\n";
    script += "echo Enrollment failed: use 1-15 characters (A-Z, 0-9, hyphen) "
              "that are not already taken.\n";
    script += "prompt Press any key to try again...\n";
    script += "goto ask\n";
    return script;
}

/**
 * @brief Options for the Legacy BIOS (MBR) SAN boot script served to READY clients.
 *
 * Both fields default to empty, which keeps the iPXE variable reference
 * (`${server_ip}` / `${target_iqn}`) verbatim in the emitted script. Passing a
 * concrete value expands it instead, so one server can hand out a script that
 * survives an IP or target-IQN change without re-rendering the script itself.
 */
struct LegacySanbootOptions {
    /// Empty -> emit the literal `${server_ip}` iPXE variable.
    std::string server_ip;
    /// Empty -> emit the literal `${target_iqn}` iPXE variable.
    std::string target_iqn;
};

/**
 * @brief Builds the Legacy BIOS MBR script for a client that is READY to SAN boot.
 *
 * `sanhook` attaches the OS drive to drive 0x80 without booting it (so the
 * firmware-visible disk is stable) and `sanboot --no-describe` then boots it.
 *
 * Emits strictly:
 * #!ipxe
 * sanhook --drive 0x80 iscsi:${server_ip}::::${target_iqn}
 * sanboot --no-describe
 */
[[nodiscard]] inline std::string legacy_mbr_sanboot_script(const LegacySanbootOptions& options = {}) {
    const std::string server = options.server_ip.empty() ? std::string("${server_ip}")
                                                         : options.server_ip;
    const std::string iqn = options.target_iqn.empty() ? std::string("${target_iqn}")
                                                       : options.target_iqn;
    std::string script;
    script += "#!ipxe\n";
    script += "sanhook --drive 0x80 iscsi:" + server + "::::" + iqn + "\n";
    script += "sanboot --no-describe\n";
    return script;
}

/**
 * @brief Options for the in-RAM WinPE `wimboot` staging script.
 *
 * Empty defaults keep the `${server_ip}` / `${port}` iPXE variables verbatim.
 */
struct WimbootOptions {
    /// Empty -> emit the literal `${server_ip}` iPXE variable.
    std::string server_ip;
    /// 0     -> emit the literal `${port}` iPXE variable.
    uint16_t port{0};
    /// Prefix the HTTP asset URLs are served from (no trailing slash).
    std::string asset_base{"/static/wim"};
};

/**
 * @brief Builds the Legacy BIOS script that boots the in-RAM WinPE staging image.
 *
 * wimboot loads bootmgr/bcd/boot.sdi/boot.wim straight into RAM, so the client
 * never touches the target VHD while drivers are being injected into it.
 *
 * Emits strictly:
 * #!ipxe
 * kernel http://${server_ip}:${port}/static/wim/wimboot
 * initrd http://${server_ip}:${port}/static/wim/bootmgr <alias>
 * initrd http://${server_ip}:${port}/static/wim/bcd     <alias>
 * initrd http://${server_ip}:${port}/static/wim/boot.sdi <alias>
 * initrd http://${server_ip}:${port}/static/wim/boot.wim <alias>
 * boot
 */
[[nodiscard]] inline std::string wimboot_script(const WimbootOptions& options = {}) {
    const std::string server = options.server_ip.empty() ? std::string("${server_ip}")
                                                         : options.server_ip;
    const std::string port = (options.port != 0) ? std::to_string(options.port)
                                                 : std::string("${port}");
    std::string base = options.asset_base.empty() ? std::string("/static/wim")
                                                  : options.asset_base;
    while (base.size() > 1 && base.back() == '/') {
        base.pop_back();
    }

    const std::string root = "http://" + server + ":" + port + base + "/";
    std::string script;
    script += "#!ipxe\n";
    script += "kernel " + root + "wimboot\n";
    script += "initrd " + root + "bootmgr          bootmgr\n";
    script += "initrd " + root + "bcd              bcd\n";
    script += "initrd " + root + "boot.sdi          boot.sdi\n";
    script += "initrd " + root + "boot.wim          boot.wim\n";
    script += "boot\n";
    return script;
}

/**
 * @brief Builds the bootstrap script served when no MAC was supplied.
 *
 * DHCP hands out a bare "http://server:port/boot.ipxe" URL (it cannot expand
 * iPXE variables), so the first hop re-chains itself with the MAC attached.
 * That keeps a single URL configurable in DHCP while still identifying the
 * client on the real request.
 */
[[nodiscard]] inline std::string self_chain_script(const std::string& boot_url) {
    std::string script;
    script += "#!ipxe\n";
    script += "chain " + boot_url + "?mac=${net0/mac}\n";
    return script;
}

} // namespace cobdiskless::ipxe
