import Foundation
import Testing
@testable import ASFW

struct FireWireDeviceProfilesTests {
    @Test func generatedLibraryContainsExpectedMetadataProfiles() {
        #expect(FireWireDeviceProfiles.all.count > 100)

        let midas = FireWireDeviceProfiles.lookup(vendorId: 0x0010C73F,
                                                  modelId: 0x000001,
                                                  unitSpecifierId: 0x0010C73F,
                                                  unitVersion: 0x000001)
        #expect(midas?.modelName == "Venice F32")
        #expect(midas?.supportStatus == .supportedBinding)
        #expect(midas?.protocolFamily == .dice)

        let tascam = FireWireDeviceProfiles.lookup(vendorId: 0x00022E,
                                                   modelId: 0x000000,
                                                   unitSpecifierId: 0x00022E,
                                                   unitVersion: 0x800000)
        #expect(tascam?.modelName == "FW-1884")
        #expect(tascam?.supportStatus == .metadataOnly)

        let zed = FireWireDeviceProfiles.lookup(vendorId: 0x0004C4,
                                                modelId: 0x000000,
                                                unitSpecifierId: 0x0004C4,
                                                unitVersion: 0x000001)
        #expect(zed?.modelName == "Zed R16")
        #expect(zed?.protocolFamily == .dice)

        let echo = FireWireDeviceProfiles.lookup(vendorId: 0x001486,
                                                 modelId: 0x000AF2,
                                                 unitSpecifierId: 0x00A02D,
                                                 unitVersion: 0x010000)
        #expect(echo?.modelName == "AudioFire2")
        #expect(echo?.protocolFamily == .fireWorks)
    }

    @Test func weakVendorModelLookupSkipsUnitRequiredProfiles() {
        #expect(FireWireDeviceProfiles.lookup(vendorId: 0x00022E, modelId: 0x000000) == nil)
        #expect(FireWireDeviceProfiles.lookup(vendorId: 0x000D6C, modelId: 0x00010046)?.modelName == "FW 410")
    }

    @Test func midasPublishedDiceParserRequiresMidasAudioNub() {
        let midas = """
        "ASFWVendorID" = 1099583
        "ASFWModelID" = 1
        "ASFWDICEProtocol" = "TCAT DICE"
        "ASFWDICECapsSource" = "runtime"
        "ASFWDICERuntimeCapsValid" = Yes
        "ASFWDICEHostInputPcmChannels" = 32
        "ASFWDICEHostOutputPcmChannels" = 32
        "ASFWDICEDeviceToHostAm824Slots" = 33
        "ASFWDICEHostToDeviceAm824Slots" = 33
        "ASFWDICESampleRateHz" = 48000
        "ASFWDICEDeviceToHostIsoChannel" = 1
        "ASFWDICEHostToDeviceIsoChannel" = 0
        """
        #expect(MidasPublishedDiceStatus.parse(midas)?.inner.channelSummary == "32 in / 32 out")

        let alesis = midas
            .replacingOccurrences(of: "\"ASFWVendorID\" = 1099583", with: "\"ASFWVendorID\" = 1429")
            .replacingOccurrences(of: "\"ASFWModelID\" = 1", with: "\"ASFWModelID\" = 0")
        #expect(MidasPublishedDiceStatus.parse(alesis) == nil)
    }

    @Test func midasDriverProbeDiagnosticExplainsFailClosedPublication() {
        let output = """
        "ASFWDICELastProbeGUID" = 1217744273727168991
        "ASFWDICELastProbeVendorID" = 1099583
        "ASFWDICELastProbeModelID" = 1
        "ASFWDICELastProbeDeviceName" = "Midas Venice F32"
        "ASFWDICELastProbeProtocol" = "TCAT DICE"
        "ASFWDICELastProbeProfileSource" = "FFADO+systemd-hwdb"
        "ASFWDICELastProbeState" = "failed"
        "ASFWDICELastProbeFailReason" = "unsupported_multi_stream_geometry"
        "ASFWDICELastProbeCapsSource" = "runtime-discovery"
        "ASFWDICELastProbeHostInputPcmChannels" = 32
        "ASFWDICELastProbeHostOutputPcmChannels" = 32
        "ASFWDICELastProbeDeviceToHostAm824Slots" = 33
        "ASFWDICELastProbeHostToDeviceAm824Slots" = 33
        "ASFWDICELastProbeDeviceToHostActiveStreams" = 2
        "ASFWDICELastProbeHostToDeviceActiveStreams" = 2
        "ASFWDICELastProbeSampleRateHz" = 48000
        "ASFWDICELastProbeDeviceToHostIsoChannel" = 1
        "ASFWDICELastProbeHostToDeviceIsoChannel" = 0
        "ASFWDICELastProbeAttempt" = 3
        "ASFWDICELastProbeMaxAttempts" = 3
        "ASFWDICELastProbeStatus" = 0
        """

        let diagnostic = MidasDiceProbeDiagnostic.parse(output)
        #expect(diagnostic?.humanFailReason == "Unsupported multi-stream DICE geometry")
        #expect(diagnostic?.channelSummary == "32 in / 32 out")
        #expect(diagnostic?.streamSummary == "2 capture / 2 playback")

        let alesis = output
            .replacingOccurrences(of: "\"ASFWDICELastProbeVendorID\" = 1099583", with: "\"ASFWDICELastProbeVendorID\" = 1429")
            .replacingOccurrences(of: "\"ASFWDICELastProbeModelID\" = 1", with: "\"ASFWDICELastProbeModelID\" = 0")
        #expect(MidasDiceProbeDiagnostic.parse(alesis) == nil)
    }
}
