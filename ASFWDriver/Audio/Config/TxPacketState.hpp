#pragma once

#include <cstdint>

namespace ASFW::Audio::Runtime {

enum class TxBlockingResult : uint8_t {
    Data = 0,
    NoData,
};

inline const char* ToString(TxBlockingResult result) noexcept {
    switch (result) {
        case TxBlockingResult::Data:   return "Data";
        case TxBlockingResult::NoData: return "NoData";
        default:                       return "Unknown";
    }
}

enum class TxPacketState : uint8_t {
    Unknown = 0,
    NoPhaseSilence,
    ValidPhaseSilence,
    ValidPhasePcm,
    UnderrunSilence,
    StaleSync,
    InvalidGeometry,
};

inline const char* ToString(TxPacketState state) noexcept {
    switch (state) {
        case TxPacketState::NoPhaseSilence:    return "NoPhaseSilence";
        case TxPacketState::ValidPhaseSilence:  return "ValidPhaseSilence";
        case TxPacketState::ValidPhasePcm:      return "ValidPhasePcm";
        case TxPacketState::UnderrunSilence:    return "UnderrunSilence";
        case TxPacketState::StaleSync:          return "StaleSync";
        case TxPacketState::InvalidGeometry:    return "InvalidGeometry";
        default:                                return "Unknown";
    }
}

struct TxPacketProductionResult {
    TxPacketState state{TxPacketState::Unknown};
    TxBlockingResult blockingResult{TxBlockingResult::NoData};
    bool hasValidPhase{false};
    bool hasPayloadFrames{false};
    bool fatal{false};
    uint16_t syt{0xFFFF};
    uint8_t dbc{0};
    uint32_t frames{0};
    uint32_t quadlets{0};
    uint64_t syncAgeTicks{0};
    uint64_t underrunDistanceFrames{0};
    uint32_t sourceReadStatus{0};
};

} // namespace ASFW::Audio::Runtime
