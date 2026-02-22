// TxVerifierDecode.hpp
// ASFW - Dev-only IT TX verifier helpers (host-test friendly)
//
// These utilities are intentionally free of DriverKit dependencies so they can
// be unit-tested on the host (ASFW_HOST_TEST).

#pragma once

#include <cstdint>

namespace ASFW::Isoch::TxVerify {

[[nodiscard]] constexpr uint32_t ByteSwap32(uint32_t x) noexcept {
    return ((x & 0xFF000000u) >> 24) |
           ((x & 0x00FF0000u) >> 8)  |
           ((x & 0x0000FF00u) << 8)  |
           ((x & 0x000000FFu) << 24);
}

struct CIPFields {
    uint8_t eoh0{0};
    uint8_t sid{0};
    uint8_t dbs{0};
    uint8_t dbc{0};

    uint8_t eoh1{0};
    uint8_t fmt{0};
    uint8_t fdf{0};
    uint16_t syt{0};
};

/// Parse CIP header quadlets stored in host order (as written to DMA memory).
/// The writer stores wire bytes into memory, so on little-endian hosts the
/// in-memory uint32_t is byteswapped relative to what FireBug prints.
[[nodiscard]] constexpr CIPFields ParseCIPFromHostWords(uint32_t q0Host, uint32_t q1Host) noexcept {
    const uint32_t q0 = ByteSwap32(q0Host);
    const uint32_t q1 = ByteSwap32(q1Host);

    CIPFields f{};
    f.eoh0 = static_cast<uint8_t>((q0 >> 30) & 0x3u);
    f.sid  = static_cast<uint8_t>((q0 >> 24) & 0x3Fu);
    f.dbs  = static_cast<uint8_t>((q0 >> 16) & 0xFFu);
    f.dbc  = static_cast<uint8_t>(q0 & 0xFFu);

    f.eoh1 = static_cast<uint8_t>((q1 >> 30) & 0x3u);
    f.fmt  = static_cast<uint8_t>((q1 >> 24) & 0x3Fu);
    f.fdf  = static_cast<uint8_t>((q1 >> 16) & 0xFFu);
    f.syt  = static_cast<uint16_t>(q1 & 0xFFFFu);
    return f;
}

[[nodiscard]] constexpr bool HasValidAM824Label(uint32_t am824HostWord, uint8_t labelByte) noexcept {
    return static_cast<uint8_t>(am824HostWord & 0xFFu) == labelByte;
}

[[nodiscard]] constexpr uint8_t AM824LabelByte(uint32_t am824HostWord) noexcept {
    return static_cast<uint8_t>(am824HostWord & 0xFFu);
}

/// Simple DBC continuity checker for blocking-mode (NO-DATA packets ignored).
/// For IEC 61883-6 blocking cadence, NO-DATA carries the *next* DATA DBC value,
/// but does not advance the continuity state.
class DbcContinuity {
public:
    explicit constexpr DbcContinuity(uint8_t blocksPerDataPacket) noexcept
        : blocksPerDataPacket_(blocksPerDataPacket) {}

    void Reset() noexcept {
        haveLastData_ = false;
        lastDataDbc_ = 0;
    }

    /// Observe a packet. Returns true if continuity is OK (or not applicable yet).
    [[nodiscard]] bool Observe(bool isDataPacket, uint8_t dbc) noexcept {
        if (!isDataPacket) {
            return true;
        }
        if (!haveLastData_) {
            haveLastData_ = true;
            lastDataDbc_ = dbc;
            return true;
        }
        const uint8_t expected = static_cast<uint8_t>(lastDataDbc_ + blocksPerDataPacket_);
        const bool ok = (dbc == expected);
        lastDataDbc_ = dbc;
        return ok;
    }

    [[nodiscard]] uint8_t LastDataDbc() const noexcept { return lastDataDbc_; }
    [[nodiscard]] bool HasLastData() const noexcept { return haveLastData_; }

private:
    uint8_t blocksPerDataPacket_{0};
    bool haveLastData_{false};
    uint8_t lastDataDbc_{0};
};

} // namespace ASFW::Isoch::TxVerify

