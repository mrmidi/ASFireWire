// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Engine/Direct/Rx/RxCaptureChannelMap.hpp"
#include "../../Wire/AMDTP/AmdtpTypes.hpp"
#include "../../Wire/MOTU/MotuPortLayout.hpp"

#include <cstdint>

namespace ASFW::Audio {

/// Direct RX format descriptor collapsing wire format, slot sizing, channel counts,
/// and backend-specific format traits into a single value-type descriptor.
struct DirectRxFormatDescriptor final {
    ::ASFW::Encoding::AudioWireFormat wireFormat{::ASFW::Encoding::AudioWireFormat::kAM824};
    uint32_t am824Slots{0};
    uint32_t streamChannels{0};
    bool trustConfiguredStride{false};
    bool emptyPacketHasWrongDbc{false};
    uint32_t motuPcmChunks{0};
    ::ASFW::Encoding::Motu::MotuPortMap motuPorts{};
    AudioEngine::Direct::Rx::RxCaptureChannelMap captureChannelMap{};
};

} // namespace ASFW::Audio
