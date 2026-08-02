// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// IRMCSRConstants.hpp — IEEE 1394 IRM CSR addresses and spec values.

#pragma once

#include <cstdint>

namespace ASFW::Driver::IRMCSR {

/*
 * IRM CSR ownership model
 * =======================
 *
 * The local software does not "become IRM" by winning a CSR lock transaction.
 * IRM identity is derived from Self-ID after a bus reset: among contender-capable
 * nodes, the elected IRM is the contender with the highest physical ID. ASFW can
 * only make the local node eligible by advertising IRMC=1 in the bus information
 * block and ensuring that the Self-ID it transmits has both contender (c=1) and
 * link active (L=1). If a higher-ID eligible contender exists, that remote node
 * is IRM.
 *
 * Once a node is IRM, the core IRM resource CSRs below are a cooperative atomic
 * ledger, not a software arbitration callback:
 *
 *   BUS_MANAGER_ID
 *   BANDWIDTH_AVAILABLE
 *   CHANNELS_AVAILABLE_HI
 *   CHANNELS_AVAILABLE_LO
 *
 * On OHCI hardware these registers are hosted by the controller's autonomous CSR
 * engine. Remote quadlet reads and compare-swap locks to these addresses are
 * normally answered by OHCI itself; they do not traverse ASFW's software CSR
 * responder and therefore are not reliable software-observable telemetry. This
 * is intentional: remote nodes allocate bandwidth/channels by atomically CAS'ing
 * the IRM's ledger, and the IRM host's hardware preserves the atomicity.
 *
 * ASFW software participates in three cases:
 *
 *   1. Local eligibility/hosting setup:
 *      program InitialBandwidthAvailable / InitialChannelsAvailable* and expose
 *      BROADCAST_CHANNEL in software when local actually is the IRM.
 *      BROADCAST_CHANNEL lives outside the legacy OHCI 1.1 autonomous resource
 *      set: it was introduced by 1394a at CSR offset +0x234, so remote
 *      BROADCAST_CHANNEL reads/writes correctly fall through to AR request DMA
 *      and are answered by ASFW's software CSR responder.
 *
 *   2. Local software allocating/releasing resources from a remote IRM:
 *      issue normal async read/lock transactions over AT DMA.
 *
 *   3. Local software allocating/releasing resources from its own OHCI IRM:
 *      use the local CSRData, CSRCompareData, CSRControl sequence. This is a
 *      local hardware loopback path, not an inbound software CSR service.
 *
 * Diagnostics must follow that split. Count software-owned CSR requests
 * (STATE_SET/CLEAR, BROADCAST_CHANNEL, TOPOLOGY_MAP, legacy SPEED_MAP). For the
 * OHCI-owned IRM CSRs, report ownership/readback/health and unexpected software
 * hits, but do not present zero remote access counts as meaningful. An
 * unexpected software hit is not a normal allocation event; it means something
 * malformed or out-of-contract escaped OHCI's autonomous path, such as a block
 * read, quadlet write, non-compare-swap lock, or otherwise unsupported tcode to
 * a hardware-owned IRM resource offset.
 */

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
