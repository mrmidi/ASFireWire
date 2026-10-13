// SPDX-License-Identifier: Apache-2.0
// Native Sequoia transport for the upstream read-only DICE report.
// Behavioral references (no source copied): Apple IOFireWireLibDevice.cpp:580-665
// (Open required, device-relative Read, generation checking) and Apple's SDK
// IOFireWireLib.h Read/ReadQuadlet/GetGenerationAndNodeID contracts.
#include "NativeFireWire.h"
#include "NativeReadBudget.hpp"
#include "PlaybackLockCheck.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <chrono>
#ifdef PRO40_CLOCK_CONTROL
#include "ClockPolicy.hpp"
#endif

static IOFireWireLibDeviceRef device = nullptr;
static io_service_t service = IO_OBJECT_NULL;
static bool opened = false;
#if defined(PRO40_DMA_SOURCE_QUEUE) && defined(PRO40_DMA_CYCLE_BUDGET)
// Match the staged scheduler's bounded capacity; sustained mode retains its
// unchanged512-read/second ceiling and independent cleanup reserve.
static constexpr unsigned budgetReadLimit=3*Pro40Packets::PlaybackLockCheck::observationCapacity(PRO40_DMA_CYCLE_BUDGET)+256;
#ifdef PRO40_STARTUP_RECOVERY
// One bounded startup prefix plus fresh channel setup; never replenished on a
// restart. Final restoration still has the independent single-use reserve.
static constexpr unsigned readLimit=budgetReadLimit+512;
#else
static constexpr unsigned readLimit=budgetReadLimit>2048?budgetReadLimit:2048;
#endif
#else
static constexpr unsigned readLimit=2048;
#endif
static NativeReadBudget newReadBudget() {
#ifdef PRO40_DMA_SUSTAINED
    (void)readLimit;
    return NativeReadBudget::sustained();
#else
    return NativeReadBudget(readLimit);
#endif
}
static NativeReadBudget readBudget=newReadBudget();
static std::chrono::steady_clock::time_point started;

static uint64_t property(io_service_t s, CFStringRef key) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);
    int64_t result = 0;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID())
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &result);
    if (value) CFRelease(value);
    return static_cast<uint64_t>(result);
}

void pro40_close(void) {
    if (device) {
        if (opened) (*device)->Close(device);
        (*device)->Release(device);
    }
    if (service) IOObjectRelease(service);
    device = nullptr;
    service = IO_OBJECT_NULL;
    opened = false;
}

int32_t pro40_open(uint64_t guid) {
    pro40_close();
    io_iterator_t iterator = IO_OBJECT_NULL;
    auto status = IOServiceGetMatchingServices(kIOMainPortDefault,
                                               IOServiceMatching("IOFireWireDevice"), &iterator);
    if (status != kIOReturnSuccess) return status;
    while (auto candidate = IOIteratorNext(iterator)) {
        // Only the explicitly requested original Pro 40; no other bus device.
        if (property(candidate, CFSTR("GUID")) == guid &&
            property(candidate, CFSTR("Vendor_ID")) == 0x00130e &&
            property(candidate, CFSTR("Model_ID")) == 5) {
            service = candidate;
            break;
        }
        IOObjectRelease(candidate);
    }
    IOObjectRelease(iterator);
    if (!service) return kIOReturnNotFound;
    IOCFPlugInInterface **plugin = nullptr;
    SInt32 score = 0;
    status = IOCreatePlugInInterfaceForService(service, kIOFireWireLibTypeID,
                                               kIOCFPlugInInterfaceID, &plugin, &score);
    if (status == kIOReturnSuccess && plugin) {
#ifdef PRO40_RESOURCE_PROBE
        const auto interfaceID = kIOFireWireDeviceInterfaceID_v9;
#else
        const auto interfaceID = kIOFireWireDeviceInterfaceID;
#endif
        const auto query = (*plugin)->QueryInterface(plugin,
            CFUUIDGetUUIDBytes(interfaceID), reinterpret_cast<void **>(&device));
        IODestroyPlugInInterface(plugin);
        if (query != S_OK || !device) status = kIOReturnUnsupported;
    } else if (status == kIOReturnSuccess) status = kIOReturnUnsupported;
    if (status == kIOReturnSuccess) {
        status = (*device)->Open(device);
        opened = status == kIOReturnSuccess;
    }
    if (!opened) pro40_close();
    readBudget=newReadBudget();
    started = std::chrono::steady_clock::now();
    return status;
}

#ifdef PRO40_RESOURCE_PROBE
void *pro40_native_interface(void) { return opened ? device : nullptr; }
#endif

#if defined(PRO40_CLOCK_CONTROL) || defined(PRO40_RESOURCE_PROBE)
static uint32_t be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

static int32_t global_address(uint32_t generation, uint64_t *address) {
    uint8_t table[8]{};
    auto status = pro40_read(generation, 0xffffe0000000ULL, 8, table);
    if (status) return status;
    const uint64_t offset = uint64_t(be32(table)) * 4;
    const uint64_t size = uint64_t(be32(table + 4)) * 4;
    if (offset < 40 || size < 104 || offset > 65536 || size > 65536 || offset + size > 65536)
        return kIOReturnBadArgument;
    *address = 0xffffe0000000ULL + offset;
    return 0;
}

static int32_t read_clock(uint32_t generation, uint64_t address, Pro40ClockState *state) {
    // Read just owner and clock/status, avoiding read-to-clear NOTIFICATION and
    // EXTENDED_STATUS. These register offsets are section-relative, not absolute.
    uint8_t owner[8]{}, clock[12]{}, rate[12]{};
    auto status = pro40_read(generation, address, 8, owner);
    if (!status) status = pro40_read(generation, address + 0x4c, 12, clock);
    if (!status) status = pro40_read(generation, address + 0x5c, 12, rate);
    if (status) return status;
    *state = {be32(clock), be32(clock + 8), be32(rate), be32(clock + 4),
              be32(rate + 8), be32(owner), be32(owner + 4)};
    return 0;
}

int32_t pro40_clock_state(uint32_t generation, Pro40ClockState *state) {
    if (!state) return kIOReturnBadArgument;
    uint64_t address = 0;
    auto status = global_address(generation, &address);
    return status ? status : read_clock(generation, address, state);
}
#endif

#ifdef PRO40_CLOCK_CONTROL
int32_t pro40_set_idle_rate(uint32_t generation, uint32_t hz) {
    uint64_t address = 0;
    auto status = global_address(generation, &address);
    if (status) return status;
    Pro40ClockState state{};
    status = read_clock(generation, address, &state);
    if (status) return status;
    if (!Pro40Clock::permitted(hz, state.selected, state.enabled, state.capabilities,
        uint64_t(state.owner_high) << 32 | state.owner_low)) return kIOReturnNotPermitted;
    const auto selected = Pro40Clock::select(hz, state.selected);
    const uint8_t bytes[] = {uint8_t(selected >> 24), uint8_t(selected >> 16),
                             uint8_t(selected >> 8), uint8_t(selected)};
    // Apple IOFireWireLibDevice.cpp:665-712: Write transports raw wire bytes.
    // No arbitrary register address is accepted by this exported function.
    uint32_t current = 0;
    uint16_t node = 0;
    status = pro40_state(&current, &node);
    if (status) return status;
    if (current != generation) return kIOFireWireBusReset;
    FWAddress target{};
    target.nodeID = node;
    target.addressHi = uint16_t((address + 0x4c) >> 32);
    target.addressLo = uint32_t(address + 0x4c);
    UInt32 size = 4;
    status = (*device)->Write(device, service, &target, bytes, &size, true, generation);
    return !status && size != 4 ? kIOReturnUnderrun : status;
}
#endif

int32_t pro40_state(uint32_t *generation, uint16_t *node) {
    if (!opened) return kIOReturnNotOpen;
    return (*device)->GetGenerationAndNodeID(device, generation, node);
}

void pro40_begin_cleanup(void) {
    const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-started).count();
    readBudget.beginCleanup(uint64_t(elapsed));
}

int32_t pro40_admit_read(uint64_t address, uint32_t length) {
    if (!opened) return kIOReturnNotOpen;
    // General and EAP address windows only. Bounded per request and per session.
    // TCAT bases: ALSA protocols/dice/src/tcat.rs and tcat/extension.rs.
    constexpr uint64_t base = 0xffffe0000000ULL;
    if (!length || length > 512 || (length % 4) || (address % 4) ||
        address < base || address >= base + 0x400000 ||
        length > base + 0x400000 - address) return kIOReturnBadArgument;
    const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-started).count();
    if (!readBudget.admit(uint64_t(elapsed)))return kIOReturnTimeout;
    return 0;
}

int32_t pro40_read(uint32_t generation, uint64_t address, uint32_t length, void *bytes) {
    if (!bytes) return kIOReturnBadArgument;
    auto status = pro40_admit_read(address, length);
    if (status) return status;
    uint32_t current = 0;
    uint16_t node = 0;
    status = pro40_state(&current, &node);
    if (status != kIOReturnSuccess) return status;
    if (current != generation) return kIOFireWireBusReset;
    FWAddress target{};
    target.nodeID = node;
    target.addressHi = static_cast<UInt16>(address >> 32);
    target.addressLo = static_cast<UInt32>(address);
    UInt32 actual = length;
    // Read preserves wire bytes. No endian conversion at the transport boundary.
    status = (*device)->Read(device, service, &target, bytes, &actual, true, generation);
    if (status == kIOReturnSuccess && actual != length) return kIOReturnUnderrun;
    return status;
}
