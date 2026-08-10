import Testing
@testable import ASFW

struct MCPAudioStreamToolsTests {
    private func object(_ value: ASFWMCPValue?) -> [String: ASFWMCPValue] {
        guard case .object(let object)? = value else {
            Issue.record("Expected an MCP object.")
            return [:]
        }
        return object
    }

    private func cursor(
        streaming: Bool = true,
        stagedOldest: UInt64 = 10_000,
        stagedWrittenEnd: UInt64 = 14_096,
        finalizedEnd: UInt64 = 13_984,
        deadlineNoData: UInt64 = 0,
        staleXruns: UInt64 = 0,
        firstFaultReason: UInt32 = 0,
        transportStatus: UInt32 = 1
    ) -> ASFWMCPAudioCursorSnapshot {
        ASFWMCPAudioCursorSnapshot(
            guid: 0x0011_2233_4455_6677,
            bindingReady: true,
            streaming: streaming,
            sampleRateHz: 48_000,
            outputChannels: 2,
            stagedOldestFrame: stagedOldest,
            stagedWrittenEndFrame: stagedWrittenEnd,
            finalizedFrameEnd: finalizedEnd,
            completionPacket: 8_000,
            committedPacketEnd: 8_678,
            transportStatus: transportStatus,
            stagingWrites: 11,
            stagingFrames: 5_632,
            stagingDiscontinuities: 0,
            stagingOverwrittenFrames: 0,
            readsReady: 1_748,
            readsNotYetWritten: 3,
            readsStaleOverwritten: staleXruns,
            readsSnapshotBusy: 0,
            readsInvalid: 0,
            deferrals: 3,
            deadlineNoData: deadlineNoData,
            staleXruns: staleXruns,
            rebases: staleXruns,
            faultEvents: deadlineNoData + staleXruns,
            firstFaultReason: firstFaultReason,
            firstFaultPacket: 8_679,
            firstFaultAudioFrame: finalizedEnd,
            firstFaultOldestFrame: stagedOldest,
            firstFaultWrittenEndFrame: stagedWrittenEnd,
            firstFaultCompletionPacket: 8_000,
            firstFaultCommittedPacketEnd: 8_678
        )
    }

    @Test func cursorToolIsReadOnlyAndSeparatesFrameAndPacketUnits() async {
        let driver = MockASFWDriverControl()
        let transport = ASFWMCPMockTransport(
            core: ASFWMCPCore(
                configuration: .readOnlyDeveloper,
                driver: driver
            )
        )

        let result = await transport.callTool("asfw_get_audio_cursors")
        let root = object(result.data)
        guard case .array(let endpoints)? = root["endpoints"],
              case .object(let endpoint)? = endpoints.first else {
            Issue.record("Expected one endpoint cursor snapshot.")
            return
        }
        let frames = object(endpoint["frameCursors"])
        let packets = object(endpoint["transportCursors"])

        #expect(result.ok)
        #expect(root["endpointCount"] == .int(1))
        #expect(endpoint["snapshotKind"] == .string("valueOwned"))
        #expect(endpoint["bindingReady"] == .bool(true))
        #expect(endpoint["verdict"] == .string("healthyPendingContent"))
        #expect(frames["units"] == .string("absoluteHostFrames"))
        #expect(frames["pendingStagedFrames"] == .uint64(112))
        #expect(packets["units"] == .string("absoluteIsochPackets"))
        #expect(packets["committedMargin"] == .uint64(678))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func cursorVerdictsPreserveIdleDeadlineAndStaleDistinctions() {
        #expect(cursor(streaming: false).verdict == "idle")
        #expect(cursor(deadlineNoData: 1, firstFaultReason: 1).verdict ==
                "deadlineNoData")
        #expect(cursor(
            stagedOldest: 14_000,
            finalizedEnd: 13_984,
            staleXruns: 1,
            firstFaultReason: 2
        ).verdict == "staleXrun")
        #expect(cursor(firstFaultReason: 4).verdict == "fatal")
        #expect(cursor(transportStatus: 2).verdict == "fatal")
        #expect(cursor(transportStatus: 4).verdict == "fatal")
    }

    @Test func emptyReceiveCompletionIsAnExplicitRejectedCycle() {
        let health = ASFWMCPAudioStreamHealth(
            guid: 0x0011_2233_4455_6677,
            bindingReady: true,
            streaming: true,
            sampleRateHz: 48_000,
            inputChannels: 16,
            outputChannels: 16,
            packetsSeen: 1,
            dataPackets: 0,
            noDataPackets: 0,
            emptyCompletions: 1,
            shortPackets: 0,
            invalidCipHeaders: 0,
            zeroDataBlockSize: 0,
            geometryMismatch: 0,
            replayEntries: 0,
            replayEpochResets: 1
        )

        #expect(health.rejectedPackets == 1)
        #expect(health.verdict == "packetsRejected")
        #expect(health.explanation.contains("status-only/zero-length"))
        let counters = object(object(health.mcpValue())["counters"])
        #expect(counters["emptyCompletions"] == .uint64(1))
    }

    @Test func incompleteBindingIsVisibleAndNamed() {
        let health = ASFWMCPAudioStreamHealth(
            guid: 0x0011_2233_4455_6677,
            bindingReady: false,
            streaming: false,
            sampleRateHz: 48_000,
            inputChannels: 16,
            outputChannels: 8,
            packetsSeen: 0,
            dataPackets: 0,
            noDataPackets: 0,
            emptyCompletions: 0,
            shortPackets: 0,
            invalidCipHeaders: 0,
            zeroDataBlockSize: 0,
            geometryMismatch: 0,
            replayEntries: 0,
            replayEpochResets: 0
        )

        #expect(health.verdict == "bindingNotReady")
        #expect(object(health.mcpValue())["bindingReady"] == .bool(false))
    }
}
