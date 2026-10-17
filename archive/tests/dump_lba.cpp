#include <iostream>
#include <vector>
#include <span>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include "CobDisklessCore/storage/vhd_image_backend.hpp"

using namespace cobdiskless;

int main() {
    auto res = VhdImageBackend::open("G:/csmw10.VHD", true);
    if (!res.is_ok()) {
        std::cerr << "Open failed: " << res.error().message() << std::endl;
        return 1;
    }
    auto vhd = res.value();
    std::cout << "Opened VHD successfully! Size: " << vhd->size() << std::endl;
    std::vector<uint8_t> buf(14 * 512);
    uint64_t offset = 1223408ull * 512ull;
    auto rres = vhd->read(offset, std::span<std::byte>(reinterpret_cast<std::byte*>(buf.data()), buf.size()));
    if (!rres.is_ok()) {
        std::cerr << "Read failed: " << rres.error().message() << std::endl;
        return 1;
    }
    std::cout << "Read " << rres.value() << " bytes at offset " << offset << " (LBA 1223408):\n";
    for (size_t i = 0; i < std::min<size_t>(buf.size(), 256); ++i) {
        printf("%02X ", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    std::cout << "\nASCII strings in buffer:\n";
    std::string s;
    for (size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] >= 32 && buf[i] <= 126) {
            s += static_cast<char>(buf[i]);
        } else {
            if (s.size() >= 4) {
                std::cout << "  " << s << "\n";
            }
            s.clear();
        }
    }
    if (s.size() >= 4) std::cout << "  " << s << "\n";
    return 0;
}
