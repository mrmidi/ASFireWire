// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBShellTelemetryTests.swift — parser tests against verbatim device output.

import Testing
@testable import ASFW

struct BeBoBShellTelemetryTests {

    // MARK: - sys stat

    @Test func parsesGlobalBlockAsLabelValuePairsNotColumns() throws {
        let stats = try #require(BeBoBShellTelemetryParser.parseStreamingStats(BeBoBShellFixtures.sysStat))
        // The firmware prints three DIFFERENT counters per line
        // ("rxIsr %9d  rxStreamInvalid %5d  MDBAliveErrors %6d"), so all three
        // must be picked up from a single line.
        #expect(stats.global["rxIsr"] == 9_691_179)
        #expect(stats.global["rxStreamInvalid"] == 0)
        #expect(stats.global["MDBAliveErrors"] == 0)
        #expect(stats.global["txIsr"] == 6_116_574)
        #expect(stats.global["rxIsrH2"] == 0)
        #expect(stats.global["rxLLCAlarmIntrv"] == 0)
    }

    @Test func identifiesEachStreamColumnByItsIsoChannelAndEndpoint() throws {
        let stats = try #require(BeBoBShellTelemetryParser.parseStreamingStats(BeBoBShellFixtures.sysStat))

        #expect(stats.outputs.count == 3)
        #expect(stats.outputs.map(\.isoChannel) == [58, 1, 60])
        #expect(stats.outputs.map(\.endpoint) == [.av, .fireWire, .av])

        #expect(stats.inputs.count == 3)
        #expect(stats.inputs.map(\.isoChannel) == [0, 61, 62])
        #expect(stats.inputs.map(\.endpoint) == [.fireWire, .av, .av])
    }

    /// The defect this parser exists to prevent: the first value on each row of
    /// the OUTPUT table belongs to iso channel 58, the device's internal
    /// S/PDIF-ADAT path. Reading column 0 reports that stream's timing as ours.
    @Test func fireWireOutputIsNotTheFirstColumn() throws {
        let stats = try #require(BeBoBShellTelemetryParser.parseStreamingStats(BeBoBShellFixtures.sysStat))
        let ours = try #require(stats.fireWireOutput)
        let firstColumn = try #require(stats.outputs.first)

        #expect(ours.isoChannel == 1)
        #expect(firstColumn.isoChannel == 58)

        // Same rows, different streams, different numbers.
        #expect(ours.value("SytOffset") == 12288)
        #expect(firstColumn.value("SytOffset") == 9216)
        #expect(ours.value("pkt Future") == 0)
        #expect(firstColumn.value("pkt Future") == 4)
        #expect(ours.value("FFLimit") == 56520)
        #expect(firstColumn.value("FFLimit") == 53448)
    }

    @Test func readsOurTransmitStreamCountersFromTheInputTable() throws {
        let stats = try #require(BeBoBShellTelemetryParser.parseStreamingStats(BeBoBShellFixtures.sysStat))
        let ours = try #require(stats.fireWireInput)

        #expect(ours.isoChannel == 0)
        #expect(ours.value("rxPackets") == 3_882_332)
        #expect(ours.value("rxEmptyPkt") == 1_294_172)
        #expect(ours.value("onlyHeaders") == 0)
        #expect(ours.value("BCOHdrErr") == 0)
        #expect(ours.value("CtrDiffErr") == 1)
        #expect(ours.value("SytDiffErr") == 2)
        #expect(ours.value("rxNoMem") == 1652)
        #expect(ours.value("linStartSyt") == 9_668_096)
    }

    @Test func parsesLabelsContainingSpacesAndPercentRows() throws {
        let stats = try #require(BeBoBShellTelemetryParser.parseStreamingStats(BeBoBShellFixtures.sysStat))
        let output = try #require(stats.fireWireOutput)
        let input = try #require(stats.fireWireInput)

        // "pkt Past" / "pkt Future" are two-token labels.
        #expect(output.counters["pkt Past"] != nil)
        #expect(output.counters["pkt Future"] != nil)
        // "%7d %%" rows keep the number and drop the marker.
        #expect(output.value("txQFillLevel") == 33)
        #expect(input.value("rxQFillLevel") == 80)
        #expect(input.value("PoolFillLevel") == 90)
    }

    @Test func recordsTheSeverityPrefixTheFirmwarePrints() throws {
        let stats = try #require(BeBoBShellTelemetryParser.parseStreamingStats(BeBoBShellFixtures.sysStat))
        let input = try #require(stats.fireWireInput)
        let output = try #require(stats.fireWireOutput)

        #expect(input.severity["rxEmptyPkt"] == "W")   // warning
        #expect(input.severity["BCOHdrErr"] == "E")    // error
        #expect(output.severity["SytOffset"] == "S")   // status
        #expect(input.severity["onlyHeaders"] == nil)  // informational, no prefix
    }

    @Test func doesNotConfuseSimilarlyNamedCounters() throws {
        let stats = try #require(BeBoBShellTelemetryParser.parseStreamingStats(BeBoBShellFixtures.sysStat))
        let input = try #require(stats.fireWireInput)
        // rxEmptyPkt and rxEMIEtyPkt are distinct firmware labels, as are
        // rxToLong and rxPktToLong.
        #expect(input.value("rxEmptyPkt") == 1_294_172)
        #expect(input.value("rxEMIEtyPkt") == 0)
        #expect(input.counters["rxToLong"] != nil)
        #expect(input.counters["rxPktToLong"] != nil)
    }

    // MARK: - sys avstat

    @Test func latchPresenceIsNotLatchValue() throws {
        let stat = try #require(BeBoBShellTelemetryParser.parseAvStat(BeBoBShellFixtures.sysAvStatAll))

        // All of these labels are printed in every dump. Three read zero.
        #expect(stat.latch("SetTgInLock", block: "TGEN") != nil)
        #expect(stat.latch("SetTgSytMiss", block: "TGEN") != nil)
        #expect(stat.latch("CIPMismatch") != nil)

        #expect(stat.setTgInLock)          // 00000001
        #expect(!stat.setTgSytMiss)        // 00000000
        #expect(!stat.cipMismatch)         // 00000000
        #expect(!stat.dbcMismatch)         // 00000000
        #expect(!stat.fmtMismatch)         // 00000000
        #expect(!stat.headerMismatch)      // 00000000
        #expect(stat.sidMismatch)          // 00000001
    }

    @Test func parsesHexValuesNotDecimal() throws {
        let stat = try #require(BeBoBShellTelemetryParser.parseAvStat(BeBoBShellFixtures.sysAvStatAll))
        // Values are hex without an 0x prefix. A decimal parse stops at the
        // first letter and silently yields 0 — turning a live latch into an
        // all-clear.
        #expect(stat.latch("AV2FifoEmpty(FEA00804)")?.value == 0xFF)
        #expect(stat.latch("LLCInt(FEF00C00)")?.value == 0xD018_053C)
        #expect(stat.latch("IsoTxAlarmID(FEE00800)")?.value == 0x3E)
    }

    @Test func scopesDuplicateLabelsToTheirBlock() throws {
        let stat = try #require(BeBoBShellTelemetryParser.parseAvStat(BeBoBShellFixtures.sysAvStatAll))
        // SetTSErr, SyncWarn, DataErr and IntMask all appear under both AV1 and
        // AV2 with different values, so an unscoped lookup is ambiguous.
        #expect(stat.isSet("SetTSErr", block: "AV1"))
        #expect(!stat.isSet("SetTSErr", block: "AV2"))
        #expect(stat.isSet("SyncWarn", block: "AV1"))
        #expect(stat.isSet("SyncWarn", block: "AV2"))
    }

    @Test func blockHeadersAreNotLatches() throws {
        let stat = try #require(BeBoBShellTelemetryParser.parseAvStat(BeBoBShellFixtures.sysAvStatAll))
        let blocks = Set(stat.latches.map(\.block))
        #expect(blocks.contains("TGEN"))
        #expect(blocks.contains("FRAMER/DEFRAMER"))
        #expect(blocks.contains("MDB"))
        // "TGEN : FE700000" names a peripheral base address; it is not a latch.
        #expect(stat.latch("TGEN") == nil)
        #expect(stat.latch("AV1") == nil)
    }

    @Test func listsOnlyTheLatchesActuallySet() throws {
        let stat = try #require(BeBoBShellTelemetryParser.parseAvStat(BeBoBShellFixtures.sysAvStatAll))
        let set = Set(stat.setLatches.map(\.label))
        #expect(set.contains("SetTgInLock"))
        #expect(set.contains("SIDMismatch"))
        #expect(set.contains("IsoTXalarm"))
        #expect(!set.contains("SetTgSytMiss"))
        #expect(!set.contains("DBCMismatch"))
    }

    // MARK: - fw show

    @Test func parsesFwShowStateRateAndDigitalFormat() throws {
        let sync = try #require(BeBoBShellTelemetryParser.parseSyncState(BeBoBShellFixtures.fwShow))
        #expect(sync.audioState == "Running")
        #expect(sync.sampleRateHz == 48000)
        #expect(sync.syncSource == "Internal Digital Input Sync")
        // The digital format selects which half of the hardcoded channel
        // formation table is correct.
        #expect(sync.inputSource == "SPDIF")
        #expect(sync.outputSource == "SPDIF")
        #expect(sync.spdifSource == "RCA")
    }

    @Test func isoChannelRowsAreAssignmentsAndMayBeUnassigned() throws {
        let sync = try #require(BeBoBShellTelemetryParser.parseSyncState(BeBoBShellFixtures.fwShow))
        // These are channel NUMBERS, not channel counts, and -1 is legal.
        #expect(sync.swReturnChannel == 0)   // host -> device
        #expect(sync.swSendChannel == 1)     // device -> host
        #expect(sync.isoChannels["LineIn"] == 62)
        #expect(sync.isoChannels["MixerOut"] == 60)
        #expect(sync.isoChannels["MidiIn"] == -1)
        #expect(sync.isoChannels["AC3"] == -1)
    }

    /// The `sys stat` iso channels must agree with `fw show`'s assignments —
    /// that cross-check is what proves which column is ours.
    @Test func sysStatColumnsAgreeWithFwShowAssignments() throws {
        let stats = try #require(BeBoBShellTelemetryParser.parseStreamingStats(BeBoBShellFixtures.sysStat))
        let sync = try #require(BeBoBShellTelemetryParser.parseSyncState(BeBoBShellFixtures.fwShow))

        #expect(stats.fireWireInput?.isoChannel == sync.swReturnChannel)
        #expect(stats.fireWireOutput?.isoChannel == sync.swSendChannel)
    }

    // MARK: - robustness

    @Test func returnsNilRatherThanAnEmptyModelForJunk() {
        #expect(BeBoBShellTelemetryParser.parseStreamingStats("") == nil)
        #expect(BeBoBShellTelemetryParser.parseAvStat("") == nil)
        #expect(BeBoBShellTelemetryParser.parseSyncState("") == nil)
        #expect(BeBoBShellTelemetryParser.parseStreamingStats("Invalid path!\n/cfg>") == nil)
    }

    @Test func handlesTheIdleDeviceWithNoOutputStreams() throws {
        let idle = """
        sys stat
        global statistic:
        rxIsr              0    rxStreamInvalid     0    MDBAliveErrors      0

        Output Stream statistic:
        No active output streams.
        /cfg>
        """
        let stats = try #require(BeBoBShellTelemetryParser.parseStreamingStats(idle))
        #expect(stats.noActiveOutputStreams)
        #expect(stats.outputs.isEmpty)
        #expect(stats.fireWireOutput == nil)
        #expect(stats.global["rxIsr"] == 0)
    }

}
