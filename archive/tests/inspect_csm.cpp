#include <iostream>
#include "CobDisklessCore/image/pnp_driver_armer.hpp"

using namespace cobdiskless;

int main() {
    auto res = PnpDriverArmer::inspect_vhd_image("G:/csmw10.VHD");
    if (!res.is_ok()) {
        std::cerr << "Inspection failed: " << res.error().message() << std::endl;
        return 1;
    }
    const auto& rep = res.value();
    std::cout << rep.summary() << std::endl;
    return 0;
}
