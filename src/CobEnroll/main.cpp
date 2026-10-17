// SPDX-License-Identifier: GPL-3.0-or-later

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <iphlpapi.h>
#include <winhttp.h>

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

struct NetDeviceInfo {
    std::string hardware_id;
    std::string subsystem_id;
    std::string revision;
    std::string device_instance;
    std::string description;
    std::string mac_address;
};

// Formats binary bytes to MAC string "AA:BB:CC:DD:EE:FF"
std::string format_mac(const uint8_t* bytes, size_t len) {
    if (len < 6) return "";
    std::ostringstream oss;
    for (size_t i = 0; i < 6; ++i) {
        if (i > 0) oss << ":";
        oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

// Retrieves the primary physical network adapter MAC address and Gateway/DHCP IP
bool get_adapter_network_info(std::string& mac, std::string& gateway_ip, std::string& dhcp_server_ip) {
    ULONG out_buf_len = 15000;
    std::vector<BYTE> buffer(out_buf_len);
    PIP_ADAPTER_ADDRESSES addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG ret = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, addresses, &out_buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(out_buf_len);
        addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ret = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, addresses, &out_buf_len);
    }

    if (ret != NO_ERROR) return false;

    for (PIP_ADAPTER_ADDRESSES curr = addresses; curr != NULL; curr = curr->Next) {
        if (curr->IfType == IF_TYPE_ETHERNET_CSMACD && curr->PhysicalAddressLength == 6) {
            // Found Ethernet adapter
            mac = format_mac(curr->PhysicalAddress, curr->PhysicalAddressLength);

            // Extract Gateway IP
            if (curr->FirstGatewayAddress && curr->FirstGatewayAddress->Address.lpSockaddr) {
                sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(curr->FirstGatewayAddress->Address.lpSockaddr);
                char ip_buf[INET_ADDRSTRLEN] = {0};
                if (inet_ntop(AF_INET, &(sa->sin_addr), ip_buf, INET_ADDRSTRLEN)) {
                    gateway_ip = ip_buf;
                }
            }

            // Extract DHCP Server IP
            if (curr->Dhcpv4Server.lpSockaddr) {
                sockaddr_in* sa = reinterpret_cast<sockaddr_in*>(curr->Dhcpv4Server.lpSockaddr);
                char ip_buf[INET_ADDRSTRLEN] = {0};
                if (inet_ntop(AF_INET, &(sa->sin_addr), ip_buf, INET_ADDRSTRLEN)) {
                    dhcp_server_ip = ip_buf;
                }
            }

            if (!mac.empty()) return true;
        }
    }
    return false;
}

// Queries SMBIOS UUID
std::string get_smbios_uuid() {
    DWORD size = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    if (size == 0) return "";
    std::vector<BYTE> buffer(size);
    if (GetSystemFirmwareTable('RSMB', 0, buffer.data(), size) == 0) return "";

    // Parse SMBIOS tables for Type 1 (System Information)
    const BYTE* ptr = buffer.data() + 8; // Skip RawSMBIOSData header
    const BYTE* end = buffer.data() + size;

    while (ptr + 4 < end) {
        BYTE type = ptr[0];
        BYTE len = ptr[1];
        if (len < 4) break;

        if (type == 1 && len >= 0x19) {
            const BYTE* uuid_bytes = ptr + 8;
            std::ostringstream oss;
            oss << std::hex << std::uppercase << std::setfill('0');
            for (int i = 0; i < 16; ++i) {
                if (i == 4 || i == 6 || i == 8 || i == 10) oss << "-";
                oss << std::setw(2) << static_cast<int>(uuid_bytes[i]);
            }
            return oss.str();
        }

        // Advance to next structure (scan past double null terminator)
        ptr += len;
        while (ptr + 1 < end && !(ptr[0] == 0 && ptr[1] == 0)) {
            ptr++;
        }
        ptr += 2;
    }
    return "";
}

// Fingerprints physical network devices via SetupAPI
std::vector<NetDeviceInfo> enumerate_pnp_net_devices() {
    std::vector<NetDeviceInfo> devices;
    HDEVINFO hdev_info = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, NULL, NULL, DIGCF_PRESENT);
    if (hdev_info == INVALID_HANDLE_VALUE) {
        return devices;
    }

    SP_DEVINFO_DATA dev_info_data;
    dev_info_data.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hdev_info, i, &dev_info_data); ++i) {
        WCHAR inst_buf[MAX_DEVICE_ID_LEN] = {0};
        if (CM_Get_Device_IDW(dev_info_data.DevInst, inst_buf, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
            continue;
        }

        std::wstring ws_inst(inst_buf);
        // Only target physical PCI network devices
        if (ws_inst.find(L"PCI\\") == std::wstring::npos) {
            continue;
        }

        NetDeviceInfo info;
        char mb_inst[512] = {0};
        WideCharToMultiByte(CP_UTF8, 0, inst_buf, -1, mb_inst, sizeof(mb_inst), NULL, NULL);
        info.device_instance = mb_inst;

        // Hardware IDs
        WCHAR hw_id_buf[1024] = {0};
        if (SetupDiGetDeviceRegistryPropertyW(hdev_info, &dev_info_data, SPDRP_HARDWAREID,
                                              NULL, reinterpret_cast<PBYTE>(hw_id_buf),
                                              sizeof(hw_id_buf), NULL)) {
            char mb_hw[1024] = {0};
            WideCharToMultiByte(CP_UTF8, 0, hw_id_buf, -1, mb_hw, sizeof(mb_hw), NULL, NULL);
            info.hardware_id = mb_hw;

            // Extract Subsystem ID and Revision
            std::string s_hw = info.hardware_id;
            size_t subsys_pos = s_hw.find("SUBSYS_");
            if (subsys_pos != std::string::npos) {
                size_t end_pos = s_hw.find('&', subsys_pos);
                info.subsystem_id = s_hw.substr(subsys_pos, (end_pos != std::string::npos) ? (end_pos - subsys_pos) : std::string::npos);
            }
            size_t rev_pos = s_hw.find("REV_");
            if (rev_pos != std::string::npos) {
                size_t end_pos = s_hw.find('&', rev_pos);
                info.revision = s_hw.substr(rev_pos, (end_pos != std::string::npos) ? (end_pos - rev_pos) : std::string::npos);
            }
        }

        // Friendly name / description
        WCHAR desc_buf[512] = {0};
        if (SetupDiGetDeviceRegistryPropertyW(hdev_info, &dev_info_data, SPDRP_DEVICEDESC,
                                              NULL, reinterpret_cast<PBYTE>(desc_buf),
                                              sizeof(desc_buf), NULL)) {
            char mb_desc[512] = {0};
            WideCharToMultiByte(CP_UTF8, 0, desc_buf, -1, mb_desc, sizeof(mb_desc), NULL, NULL);
            info.description = mb_desc;
        }

        devices.push_back(info);
    }

    SetupDiDestroyDeviceInfoList(hdev_info);
    return devices;
}

// Scans local directories for matching .inf and silently binds via SetupAPI
void bind_network_driver(const std::string& search_dir = "X:\\drivers") {
    std::error_code ec;
    if (!std::filesystem::exists(search_dir, ec)) return;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(search_dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".inf") {
            std::wstring inf_w = entry.path().wstring();
            WCHAR dest_inf[MAX_PATH] = {0};
            // Copy INF into driver store
            if (SetupCopyOEMInfW(inf_w.c_str(), NULL, SPOST_PATH, 0, dest_inf, MAX_PATH, NULL, NULL)) {
                std::wcout << L"[cob_enroll] Driver pre-staged into PE store: " << inf_w << std::endl;
            }
        }
    }
}

// Sends HTTP POST JSON payload to CobDiskless enrollment API
bool send_enrollment_request(const std::string& server_ip, uint16_t port, const std::string& json_payload) {
    std::wstring ws_server(server_ip.begin(), server_ip.end());

    HINTERNET h_session = WinHttpOpen(L"CobEnroll/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h_session) return false;

    HINTERNET h_connect = WinHttpConnect(h_session, ws_server.c_str(), port, 0);
    if (!h_connect) {
        WinHttpCloseHandle(h_session);
        return false;
    }

    HINTERNET h_request = WinHttpOpenRequest(h_connect, L"POST", L"/api/v1/client/enroll",
                                            NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!h_request) {
        WinHttpCloseHandle(h_connect);
        WinHttpCloseHandle(h_session);
        return false;
    }

    std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\n";
    BOOL sent = WinHttpSendRequest(h_request, headers.c_str(), static_cast<DWORD>(headers.length()),
                                   const_cast<char*>(json_payload.data()),
                                   static_cast<DWORD>(json_payload.length()),
                                   static_cast<DWORD>(json_payload.length()), 0);

    bool success = false;
    if (sent && WinHttpReceiveResponse(h_request, NULL)) {
        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        if (WinHttpQueryHeaders(h_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX)) {
            success = (status_code >= 200 && status_code < 300);
        }
    }

    WinHttpCloseHandle(h_request);
    WinHttpCloseHandle(h_connect);
    WinHttpCloseHandle(h_session);
    return success;
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "================================================================" << std::endl;
    std::cout << "  CobDiskless Zero-Touch Bootstrap Agent (cob_enroll)           " << std::endl;
    std::cout << "================================================================" << std::endl;

    std::string server_ip;
    uint16_t server_port = 18080;

    // Parse command line overrides: -s <server_ip> -p <port>
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-s" || arg == "--server") && i + 1 < argc) {
            server_ip = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            server_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
    }

    // 1. Hardware Fingerprinting: PnP and MAC
    std::cout << "[cob_enroll] Probing physical network topology..." << std::endl;
    std::string mac_address, gateway_ip, dhcp_ip;
    if (!get_adapter_network_info(mac_address, gateway_ip, dhcp_ip)) {
        std::cerr << "[cob_enroll] WARNING: Could not resolve MAC from GetAdaptersAddresses" << std::endl;
    }

    if (server_ip.empty()) {
        if (!dhcp_ip.empty()) server_ip = dhcp_ip;
        else if (!gateway_ip.empty()) server_ip = gateway_ip;
        else server_ip = "192.168.1.100";
    }

    std::string uuid = get_smbios_uuid();
    auto pnp_devices = enumerate_pnp_net_devices();

    std::string primary_hw_id;
    std::string primary_subsys_id;
    std::string primary_rev;
    std::string primary_inst;

    if (!pnp_devices.empty()) {
        const auto& dev = pnp_devices[0];
        primary_hw_id = dev.hardware_id;
        primary_subsys_id = dev.subsystem_id;
        primary_rev = dev.revision;
        primary_inst = dev.device_instance;
        std::cout << "[cob_enroll] Primary NIC: " << dev.description << std::endl;
        std::cout << "             HW ID:   " << primary_hw_id << std::endl;
        std::cout << "             Device:  " << primary_inst << std::endl;
    }

    std::cout << "[cob_enroll] MAC Address: " << mac_address << std::endl;
    std::cout << "[cob_enroll] SMBIOS UUID: " << uuid << std::endl;
    std::cout << "[cob_enroll] Server Target: " << server_ip << ":" << server_port << std::endl;

    // 2. Silent Driver Binding
    std::cout << "[cob_enroll] Scanning local WinPE driver store..." << std::endl;
    bind_network_driver("X:\\drivers");
    bind_network_driver("X:\\driver_pool");

    // 3. Hardware Profile Generation
    std::ostringstream json;
    json << "{\n"
         << "  \"mac\": \"" << mac_address << "\",\n"
         << "  \"uuid\": \"" << uuid << "\",\n"
         << "  \"hardware_id\": \"" << primary_hw_id << "\",\n"
         << "  \"subsystem_id\": \"" << primary_subsys_id << "\",\n"
         << "  \"revision\": \"" << primary_rev << "\",\n"
         << "  \"device_instance\": \"" << primary_inst << "\"\n"
         << "}\n";

    std::cout << "[cob_enroll] Submitting hardware profile to CobDiskless API..." << std::endl;
    bool enrolled = send_enrollment_request(server_ip, server_port, json.str());
    if (enrolled) {
        std::cout << "[cob_enroll] SUCCESS: Client hardware profile registered and validated!" << std::endl;
    } else {
        std::cerr << "[cob_enroll] API call returned non-200. Proceeding with reboot retry cycle." << std::endl;
    }

    // 4. Reboot
    std::cout << "[cob_enroll] Zero-touch bootstrap complete. Initiating silent reboot..." << std::endl;
    system("wpeutil reboot");
    return 0;
}
