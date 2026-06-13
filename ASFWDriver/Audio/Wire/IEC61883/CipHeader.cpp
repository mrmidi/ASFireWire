#include "CipHeader.hpp"

namespace ASFW::Protocols::Audio::IEC61883 {

// CIP header layout (IEC 61883-1, cross-checked against Linux
// sound/firewire/amdtp-stream.c macros and FFADO iec61883_packet):
//
//   Q0: [31:30]=00  [29:24]=SID  [23:16]=DBS  [15:14]=FN  [13:11]=QPC
//       [10]=SPH    [9:8]=reserved  [7:0]=DBC
//   Q1: [31:30]=10 (EOH)  [29:24]=FMT  [23:16]=FDF  [15:0]=SYT
//
// No-data packets normally use FDF=0xFF and SYT=0xFFFF. Device profiles may
// override the no-data FDF for hardware-specific compatibility. DBS keeps the
// stream value and DBC is carried unchanged. Values returned are logical
// quadlet values; bus byte order is applied at serialization time.

namespace {
constexpr uint32_t kEoh1 = 0x80000000u;
constexpr uint16_t kSytNoInfo = 0xFFFF;
} // namespace

CipHeaderBuilder::CipHeaderBuilder(const CipHeaderConfig& config) noexcept
    : config_(config) {}

void CipHeaderBuilder::Configure(const CipHeaderConfig& config) noexcept {
    config_ = config;
}

const CipHeaderConfig& CipHeaderBuilder::Config() const noexcept {
    return config_;
}

CipHeaderWords CipHeaderBuilder::BuildData(uint8_t dbc, uint16_t syt) const noexcept {
    CipHeaderWords words{};
    words.q0 = (static_cast<uint32_t>(config_.sid & 0x3F) << 24) |
               (static_cast<uint32_t>(config_.dbs) << 16) |
               (static_cast<uint32_t>(config_.fn & 0x3) << 14) |
               (static_cast<uint32_t>(config_.qpc & 0x7) << 11) |
               (config_.sph ? (1u << 10) : 0u) |
               static_cast<uint32_t>(dbc);
    words.q1 = kEoh1 |
               (static_cast<uint32_t>(config_.fmt & 0x3F) << 24) |
               (static_cast<uint32_t>(config_.fdf) << 16) |
               static_cast<uint32_t>(syt);
    return words;
}

CipHeaderWords CipHeaderBuilder::BuildNoData(uint8_t dbc) const noexcept {
    CipHeaderWords words{};
    words.q0 = (static_cast<uint32_t>(config_.sid & 0x3F) << 24) |
               (static_cast<uint32_t>(config_.dbs) << 16) |
               (static_cast<uint32_t>(config_.fn & 0x3) << 14) |
               (static_cast<uint32_t>(config_.qpc & 0x7) << 11) |
               (config_.sph ? (1u << 10) : 0u) |
               static_cast<uint32_t>(dbc);
    words.q1 = kEoh1 |
               (static_cast<uint32_t>(config_.fmt & 0x3F) << 24) |
               (static_cast<uint32_t>(config_.noDataFdf) << 16) |
               static_cast<uint32_t>(kSytNoInfo);
    return words;
}

} // namespace ASFW::Protocols::Audio::IEC61883
