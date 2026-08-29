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
        pcmOldest: UInt64 = 10_000,
        pcmPublishedEnd: UInt64 = 14_096,
        scheduledEnd: UInt64 = 13_984,
        deadlineNoData: UInt64 = 0,
        wrongEpoch: UInt64 = 0,
        firstFaultReason: UInt32 = 0,
        transportStatus: UInt32 = 1
    ) -> ASFWMCPAudioCursorSnapshot {
        ASFWMCPAudioCursorSnapshot(
            endpointId: AudioEndpointID(101),
            deviceInstanceId: DeviceInstanceID(1),
            observedGuid: 0x0011_2233_4455_6677,
            bindingReady: true,
            streaming: streaming,
            sampleRateHz: 48_000,
            outputChannels: 2,
            pcmOldestValidFrame: pcmOldest,
            pcmPublishedEndFrame: pcmPublishedEnd,
            scheduledFrameEnd: scheduledEnd,
            completionPacket: 8_000,
            committedPacketEnd: 8_678,
            transportStatus: transportStatus,
            pcmPublications: 11,
            pcmFramesPublished: 5_632,
            pcmDiscontinuities: 0,
            pcmExpiredFrames: 0,
            copiesReady: 1_748,
            copiesNotYetPublished: 3,
            copiesExpired: 0,
            copiesConcurrentRewrite: 0,
            copiesInvalid: 0,
            deferrals: 3,
            deadlineNoData: deadlineNoData,
            copiesWrongEpoch: wrongEpoch,
            missedFrames: deadlineNoData * 8,
            faultEvents: deadlineNoData + wrongEpoch,
            firstFaultReason: firstFaultReason,
            firstFaultPacket: 8_679,
            firstFaultAudioFrame: scheduledEnd,
            firstFaultOldestFrame: pcmOldest,
            firstFaultWrittenEndFrame: pcmPublishedEnd,
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
        #expect(frames["publishedAheadFrames"] == .uint64(112))
        #expect(packets["units"] == .string("absoluteIsochPackets"))
        #expect(packets["committedMargin"] == .uint64(678))
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
    }

    @Test func cursorVerdictsPreserveIdleDeadlineAndStaleDistinctions() {
        #expect(cursor(streaming: false).verdict == "idle")
        #expect(cursor(deadlineNoData: 1, firstFaultReason: 1).verdict ==
                "deadlineNoData")
        #expect(cursor(
            pcmOldest: 14_000,
            scheduledEnd: 13_984,
            wrongEpoch: 0,
            firstFaultReason: 2
        ).verdict == "expiredRange")
        #expect(cursor(firstFaultReason: 4).verdict == "fatal")
        #expect(cursor(transportStatus: 2).verdict == "fatal")
        #expect(cursor(transportStatus: 4).verdict == "fatal")
    }

    @Test func emptyReceiveCompletionIsAnExplicitRejectedCycle() {
        let health = ASFWMCPAudioStreamHealth(
            endpointId: AudioEndpointID(101),
            deviceInstanceId: DeviceInstanceID(1),
            observedGuid: 0x0011_2233_4455_6677,
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
            replayEpochResets: 1,
            captureReaderActive: false,
            hasCompletedCaptureInterval: false,
            captureAvailableFrames: 0,
            captureCapacityFrames: 1_536,
            captureStarvationEvents: 0,
            captureTotalStarvedFrames: 0,
            captureIntervalStarvationEvents: 0,
            captureIntervalStarvedFrames: 0,
            captureOverrunEvents: 0
        )

        #expect(health.rejectedPackets == 1)
        #expect(health.verdict == "packetsRejected")
        #expect(health.explanation.contains("status-only/zero-length"))
        let counters = object(object(health.mcpValue())["counters"])
        #expect(counters["emptyCompletions"] == .uint64(1))
    }

    // The defect this verdict exists for: every packet counter is perfect and
    // the old rules returned "receivingData" while CoreAudio got pure silence,
    // because the RX write cursor and the HAL read cursor had different origins.
    @Test func decodedFramesThatNeverReachTheReaderAreNamed() {
        func health(readerActive: Bool,
                    completedInterval: Bool,
                    intervalStarvedFrames: UInt64) -> ASFWMCPAudioStreamHealth {
            ASFWMCPAudioStreamHealth(
                endpointId: AudioEndpointID(101),
                deviceInstanceId: DeviceInstanceID(1),
                observedGuid: 0x0011_2233_4455_6677,
                bindingReady: true,
                streaming: true,
                sampleRateHz: 48_000,
                inputChannels: 10,
                outputChannels: 6,
                packetsSeen: 3_103_932,
                dataPackets: 2_327_949,
                noDataPackets: 775_983,
                emptyCompletions: 0,
                shortPackets: 0,
                invalidCipHeaders: 0,
                zeroDataBlockSize: 0,
                geometryMismatch: 0,
                replayEntries: 3_103_932,
                replayEpochResets: 1,
                captureReaderActive: readerActive,
                hasCompletedCaptureInterval: completedInterval,
                captureAvailableFrames: 0,
                captureCapacityFrames: 1_536,
                captureStarvationEvents: 4_096,
                captureTotalStarvedFrames: 2_097_152,
                captureIntervalStarvationEvents: 32,
                captureIntervalStarvedFrames: intervalStarvedFrames,
                captureOverrunEvents: 0
            )
        }

        let starving = health(readerActive: true,
                              completedInterval: true,
                              intervalStarvedFrames: 16_384)
        #expect(starving.verdict == "framesNotReachingReader")
        #expect(starving.explanation.contains("zero-filled"))
        let capture = object(object(starving.mcpValue())["capture"])
        #expect(capture["intervalStarvedFrames"] == .uint64(16_384))
        #expect(capture["capacityFrames"] == .int(1_536))

        // Same counters, reader healthy: must stay receivingData.
        #expect(health(readerActive: true,
                       completedInterval: true,
                       intervalStarvedFrames: 0).verdict == "receivingData")
        // No reader attached, or no interval yet: starvation is expected and
        // must never be reported as a fault.
        #expect(health(readerActive: false,
                       completedInterval: true,
                       intervalStarvedFrames: 16_384).verdict == "receivingData")
        #expect(health(readerActive: true,
                       completedInterval: false,
                       intervalStarvedFrames: 16_384).verdict == "receivingData")
    }

    @Test func incompleteBindingIsVisibleAndNamed() {
        let health = ASFWMCPAudioStreamHealth(
            endpointId: AudioEndpointID(101),
            deviceInstanceId: DeviceInstanceID(1),
            observedGuid: 0x0011_2233_4455_6677,
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
            replayEpochResets: 0,
            captureReaderActive: false,
            hasCompletedCaptureInterval: false,
            captureAvailableFrames: 0,
            captureCapacityFrames: 1_536,
            captureStarvationEvents: 0,
            captureTotalStarvedFrames: 0,
            captureIntervalStarvationEvents: 0,
            captureIntervalStarvedFrames: 0,
            captureOverrunEvents: 0
        )

        #expect(health.verdict == "bindingNotReady")
        #expect(object(health.mcpValue())["bindingReady"] == .bool(false))
    }
}
