// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// IRMCSRConstants.hpp — IEEE 1394 IRM CSR addresses and spec values.

#pragma once

#include <cstdint>

namespace ASFW::Driver::IRMCSR {

// 1394 CSR offsets relative to CSR_REGISTER_BASE / 0xFFFF_F000_0000.
static constexpr uint32_t kCSRBusManagerIdOffset        = 0x0000021C;
static constexpr uint32_t kCSRBandwidthAvailableOffset  = 0x00000220;
static constexpr uint32_t kCSRChannelsAvailableHiOffset = 0x00000224;
static constexpr uint32_t kCSRChannelsAvailableLoOffset = 0x00000228;
static constexpr uint32_t kCSRBroadcastChannelOffset    = 0x00000234;

// Full 48-bit CSR addresses if useful in diagnostics.
static constexpr uint64_t kCSRBaseAddress               = 0xFFFFF0000000ULL;
static constexpr uint64_t kCSRBusManagerIdAddress       = kCSRBaseAddress + kCSRBusManagerIdOffset;
static constexpr uint64_t kCSRBandwidthAvailableAddress = kCSRBaseAddress + kCSRBandwidthAvailableOffset;
static constexpr uint64_t kCSRChannelsAvailableHiAddress= kCSRBaseAddress + kCSRChannelsAvailableHiOffset;
static constexpr uint64_t kCSRChannelsAvailableLoAddress= kCSRBaseAddress + kCSRChannelsAvailableLoOffset;
static constexpr uint64_t kCSRBroadcastChannelAddress   = kCSRBaseAddress + kCSRBroadcastChannelOffset;

// OHCI MMIO registers.
static constexpr uint32_t kCSRDataOffset                = 0x00C;
static constexpr uint32_t kCSRCompareDataOffset         = 0x010;
static constexpr uint32_t kCSRControlOffset             = 0x014;

static constexpr uint32_t kInitialBandwidthAvailableOffset  = 0x0B0;
static constexpr uint32_t kInitialChannelsAvailableHiOffset = 0x0B4;
static constexpr uint32_t kInitialChannelsAvailableLoOffset = 0x0B8;

// CSRControl selector values.
enum class CSRSelector : uint8_t {
    BusManagerId        = 0,
    BandwidthAvailable  = 1,
    ChannelsAvailableHi = 2,
    ChannelsAvailableLo = 3,
};

// Required initial values (IEEE 1394-2008).
// cross-validated with Linux: core.h:46 Apple: IOFireWireController.cpp:6302
static constexpr uint32_t kNoBusManagerId               = 0x0000003F;
static constexpr uint32_t kInitialBandwidthAvailable    = 0x00001333; // 4915 units
// Channel 31 reserved (bit 0 of Hi register).
// cross-validated with Linux: ohci.c:2492 Apple: IOFireWireIRM.cpp:238
static constexpr uint32_t kInitialChannelsAvailableHi   = 0xFFFFFFFE; 
static constexpr uint32_t kInitialChannelsAvailableLo   = 0xFFFFFFFF;

// BROADCAST_CHANNEL, software-owned.
// Integer bit layout: implemented bit = 0x80000000, valid bit = 0x40000000,
// channel field = 31.
static constexpr uint32_t kBroadcastChannelImplementedInvalid = 0x8000001F;
static constexpr uint32_t kBroadcastChannelImplementedValid   = 0xC000001F;

} // namespace ASFW::Driver::IRMCSR
