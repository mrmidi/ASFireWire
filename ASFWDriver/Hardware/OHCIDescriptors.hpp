#pragma once

#include <cstdint>
#include "OHCIConstants.hpp"

namespace ASFW::Async::HW {

// Branch helpers and descriptor structures
[[nodiscard]] constexpr uint32_t MakeBranchWordAT(uint64_t physAddr, uint8_t Zblocks) noexcept;
[[nodiscard]] constexpr uint32_t MakeBranchWordAR(uint64_t physAddr, bool continueFlag) noexcept;
[[nodiscard]] constexpr uint32_t DecodeBranchPhys32_AT(uint32_t branchWord) noexcept;
[[nodiscard]] constexpr uint32_t DecodeBranchPhys32_AR(uint32_t branchWord) noexcept;

struct alignas(16) OHCIDescriptor {
    union {
        uint32_t control{0};
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        struct { uint16_t reqCount; uint16_t controlUpper; };
#else
        struct { uint16_t controlUpper; uint16_t reqCount; };
#endif
    };

    uint32_t dataAddress{0};
    uint32_t branchWord{0};

    union {
        uint32_t statusWord{0};
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        struct { uint16_t timeStamp; uint16_t xferStatus; };
#else
        struct { uint16_t timeStamp; uint16_t xferStatus; };
#endif
        uint32_t softwareTag;
    };

    static constexpr uint32_t kControlHighShift = 16;
    static constexpr uint32_t kCmdShift = 12;
    static constexpr uint32_t kStatusShift = 11;
    static constexpr uint32_t kKeyShift = 8;
    static constexpr uint32_t kPingShift = 7;
    static constexpr uint32_t kYYShift = 6;
    static constexpr uint32_t kIntShift = 4;
    static constexpr uint32_t kBranchShift = 2;
    static constexpr uint32_t kWaitShift = 0;
    static constexpr uint32_t kZShift = 28;

    static constexpr uint8_t kCmdOutputMore = 0x0;
    static constexpr uint8_t kCmdOutputLast = 0x1;
    static constexpr uint8_t kCmdInputMore  = 0x2;
    static constexpr uint8_t kKeyStandard = 0x0;
    static constexpr uint8_t kKeyImmediate = 0x2;
    static constexpr uint8_t kIntNever = 0b00;
    static constexpr uint8_t kIntOnError = 0b01;
    static constexpr uint8_t kIntAlways = 0b11;
    static constexpr uint8_t kBranchNever = 0b00;
    static constexpr uint8_t kBranchAlways = 0b11;

    [[nodiscard]] static constexpr uint32_t BuildControl(uint16_t reqCount, uint8_t cmd, uint8_t key, uint8_t i, uint8_t b, bool ping = false) noexcept {
        const uint8_t cmd_masked = cmd & 0xF;
        const uint8_t key_masked = key & 0x7;
        const uint8_t i_masked = i & 0x3;
        const uint8_t b_masked = b & 0x3;
        const uint32_t high = (static_cast<uint32_t>(cmd_masked) << kCmdShift) | (static_cast<uint32_t>(key_masked) << kKeyShift) | (static_cast<uint32_t>(i_masked) << kIntShift) | (static_cast<uint32_t>(b_masked) << kBranchShift) | (ping ? (1u << kPingShift) : 0);
        return ((high & 0xFFFFu) << kControlHighShift) | (reqCount & 0xFFFFu);
    }

    static inline void PatchBranch(OHCIDescriptor& desc, uint8_t b) noexcept {
        const uint32_t mask = 0x3u << (kBranchShift + kControlHighShift);
        const uint32_t val = (b & 0x3u) << (kBranchShift + kControlHighShift);
        desc.control = (desc.control & ~mask) | val;
    }

    static inline void ClearBranchBits(OHCIDescriptor& desc) noexcept {
        const uint32_t mask = 0x3u << (kBranchShift + kControlHighShift);
        desc.control = desc.control & ~mask;
    }
};
static_assert(sizeof(OHCIDescriptor) == 16, "OHCIDescriptor must be 16 bytes per OHCI §7.1");

struct alignas(16) OHCIDescriptorImmediate {
    OHCIDescriptor common;
    uint32_t immediateData[4]{};
};
static_assert(sizeof(OHCIDescriptorImmediate) == 32, "OHCIDescriptorImmediate must be 32 bytes per OHCI");

[[nodiscard]] inline uint16_t AR_xferStatus(const OHCIDescriptor& d) noexcept { return static_cast<uint16_t>(d.statusWord >> 16); }
[[nodiscard]] inline uint16_t AR_resCount(const OHCIDescriptor& d) noexcept { return static_cast<uint16_t>(d.statusWord & 0xFFFF); }
inline void AR_init_status(OHCIDescriptor& d, uint16_t reqCount_host) noexcept { d.statusWord = (0x0000u << 16) | reqCount_host; }
[[nodiscard]] inline uint16_t AT_xferStatus(const OHCIDescriptor& d) noexcept { return d.xferStatus; }
[[nodiscard]] inline uint16_t AT_timeStamp(const OHCIDescriptor& d) noexcept { return d.timeStamp; }

[[nodiscard]] inline bool IsImmediate(const OHCIDescriptor& d) noexcept {
    const uint32_t controlHi = d.control >> OHCIDescriptor::kControlHighShift;
    const uint8_t keyField = (controlHi >> OHCIDescriptor::kKeyShift) & 0x7;
    return keyField == OHCIDescriptor::kKeyImmediate;
}

[[nodiscard]] inline uint8_t ExtractTLabel(const OHCIDescriptorImmediate* immDesc) noexcept {
    if (!immDesc) return 0xFF;
    const uint32_t controlHost = immDesc->immediateData[0];
    const uint8_t tLabel = static_cast<uint8_t>((controlHost >> 10) & 0x3F);
    return tLabel;
}

// Implementation of branch helpers
[[nodiscard]] constexpr uint32_t MakeBranchWordAT(uint64_t physAddr, uint8_t Zblocks) noexcept {
    if ((physAddr & 0xFULL) != 0 || physAddr > 0xFFFFFFFFu) return 0;
    if (Zblocks != 0 && (Zblocks < 2 || Zblocks > 8)) return 0;
    return (static_cast<uint32_t>(physAddr) & 0xFFFFFFF0u) | (static_cast<uint32_t>(Zblocks) & 0xFu);
}

[[nodiscard]] constexpr uint32_t MakeBranchWordAR(uint64_t physAddr, bool continueFlag) noexcept {
    if ((physAddr & 0xFULL) != 0 || physAddr > 0xFFFFFFFFu) return 0;
    const uint32_t Z = continueFlag ? 1u : 0u;
    return (static_cast<uint32_t>(physAddr) & 0xFFFFFFF0u) | Z;
}

[[nodiscard]] constexpr uint32_t DecodeBranchPhys32_AT(uint32_t branchWord) noexcept { return branchWord & 0xFFFFFFF0u; }
[[nodiscard]] constexpr uint32_t DecodeBranchPhys32_AR(uint32_t branchWord) noexcept { return branchWord & 0xFFFFFFF0u; }

} // namespace ASFW::Async::HW
