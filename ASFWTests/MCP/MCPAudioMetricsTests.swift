import Foundation
import Testing
@testable import ASFW

struct MCPAudioMetricsTests {
    @Test func snapshotToolIsReadOnlyAndReturnsStructuredTxRxState() async {
        let core = ASFWMCPCore(
            configuration: .readOnlyDeveloper,
            driver: MockASFWDriverControl()
        )
        let result = await core.callTool(
            name: "asfw_audio_stream_snapshot",
            arguments: .object(["guid": .string("0x0011223344556677")])
        )

        guard case .object(let data) = result.data,
              case .object(let state)? = data["state"],
              case .object(let tx)? = data["tx"],
              case .object(let rx)? = data["rx"] else {
            Issue.record("Expected a structured audio metrics snapshot.")
            return
        }
        #expect(result.ok)
        #expect(data["status"] == .string("ok"))
        #expect(state["streaming"] == .bool(true))
        #expect(state["consistent"] == .bool(true))
        #expect(tx["replayUnderflows"] == .uint64(0))
        #expect(rx["replayEpochResets"] == .uint64(0))

        let definition = ASFWMCPToolCatalog.all.first {
            $0.name == "asfw_audio_stream_snapshot"
        }
        #expect(definition?.readOnly == true)
        #expect(definition?.idempotent == true)
        #expect(definition?.visibility == .readOnly)
    }

    @Test func snapshotToolRejectsMissingAndUnknownGuid() async {
        let core = ASFWMCPCore(
            configuration: .readOnlyDeveloper,
            driver: MockASFWDriverControl()
        )
        let missing = await core.callTool(name: "asfw_audio_stream_snapshot")
        #expect(missing.ok == false)
        #expect(missing.errors.first?.code == .malformedRequest)

        let unknown = await core.callTool(
            name: "asfw_audio_stream_snapshot",
            arguments: .object(["guid": .uint64(0xFFFF)])
        )
        #expect(unknown.ok == false)
        #expect(unknown.errors.first?.code == .capabilityUnavailable)
    }

    @Test func captureSessionRetainsTwoHourReadOnlyRunAndRecordsManualMarker() async {
        let core = ASFWMCPCore(
            configuration: .readOnlyDeveloper,
            driver: MockASFWDriverControl()
        )
        let start = await core.callTool(
            name: "asfw_audio_stream_capture_start",
            arguments: .object(["guid": .string("0x0011223344556677")])
        )
        guard case .object(let started) = start.data else {
            Issue.record("Expected capture start data.")
            return
        }
        #expect(start.ok)
        #expect(started["active"] == .bool(true))
        #expect(started["intervalHz"] == .int(2))
        #expect(started["capacity"] == .int(14_400))
        #expect(started["sampleCount"] == .int(1))

        let mark = await core.callTool(name: "asfw_audio_stream_capture_mark")
        guard case .object(let marked) = mark.data,
              case .array(let markers)? = marked["markers"],
              case .object(let marker)? = markers.first else {
            Issue.record("Expected a manual misframe marker.")
            return
        }
        #expect(mark.ok)
        #expect(markers.count == 1)
        #expect(marker["kind"] == .string("manualMisframe"))
        #expect(marker["logLatestSequence"] == .uint64(42))

        let status = await core.callTool(name: "asfw_audio_stream_capture_status")
        guard case .object(let statusData) = status.data else {
            Issue.record("Expected capture status data.")
            return
        }
        #expect(status.ok)
        #expect(statusData["samples"] == .null)

        let stop = await core.callTool(name: "asfw_audio_stream_capture_stop")
        guard case .object(let stopped) = stop.data else {
            Issue.record("Expected capture stop data.")
            return
        }
        #expect(stop.ok)
        #expect(stopped["active"] == .bool(false))

        let exported = await core.callTool(name: "asfw_audio_stream_capture_export")
        guard case .object(let exportData) = exported.data,
              case .array(let samples)? = exportData["samples"] else {
            Issue.record("Expected retained capture samples.")
            return
        }
        #expect(exported.ok)
        #expect(samples.count >= 2)

        let names = [
            "asfw_audio_stream_capture_start",
            "asfw_audio_stream_capture_mark",
            "asfw_audio_stream_capture_status",
            "asfw_audio_stream_capture_stop",
            "asfw_audio_stream_capture_export",
        ]
        for name in names {
            let definition = ASFWMCPToolCatalog.all.first { $0.name == name }
            #expect(definition?.readOnly == true)
            #expect(definition?.visibility == .readOnly)
        }
    }

    @Test func captureDeltaResetsOnGenerationChange() {
        let previous = ASFWAudioStreamMetricsSnapshot.mock(guid: 0x0011_2233_4455_6677)
        let restarted = ASFWAudioStreamMetricsSnapshot(
            status: previous.status,
            stateFlags: previous.stateFlags,
            guid: previous.guid,
            timestampNs: previous.timestampNs + 1,
            endpointGeneration: previous.endpointGeneration,
            streamGeneration: previous.streamGeneration + 1,
            sampleRateHz: previous.sampleRateHz,
            outputChannels: previous.outputChannels,
            inputChannels: previous.inputChannels,
            ioCallbackGeneration: previous.ioCallbackGeneration,
            ioCallbackErrorGeneration: previous.ioCallbackErrorGeneration,
            ioLastError: previous.ioLastError,
            fatalGeneration: previous.fatalGeneration,
            fatalReason: previous.fatalReason,
            discontinuities: previous.discontinuities,
            tx: previous.tx,
            rx: previous.rx
        )
        #expect(ASFWAudioStreamMetricsCaptureDelta.between(
            previous: previous, current: previous
        )?.packetsTX == 0)
        #expect(ASFWAudioStreamMetricsCaptureDelta.between(
            previous: previous, current: restarted
        ) == nil)
    }

    @Test func connectorDecoderRejectsTruncationAndMapsVersionedWireStruct() {
        var wire = ASFWAudioStreamMetricsSnapshotV1()
        wire.abiVersion = UInt32(ASFW_AUDIO_STREAM_METRICS_ABI_VERSION)
        wire.structSize = UInt32(MemoryLayout.size(ofValue: wire))
        wire.status = 0
        wire.stateFlags = ASFWAudioStreamMetricsSnapshot.configAvailableFlag |
            ASFWAudioStreamMetricsSnapshot.controlAvailableFlag |
            ASFWAudioStreamMetricsSnapshot.consistentFlag
        wire.guid = 0x0011_2233_4455_6677
        wire.sampleRateHz = 48_000
        wire.outputChannels = 28
        wire.inputChannels = 28
        wire.streamGeneration = 7
        wire.txPackets = 123
        wire.txReplayUnderflows = 4
        wire.rxPackets = 456
        wire.rxReplayEpochResets = 2

        let data = withUnsafeBytes(of: &wire) { Data($0) }
        let decoded = ASFWDriverConnector.decodeAudioStreamMetricsSnapshot(data)

        #expect(decoded?.guid == 0x0011_2233_4455_6677)
        #expect(decoded?.consistent == true)
        #expect(decoded?.streamGeneration == 7)
        #expect(decoded?.tx.packets == 123)
        #expect(decoded?.tx.replayUnderflows == 4)
        #expect(decoded?.rx.packets == 456)
        #expect(decoded?.rx.replayEpochResets == 2)
        #expect(ASFWDriverConnector.decodeAudioStreamMetricsSnapshot(
            data.prefix(data.count - 1)) == nil)
    }
}
