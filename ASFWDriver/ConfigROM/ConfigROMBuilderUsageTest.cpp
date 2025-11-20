#include "ConfigROMBuilder.hpp"
#include <DriverKit/IOLib.h>

using namespace ASFW::Driver;

// This is not a unit test framework file; it simply ensures the staged API
// is used somewhere so linkage errors surface during build.
extern "C" void _asfw_config_rom_builder_usage_smoke() {
    ConfigROMBuilder b;
    b.Begin(0x0083'0000u, 0x1122334455667788ULL, 0x0000'0001u);
    b.AddImmediateEntry(ASFW::FW::ConfigKey::kModuleVendorId, 0x001122u);
    b.AddImmediateEntry(ASFW::FW::ConfigKey::kNodeCapabilities, 0x00000001u);
    b.AddTextLeaf(ASFW::FW::ConfigKey::kTextualDescriptor, "ASFW Test Vendor");
    b.Finalize();
    auto img = b.ImageBE();
    if (!img.empty()) {
        // Touch first quad so optimizer cannot drop code entirely.
        (void)img[0];
    }
}
