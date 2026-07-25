import Foundation

// Isochronous resource occupancy derived from the IRM's CHANNELS_AVAILABLE and
// BANDWIDTH_AVAILABLE registers.
//
// Scope note: these CSRs report **bus-wide availability**, not ownership. ASFW does
// not attribute an allocated channel to the node that allocated it, so this surface
// reports occupancy and says so explicitly (`ownershipAttributed: false`) rather than
// implying that the listed channels belong to ASFW.
//
// Bit convention (verified in-tree, not assumed): CHANNELS_AVAILABLE_HI carries
// channels 0..31 and LO carries 32..63, with channel N at bit (31 - N) of its word.
// `LocalIRMResourceController.cpp:81` tests `channelsAvailableHi & 0x1` for the
// channel-31 broadcast reservation, which fixes the ordering; that site is itself
// cross-validated against Linux `core-card.c:258` and Apple `IOFireWireIRM.cpp:301`.
// A **set** bit means the channel is free; a cleared bit means it is in use.

struct ASFWMCPIrmAllocationReport: Equatable {
    let generation: UInt32
    let irmNodeId: UInt32?
    let localIsIRM: Bool
    let readbackValid: Bool
    let bandwidthAvailable: UInt32
    let channelsAvailable31_0: UInt32
    let channelsAvailable63_32: UInt32

    /// Channel numbers whose availability bit is cleared, i.e. currently allocated by
    /// some node on the bus.
    var channelsInUse: [Int] {
        Self.clearedChannels(mask: channelsAvailable31_0, base: 0)
            + Self.clearedChannels(mask: channelsAvailable63_32, base: 32)
    }

    var freeChannelCount: Int { 64 - channelsInUse.count }

    private static func clearedChannels(mask: UInt32, base: Int) -> [Int] {
        (0..<32).compactMap { index in
            // Channel `base + index` occupies bit (31 - index) of its word.
            let bit = UInt32(1) << UInt32(31 - index)
            return (mask & bit) == 0 ? base + index : nil
        }
    }
}

extension ASFWMCPIrmAllocationReport {
    var mcpValue: ASFWMCPValue {
        let inUse = channelsInUse
        return .object([
            "generation": .int(Int(generation)),
            "irmNodeId": irmNodeId.map { .int(Int($0)) } ?? .null,
            "localIsIRM": .bool(localIsIRM),
            "readbackValid": .bool(readbackValid),
            "bandwidthAvailable": .int(Int(bandwidthAvailable)),
            "channelsAvailable31_0": .string(String(format: "0x%08X", channelsAvailable31_0)),
            "channelsAvailable63_32": .string(String(format: "0x%08X", channelsAvailable63_32)),
            "channelsInUse": .array(inUse.map { .int($0) }),
            "channelsInUseCount": .int(inUse.count),
            "freeChannelCount": .int(freeChannelCount),
            // The IRM CSRs cannot say *who* holds a channel. Reported explicitly so a
            // caller never reads this list as "allocations ASFW owns".
            "ownershipAttributed": .bool(false),
            "note": .string(
                "Bus-wide occupancy from the IRM resource CSRs. A cleared availability bit means "
                + "some node holds the channel; ASFW does not track which node. Channel 31 is "
                + "normally reserved for BROADCAST_CHANNEL."
            )
        ])
    }
}
