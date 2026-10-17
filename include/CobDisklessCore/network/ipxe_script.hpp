// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "CobDisklessCore/image/image_format.hpp"

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
    script += "dhcp net0\n";
    script += "set net0/gateway 0.0.0.0\n";
    script += "set keep-san 1\n";
    script += "set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}\n";
    std::string srv = options.server_ip.empty() ? std::string("${next-server}") : options.server_ip;
    if (srv == "${server_ip}") {
        srv = "${next-server}";
    }
    const std::string iqn = options.target_iqn.empty() ? std::string("iqn.2026-09.com.cobdiskless:storage") : options.target_iqn;
    script += "sanhook --drive 0x80 iscsi:" + srv + "::::" + iqn + "\n";
    script += "sanboot --drive 0x80\n";
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
 * firmware-visible disk is stable) and `sanboot --drive 0x80` then boots it.
 *
 * Emits strictly:
 * #!ipxe
 * dhcp net0
 * set net0/gateway 0.0.0.0
 * set keep-san 1
 * set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}
 * sanhook --drive 0x80 iscsi:${server_ip}::::${target_iqn}
 * sanboot --drive 0x80
 */
[[nodiscard]] inline std::string legacy_mbr_sanboot_script(const LegacySanbootOptions& options = {}) {
    const std::string server = options.server_ip.empty() ? std::string("${server_ip}")
                                                         : options.server_ip;
    const std::string iqn = options.target_iqn.empty() ? std::string("${target_iqn}")
                                                       : options.target_iqn;
    std::string script;
    script += "#!ipxe\n";
    script += "dhcp net0\n";
    script += "set net0/gateway 0.0.0.0\n";
    script += "set keep-san 1\n";
    script += "set initiator-iqn iqn.1991-05.com.microsoft:${net0/mac:hexhyp}\n";
    script += "sanhook --drive 0x80 iscsi:" + server + "::::" + iqn + "\n";
    script += "sanboot --drive 0x80\n";
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
 * @brief Dynamic Dispatcher: Case A - Native iSCSI boot script for validated clients.
 */
[[nodiscard]] inline std::string native_iscsi_boot_script(
    const std::string& server_ip,
    const std::string& client_mac = "${net0/mac}",
    const std::string& target_iqn = "") {
    const std::string srv = server_ip.empty() ? "${next-server}" : server_ip;
    std::string iqn = target_iqn;
    if (iqn.empty()) {
        std::string safe_mac = client_mac;
        if (safe_mac.empty() || safe_mac == "${net0/mac}") {
            iqn = "iqn.2026-10.local.cobdiskless:client-${net0/mac}";
        } else {
            iqn = "iqn.2026-10.local.cobdiskless:client-" + safe_mac;
        }
    }
    std::string script;
    script += "#!ipxe\n";
    script += "set net0/gateway 0.0.0.0\n";
    script += "set keep-san 1\n";
    script += "echo [CobDiskless] Attaching SAN volume for ${net0/mac}...\n";
    script += "sanhook --drive 0x80 --keep iscsi:" + srv + "::::" + iqn + " || goto san_fail\n";
    script += "sanboot --drive 0x80 || goto san_fail\n\n";
    script += ":san_fail\n";
    script += "echo [ERROR] Failed to attach or boot from SAN target!\n";
    script += "echo Dropping to iPXE recovery shell in 5s...\n";
    script += "sleep 5\n";
    script += "shell\n";
    return script;

}

/**
 * @brief Dynamic Dispatcher: Case B - Divert unknown / unvalidated clients into WinPE.
 */
[[nodiscard]] inline std::string winpe_bootstrap_script(
    const std::string& server_ip,
    uint16_t port = 0) {
    const std::string srv = server_ip.empty() ? "${next-server}" : server_ip;
    const std::string host = (port != 0 && port != 80) ? (srv + ":" + std::to_string(port)) : srv;
    const std::string root = "http://" + host + "/boot/";
    std::string script;
    script += "#!ipxe\n";
    script += "dhcp\n";
    script += "kernel " + root + "wimboot\n";
    script += "initrd " + root + "BCD         BCD\n";
    script += "initrd " + root + "boot.sdi    boot.sdi\n";
    script += "initrd " + root + "cob_pe.wim  boot.wim\n";
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
    // Passes MAC, platform, and UUID for zero-touch hardware fingerprinting
    script += "chain " + boot_url + "?mac=${net0/mac}&platform=${platform}&uuid=${smbios/uuid}\n";
    return script;
}

// ---------------------------------------------------------------------------
// Firmware / image mismatch guard (Subsystem B)
// ---------------------------------------------------------------------------

/**
 * @brief Client firmware class, as advertised by iPXE's `${platform}` setting.
 *
 * `${platform}` is the firmware that is actually executing iPXE. DHCP Option 93
 * carried the same information at PXE time, but by the time /boot.ipxe is
 * fetched that exchange is long over, so the running firmware is the only
 * authoritative answer available server-side.
 */
enum class ClientPlatform {
    /// No usable value: the guard stays open. Never block a boot we cannot judge.
    Unknown = 0,
    /// `pcat` / `pcbios` / `bios` / `legacy`: INT13 + CSM firmware.
    LegacyBios,
    /// `efi` / `uefi`: native UEFI firmware (EFI Block I/O).
    NativeUefi,
};

[[nodiscard]] inline std::string_view to_string(ClientPlatform platform) noexcept {
    switch (platform) {
        case ClientPlatform::LegacyBios: return "Legacy BIOS";
        case ClientPlatform::NativeUefi: return "UEFI";
        case ClientPlatform::Unknown:    return "Unknown firmware";
    }
    return "Unknown firmware";
}

/**
 * @brief Parses the `platform` query parameter.
 *
 * A stock iPXE build emits `pcbios` (legacy) or `efi` (UEFI); `pcat` and the
 * other aliases cover hand-written chain URLs and older builds. An unrecognised
 * token maps to ClientPlatform::Unknown rather than failing, so an odd value
 * can only ever leave the guard open -- it can never strand a machine.
 *
 * @return std::nullopt when the parameter was absent; otherwise the parsed
 *         class, which may still be ClientPlatform::Unknown.
 */
[[nodiscard]] inline std::optional<ClientPlatform> parse_client_platform(std::string_view raw) {
    std::string value;
    value.reserve(raw.size());
    for (const char c : raw) {
        value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::nullopt;
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);

    if (value == "pcat" || value == "pcbios" || value == "pc-bios" ||
        value == "bios" || value == "legacy" || value == "x86") {
        return ClientPlatform::LegacyBios;
    }
    if (value == "efi" || value == "uefi" || value == "efi32" || value == "efi64" ||
        value == "efi-x86" || value == "efi-x86_64") {
        return ClientPlatform::NativeUefi;
    }
    return ClientPlatform::Unknown;
}

/**
 * @brief True when the firmware cannot start an image built for the other class.
 *
 * BootMode::Dual (hybrid MBR + ESP) boots under either firmware and
 * BootMode::Unknown gives nothing to judge, so neither is ever a mismatch.
 */
[[nodiscard]] inline bool is_platform_mismatch(ClientPlatform platform,
                                               BootMode image_mode,
                                               PartitionScheme scheme = PartitionScheme::Unknown) noexcept {
    if (image_mode == BootMode::Dual || image_mode == BootMode::Unknown) {
        return false;
    }
    switch (platform) {
        case ClientPlatform::LegacyBios:
            return (scheme == PartitionScheme::Gpt || image_mode == BootMode::Uefi);
        case ClientPlatform::NativeUefi:
            return (image_mode == BootMode::Bios || (scheme == PartitionScheme::Mbr && image_mode != BootMode::Uefi));
        case ClientPlatform::Unknown:
            return false;
    }
    return false;
}

/**
 * @brief Rewrites a value so iPXE's script parser passes it through verbatim.
 *
 * `${` would be settings-expanded, a leading `#` would comment out the rest of
 * the line, `||` / `&&` / `;` would be read as command separators, and CR/LF
 * would split the line in two. Station and image names are sanitised on the way
 * in, but a boot script must not be one stray `$` away from running something
 * else entirely.
 */
[[nodiscard]] inline std::string sanitize_script_value(std::string_view raw, size_t max_len = 96) {
    std::string out;
    out.reserve(raw.size() < max_len ? raw.size() : max_len);
    for (const char c : raw) {
        if (out.size() >= max_len) {
            break;
        }
        switch (c) {
            case '$':
            case '#':
            case ';':
            case '|':
            case '&':
            case '\\':
                out.push_back('-');
                break;
            case '\r':
            case '\n':
            case '\t':
                out.push_back(' ');
                break;
            default:
                out.push_back((static_cast<unsigned char>(c) < 0x20 || c == '\x7f') ? ' ' : c);
                break;
        }
    }
    return out;
}

/// Raw ESC bytes. The script parser memcpy's each line, `split_command` only
/// splits on isspace() (ESC is not whitespace) and `expand_settings` only
/// reacts to `$`, so these reach the console untouched. Both the BIOS and the
/// EFI console register an ANSI SGR handler, so they render as colour instead
/// of being printed literally.
inline constexpr const char* kAnsiReset = "\033[0m";
inline constexpr const char* kAnsiBoldRed = "\033[1;31m";
inline constexpr const char* kAnsiBoldYellow = "\033[1;33m";
inline constexpr const char* kAnsiWhiteOnRed = "\033[1;37;41m";

/**
 * @brief Everything the red-screen script has to show the operator.
 */
struct PlatformMismatchOptions {
    std::string station_name;
    std::string station_mac;
    std::string image_name;
    /// Firmware class the client actually reported.
    ClientPlatform client_platform{ClientPlatform::Unknown};
    /// Raw `platform` token, echoed so the operator can see what was sent.
    std::string platform_token;
    /// Boot mode the assigned image's partition layout demands.
    BootMode image_mode{BootMode::Unknown};
    /// Human label for the image format, e.g. "UEFI (GPT)". Falls back to
    /// to_string(image_mode) when empty.
    std::string image_format;
    /// Wait before rebooting. parse_timeout() reads this as milliseconds.
    unsigned timeout_ms{15000};
};

/**
 * @brief Builds the high-contrast red screen served on a firmware mismatch.
 *
 * The whole point is that this script never touches the SAN: emitting
 * `sanhook` / `sanboot` for a GPT image on CSM firmware is exactly the
 * black-screen hang this guard exists to prevent, so the script contains
 * neither word anywhere -- not even in the prose.
 *
 * The wait is `prompt --timeout <ms> || echo ...`. A bare `prompt --timeout`
 * returns -ETIMEDOUT on expiry, iPXE aborts a script on the first non-zero
 * status, and `reboot` would then never run. The `||` absorbs the status on
 * both paths, so `reboot` is reached whether a key is pressed or the timer
 * runs out.
 *
 * Echo lines deliberately use single spaces only: iPXE's `concat_args()`
 * re-joins the split tokens with one space, so runs of two or more would
 * collapse and destroy the layout.
 */
[[nodiscard]] inline std::string platform_mismatch_script(const PlatformMismatchOptions& options) {
    const std::string station = sanitize_script_value(options.station_name, 48);
    const std::string mac = sanitize_script_value(options.station_mac, 32);
    const std::string image = sanitize_script_value(options.image_name, 64);
    const std::string token = sanitize_script_value(options.platform_token, 32);
    const std::string format = options.image_format.empty()
        ? std::string(to_string(options.image_mode))
        : sanitize_script_value(options.image_format, 48);

    std::string client = std::string(to_string(options.client_platform));
    if (!token.empty()) {
        client += " (" + token + ")";
    }

    const unsigned seconds = (options.timeout_ms + 999U) / 1000U;
    const std::string rule(73, '=');
    const bool is_csm_gpt = (options.client_platform == ClientPlatform::LegacyBios);

    std::string script;
    script += "#!ipxe\n";
    script += "clear\n";
    script += "echo " + std::string(kAnsiWhiteOnRed) + rule + kAnsiReset + "\n";
    script += "echo " + std::string(kAnsiWhiteOnRed) +
              "=== COBDISKLESS BOOT BLOCKED: FIRMWARE / IMAGE MISMATCH ===" + kAnsiReset + "\n";
    script += "echo " + std::string(kAnsiWhiteOnRed) + rule + kAnsiReset + "\n";
    script += "echo\n";

    if (is_csm_gpt) {
        script += "echo " + std::string(kAnsiBoldYellow) +
                  "FATAL: Client booted in CSM/BIOS mode, but assigned image requires UEFI (GPT). Change motherboard BIOS setting to UEFI." +
                  kAnsiReset + "\n";
    } else {
        script += "echo " + std::string(kAnsiBoldYellow) +
                  "FATAL: Client booted in UEFI mode, but assigned image requires Legacy BIOS / CSM (MBR). Change motherboard BIOS setting to CSM/Legacy." +
                  kAnsiReset + "\n";
    }
    script += "echo\n";

    script += "echo " + std::string(kAnsiBoldRed) + "Station name:      " + station + kAnsiReset + "\n";
    script += "echo " + std::string(kAnsiBoldRed) + "MAC address:       " + mac + kAnsiReset + "\n";
    script += "echo " + std::string(kAnsiBoldRed) + "Client firmware:   " + client + kAnsiReset + "\n";
    script += "echo " + std::string(kAnsiBoldRed) + "Assigned image:    " + image + kAnsiReset + "\n";
    script += "echo " + std::string(kAnsiBoldRed) + "Image boot format: " + format + kAnsiReset + "\n";
    script += "echo\n";

    script += "echo " + std::string(kAnsiBoldYellow) +
              "This PC is running in " + client + ", but the image assigned to it\n";
    script += "echo " + std::string(kAnsiBoldYellow) +
              "requires " + format + ". Those two cannot boot together: the\n";
    script += "echo " + std::string(kAnsiBoldYellow) +
              "firmware would never find a boot sector, so no disk was attached.\n";
    script += "echo " + std::string(kAnsiReset) + "\n";

    script += "echo Fix it - do ONE of the following:\n";
    script += "echo * On this PC: open the firmware setup (DEL or F2), switch the boot\n";
    script += "echo   mode to match the image format above, save, and let it reboot.\n";
    script += "echo * On the CobDiskless console: assign this station an image whose boot\n";
    script += "echo   format matches this PC, then let it reboot.\n";
    script += "echo\n";
    script += "echo " + std::string(kAnsiBoldRed) +
              "No disk was attached. Rebooting in " + std::to_string(seconds) + " seconds..." +
              kAnsiReset + "\n";
    script += "prompt --timeout " + std::to_string(options.timeout_ms) +
              " || echo Timed out. Rebooting now.\n";
    script += "reboot\n";
    return script;
}

/**
 * @brief The six iPXE settings lines that harvest PCI telemetry.
 *
 * Shared by every script that must learn the boot NIC before deciding:
 * `ven`/`dev` (config 0x00/0x02), `rev` (0x08), `subven`/`subdev`
 * (0x2C/0x2E). On builds/NICs without `${pci/...}` support everything
 * expands empty and the server treats the params as absent (legacy path).
 */
[[nodiscard]] inline std::string pci_sniff_script_lines() {
    std::string script;
    script += "set busloc ${net0/busloc}\n";
    script += "set ven ${pci/${busloc}.0.2:hex}\n";
    script += "set dev ${pci/${busloc}.2.2:hex}\n";
    script += "set rev ${pci/${busloc}.8.1:hex}\n";
    script += "set subven ${pci/${busloc}.2c.2:hex}\n";
    script += "set subdev ${pci/${busloc}.2e.2:hex}\n";
    return script;
}

/**
 * @brief Builds the one-shot re-chain that makes `${platform}` reach the server.
 *
 * DHCP cannot expand iPXE variables, so the boot URL it hands out is bare and
 * the first /boot.ipxe request carries no `platform`. When the guard has an
 * image format to judge against, the firmware class is the only missing half --
 * ask for it once, then decide on the second hop.
 *
 * The same hop also harvests PCI telemetry (`ven`/`dev` from config 0x00/0x02,
 * `rev` from 0x08, `subven`/`subdev` from 0x2C/0x2E) so the second hop can
 * feed the hardware-enrollment route (/boot with ven/dev). Without this,
 * stock-iPXE bare-metal stations could never enroll: nothing else in their
 * flow reports PCI ids. Empty expansions are harmless -- the server treats
 * absent params as "no telemetry" and keeps the legacy behavior.
 *
 * The chained request always carries the `platform` key (even if `${platform}`
 * expanded to nothing), so the second hop can never ask a third time. Failure
 * of the chain itself lands on :probe_failed rather than a silent iPXE shell.
 */
[[nodiscard]] inline std::string platform_probe_script(const std::string& boot_url) {
    const std::string sep = (boot_url.find('?') == std::string::npos) ? "?" : "&";
    std::string script;
    script += "#!ipxe\n";
    script += pci_sniff_script_lines();
    script += "chain " + boot_url + sep +
               "platform=${platform}&ven=${ven}&dev=${dev}&rev=${rev}"
               "&subven=${subven}&subdev=${subdev}&sniffed=1 || goto probe_failed\n";
    script += "goto probe_done\n";
    script += ":probe_failed\n";
    script += "echo CobDiskless did not answer the firmware check. Rebooting.\n";
    script += "prompt --timeout 15000 || echo Timed out. Rebooting now.\n";
    script += "reboot\n";
    script += ":probe_done\n";
    return script;
}

} // namespace cobdiskless::ipxe
