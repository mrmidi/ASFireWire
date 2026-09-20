// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../Wire/AMDTP/AmdtpTypes.hpp"
#include <cstdint>

namespace ASFW::Audio {

/// Physical presentation plan for one transmit cycle.
/// Emitted by the timeline/cadence planner and encoded by the wire packetizer.
/// Invariant: a wire packetizer encodes this plan and owns no HAL frame cursor.
struct TxPresentationPlan final {
    uint64_t epoch{0};
    uint64_t cycleOrdinal{0};
    uint64_t firstAudioFrame{0};
    uint32_t frameCount{0};
    uint64_t presentationBusTicks{0};
    Protocols::Audio::AMDTP::AmdtpPacketDisposition disposition{
        Protocols::Audio::AMDTP::AmdtpPacketDisposition::NoData};
};

} // namespace ASFW::Audio

namespace ASFW::Protocols::Audio::AMDTP {
using TxPresentationPlan = ::ASFW::Audio::TxPresentationPlan;
} // namespace ASFW::Protocols::Audio::AMDTP
