import Combine
import AppKit
import Foundation
import IOKit
import SwiftUI
import Testing
@testable import ASFW

/// The "asfw.controls.v1" document (driver: UserClient/WireFormats/ControlSnapshotDocument.hpp),
/// step snapping, the value wire encoding, and the write -> poll -> confirm flow against a fake driver.
@MainActor
struct ControlSnapshotTests {
    /// Shaped like the driver's output: numbers with 4 decimals, an AV/C feature
    /// block (master on the node, channels on output ports), a selector router,
    /// plus an endpoint, an enum, a value-less scalar and a crosspoint the AV/C
    /// projection does not emit today.
    static func document(revision: UInt64 = 7, masterLevel: String = "-10.5000", masterMute: String = "false") -> Data {
        Data("""
        {"schema":"asfw.controls.v1","guid":"0x000aac0300b1d1f7","family":"avc","revision":\(revision),
         "nodes":[
          {"id":66817,"name":"Master Feature","kind":"processor","inputs":[1,3],"outputs":[2,4]},
          {"id":66561,"name":"Clock Selector","kind":"router","inputs":[6,7],"outputs":[5],
           "bundles":[{"id":1,"routes":[[6,5]]},{"id":2,"routes":[[7,5]]}],"activeBundles":[2]},
          {"id":9,"name":"Host","kind":"endpoint","host":true}],
         "ports":[
          {"id":1,"owner":66817,"direction":"in","channels":1,"name":"Ch 1 In"},
          {"id":2,"owner":66817,"direction":"out","channels":1,"name":"Ch 1 Out"},
          {"id":3,"owner":66817,"direction":"in","channels":1,"name":"Ch 2 In"},
          {"id":4,"owner":66817,"direction":"out","channels":1,"name":"Ch 2 Out"},
          {"id":5,"owner":66561,"direction":"out","channels":1,"name":"Out"},
          {"id":6,"owner":66561,"direction":"in","channels":1,"name":"In 1"},
          {"id":7,"owner":66561,"direction":"in","channels":1,"name":"In 2"}],
         "parameters":[
          {"id":100,"name":"Master Volume","semantic":"level","target":{"node":66817},
           "domain":{"kind":"scalar","min":-60.0000,"max":6.0000,"step":0.5000,"unit":"dB"},
           "value":\(masterLevel),"origin":"observed"},
          {"id":101,"name":"Master Mute","semantic":"mute","target":{"node":66817},
           "domain":{"kind":"bool"},"value":\(masterMute),"origin":"observed"},
          {"id":102,"name":"Ch 1 Volume","semantic":"level","target":{"port":2},
           "domain":{"kind":"scalar","min":-60.0000,"max":6.0000,"step":0.5000,"unit":"dB"},
           "value":-3.0000,"origin":"cached"},
          {"id":103,"name":"Ch 2 Mute","semantic":"mute","target":{"port":4},
           "domain":{"kind":"bool"},"value":true,"origin":"derived"},
          {"id":104,"name":"Nominal","semantic":"nominalLevel","target":{"node":66817},
           "domain":{"kind":"enum","values":[{"value":-10,"name":"-10 dBV"},{"value":4,"name":"+4 dBu"}]},
           "value":4,"origin":"observed"},
          {"id":105,"name":"Pan","semantic":"pan","target":{"crosspoint":12},
           "domain":{"kind":"scalar","min":null,"max":1.0000,"unit":"normalized"},"value":null,"origin":null},
          {"id":106,"name":"Future","semantic":"somethingNew","target":{"port":2},
           "domain":{"kind":"bool"},"value":false,"origin":"observed"}]}
        """.utf8)
    }


    @Test func decodesAccessAndModeDependentRange() throws {
        var object = try #require(JSONSerialization.jsonObject(with: Self.document()) as? [String: Any])
        var parameters = try #require(object["parameters"] as? [[String: Any]])
        parameters[0]["access"] = "readOnly"
        parameters[0]["applicable"] = false
        parameters[0]["domainOverride"] = ["kind": "scalar", "min": -20, "max": 0, "step": 1, "unit": "dB"]
        object["parameters"] = parameters
        let parameter = try ControlSnapshot.decode(JSONSerialization.data(withJSONObject: object)).parameters[0]
        #expect(!parameter.isWritable)
        #expect(parameter.legalValue(.double(-40)) == .double(-20))
        #expect(parameter.legalValue(.double(-5.2)) == .double(-5))
    }

    @Test func actionsHaveNoRememberedValueAndEncodeOnlyTriggers() throws {
        var object = try #require(JSONSerialization.jsonObject(with: Self.document()) as? [String: Any])
        object["parameters"] = [["id": 200, "name": "Clear", "semantic": "unknown",
                                  "target": ["node": 66817], "domain": ["kind": "action"],
                                  "value": NSNull(), "origin": NSNull()]]
        let action = try ControlSnapshot.decode(JSONSerialization.data(withJSONObject: object)).parameters[0]
        #expect(action.value == nil)
        #expect(action.origin == nil)
        #expect(action.legalValue(.action) == .action)
        #expect(action.legalValue(.bool(true)) == nil)
        #expect(ControlValue.action.wire.kind == 3)
        #expect(ControlValue.action.wire.bits == 0)
        #expect(ControlValue(kind: 3, bits: 1) == nil)
        #expect(ControlParameterChange(scalars: [1, ControlParameterChange.notificationReason, 1, 7, 200, 3, 0, 0]) == nil)
    }


    @Test func refusesReadOnlyAndInactiveWritesBeforeSending() async throws {
        let driver = FakeControlsDriver()
        var object = try #require(JSONSerialization.jsonObject(with: Self.document()) as? [String: Any])
        var parameters = try #require(object["parameters"] as? [[String: Any]])
        parameters[0]["access"] = "readOnly"
        parameters[1]["applicable"] = false
        object["parameters"] = parameters
        driver.documentOverride = try JSONSerialization.data(withJSONObject: object)
        let model = DeviceControlsViewModel(source: driver)
        model.reload()
        await model.write(100, .double(-20))
        await model.write(101, .bool(true))
        #expect(driver.writes.isEmpty)
        #expect(model.writeState(100) == .failed("This control is read-only."))
        #expect(model.writeState(101) == .failed("This control is not applicable."))
    }

    @Test func rejectsModeRangeWithWrongUnit() throws {
        var object = try #require(JSONSerialization.jsonObject(with: Self.document()) as? [String: Any])
        var parameters = try #require(object["parameters"] as? [[String: Any]])
        parameters[0]["domainOverride"] = ["kind": "scalar", "min": -20, "max": 0, "unit": "Hz"]
        object["parameters"] = parameters
        let data = try JSONSerialization.data(withJSONObject: object)
        #expect(throws: (any Error).self) { try ControlSnapshot.decode(data) }
    }

    // MARK: Decoding

    @Test func decodesTheDocument() throws {
        let snapshot = try ControlSnapshot.decode(Self.document())
        #expect(snapshot.family == "avc")
        #expect(snapshot.revision == 7)
        #expect(snapshot.guid == "0x000aac0300b1d1f7")
        #expect(snapshot.nodes.map(\.kind) == [.processor, .router, .endpoint])
        #expect(snapshot.ports.count == 7)
        #expect(snapshot.port(5)?.direction == .output)
        #expect(snapshot.port(6)?.direction == .input)
        #expect(snapshot.parameters.count == 7)
    }

    @Test func decodesTheThreeDomainsAndValueByDomain() throws {
        let snapshot = try ControlSnapshot.decode(Self.document())
        let level = try #require(snapshot.parameters.first { $0.id == 100 })
        #expect(level.semantic == .level)
        #expect(level.target == .node(66817))
        #expect(level.domain == .scalar(ControlScalarDomain(min: -60, max: 6, step: 0.5, unit: "dB")))
        #expect(level.value == .double(-10.5))

        let mute = try #require(snapshot.parameters.first { $0.id == 101 })
        #expect(mute.domain == .bool)
        #expect(mute.value == .bool(false))

        let nominal = try #require(snapshot.parameters.first { $0.id == 104 })
        #expect(nominal.domain == .enumeration([.init(value: -10, name: "-10 dBV"), .init(value: 4, name: "+4 dBu")]))
        #expect(nominal.value == .int(4))

        // A non-finite bound arrives as null; no value means no origin either.
        let pan = try #require(snapshot.parameters.first { $0.id == 105 })
        guard case .scalar(let panDomain) = pan.domain else { Issue.record("pan is scalar"); return }
        #expect(panDomain.min == nil && panDomain.step == nil && panDomain.range == nil)
        #expect(pan.value == nil && pan.origin == nil)
        #expect(pan.target == .crosspoint(12))

        #expect(snapshot.parameters.first { $0.id == 106 }?.semantic == .unknown)
    }

    @Test func decodesTheThreeOrigins() throws {
        let snapshot = try ControlSnapshot.decode(Self.document())
        let origins = Dictionary(uniqueKeysWithValues: snapshot.parameters.map { ($0.id, $0.origin) })
        #expect(origins[100] == .observed)
        #expect(origins[102] == .cached)
        #expect(origins[103] == .derived)
    }

    @Test func decodesRouterBundlesAndTheActiveOne() throws {
        let snapshot = try ControlSnapshot.decode(Self.document())
        let router = try #require(snapshot.node(66561))
        #expect(router.kind == .router)
        #expect(router.bundles == [ControlBundle(id: 1, routes: [ControlRoute(input: 6, output: 5)]),
                                   ControlBundle(id: 2, routes: [ControlRoute(input: 7, output: 5)])])
        #expect(router.activeBundles == [2])
        let endpoint = try #require(snapshot.node(9))
        #expect(endpoint.inputs.isEmpty && endpoint.outputs.isEmpty && endpoint.host == true)
        #expect(endpoint.bundles.isEmpty)
    }

    @Test func groupsParametersUnderTheirNode() throws {
        let snapshot = try ControlSnapshot.decode(Self.document())
        // Node-wide parameters first, then the channels' (through their port's owner).
        #expect(snapshot.parameters(of: 66817).map(\.id) == [100, 101, 104, 102, 103, 106])
        #expect(snapshot.parameters(of: 66561).isEmpty)
        #expect(snapshot.unownedParameters.map(\.id) == [105])
    }

    @Test func refusesAValueOfTheWrongTypeForItsDomain() {
        let bad = Self.document(masterMute: "1.5")
        #expect(throws: DecodingError.self) { try ControlSnapshot.decode(bad) }
    }

    @Test func refusesAnotherSchema() {
        let other = Data(String(decoding: Self.document(), as: UTF8.self)
            .replacingOccurrences(of: "asfw.controls.v1", with: "asfw.controls.v2").utf8)
        #expect(throws: DecodingError.self) { try ControlSnapshot.decode(other) }
    }

    @Test func refusesARouteThatIsNotAPair() {
        let bad = Data(String(decoding: Self.document(), as: UTF8.self)
            .replacingOccurrences(of: "[[6,5]]", with: "[[6,5,1]]").utf8)
        #expect(throws: DecodingError.self) { try ControlSnapshot.decode(bad) }
    }

    // MARK: Step snapping

    @Test func snapsOntoTheGridFromMin() {
        let domain = ControlScalarDomain(min: -60, max: 6, step: 0.5, unit: "dB")
        #expect(domain.snap(-10.3) == -10.5)
        #expect(domain.snap(-10.2) == -10.0)
        #expect(domain.snap(-10.25) == -10.0) // halfway rounds away from zero steps-from-min
        #expect(domain.snap(-75) == -60)
        #expect(domain.snap(9) == 6)
    }

    @Test func gridAnchorsAtMinNotZero() {
        let domain = ControlScalarDomain(min: -1, max: 1, step: 0.75, unit: "generic")
        // Grid: -1, -0.25, 0.5. 0 is not on it.
        #expect(domain.snap(0) == -0.25)
        #expect(domain.snap(0.2) == 0.5)
    }

    @Test func highestGridPointNeverPassesMax() {
        let domain = ControlScalarDomain(min: 0, max: 1, step: 0.3, unit: "generic")
        #expect(abs(domain.snap(1.0) - 0.9) < 1e-12)
        #expect(domain.snap(1.0) <= 1.0)
    }

    @Test func withoutAStepOnlyClamps() {
        let domain = ControlScalarDomain(min: 0, max: 1, step: nil, unit: "normalized")
        #expect(domain.snap(0.123) == 0.123)
        #expect(domain.snap(-1) == 0)
    }

    @Test func legalValueSnapsScalarsAndRefusesMismatches() throws {
        let snapshot = try ControlSnapshot.decode(Self.document())
        let level = try #require(snapshot.parameters.first { $0.id == 100 })
        #expect(level.legalValue(.double(-10.3)) == .double(-10.5))
        #expect(level.legalValue(.bool(true)) == nil)
        let nominal = try #require(snapshot.parameters.first { $0.id == 104 })
        #expect(nominal.legalValue(.int(4)) == .int(4))
        #expect(nominal.legalValue(.int(5)) == nil)
        let mute = try #require(snapshot.parameters.first { $0.id == 101 })
        #expect(mute.legalValue(.bool(true)) == .bool(true))
    }

    // MARK: Value wire encoding

    @Test func encodesValuesAsKindAndBits() {
        func wire(_ value: ControlValue) -> [UInt64] { [value.wire.kind, value.wire.bits] }
        #expect(wire(.bool(true)) == [0, 1])
        #expect(wire(.bool(false)) == [0, 0])
        #expect(wire(.double(-10.5)) == [1, 0xC025_0000_0000_0000])
        #expect(wire(.int(-2)) == [2, 0xFFFF_FFFF_FFFF_FFFE])
    }

    @Test func decodesKindAndBitsBack() {
        for value in [ControlValue.bool(true), .bool(false), .double(-10.5), .double(0.00390625), .int(-2), .int(4)] {
            let wire = value.wire
            #expect(ControlValue(kind: wire.kind, bits: wire.bits) == value)
        }
        #expect(ControlValue(kind: 0, bits: 7) == nil)
        #expect(ControlValue(kind: 3, bits: 0) == .action)
    }

    // MARK: Write -> poll -> confirm

    static let guid: UInt64 = 0x000A_AC03_00B1_D1F7

    @Test func showsTheConfirmedValueNotTheRequestedOne() async throws {
        let driver = FakeControlsDriver()
        driver.pendingPolls = 2
        driver.confirm = { _ in .double(-10.0) } // the device chose its own nearest step
        let model = DeviceControlsViewModel(source: driver, pollInterval: .milliseconds(1), writeTimeout: .seconds(2))
        model.reload()
        #expect(model.selectedGUID == Self.guid)
        #expect(model.snapshot?.parameters.first { $0.id == 100 }?.value == .double(-10.5))

        await model.write(100, .double(-20.3))

        // Snapped before sending; the old revision quoted.
        #expect(driver.writes.count == 1)
        #expect(driver.writes.first?.value == .double(-20.5))
        #expect(driver.writes.first?.revision == 7)
        #expect(driver.polls == 3)
        #expect(model.writeState(100) == .idle)
        #expect(model.snapshot?.parameters.first { $0.id == 100 }?.value == .double(-10.0))
        // Confirmation updates the value without a snapshot poll.
        #expect(driver.snapshotReads == 1)
    }

    @Test func decodesConfirmedPushAndRejectsMalformedEvents() {
        let scalars: [UInt64] = [22, ControlParameterChange.notificationReason, Self.guid, 7, 101, 0, 1, 0]
        #expect(ControlParameterChange(scalars: scalars) == ControlParameterChange(
            guid: Self.guid, revision: 7, sequence: 22, parameterId: 101, value: .bool(true)))
        #expect(ControlParameterChange(scalars: Array(scalars.prefix(7))) == nil)
        var invalid = scalars; invalid[5] = 99
        #expect(ControlParameterChange(scalars: invalid) == nil)
        invalid = scalars; invalid[6] = 2
        #expect(ControlParameterChange(scalars: invalid) == nil)
    }

    @Test func receivesExternalChangesWithoutPollingOrWritingBack() async {
        let driver = FakeControlsDriver()
        let model = DeviceControlsViewModel(source: driver)
        model.reload()
        let event = ControlParameterChange(guid: Self.guid, revision: 7, sequence: 20,
                                           parameterId: 101, value: .bool(true))
        // A marker on the same receive queue establishes delivery, without a sleep.
        await withCheckedContinuation { continuation in
            driver.changes.send(event)
            DispatchQueue.main.async { continuation.resume() }
        }
        #expect(model.snapshot?.parameters.first { $0.id == 101 }?.value == .bool(true))
        #expect(driver.snapshotReads == 1)
        #expect(driver.writes.isEmpty)
        model.receive(.init(guid: Self.guid, revision: 7, sequence: 19, parameterId: 101, value: .bool(false)))
        model.receive(.init(guid: Self.guid, revision: 6, sequence: 21, parameterId: 101, value: .bool(false)))
        model.receive(.init(guid: Self.guid + 1, revision: 7, sequence: 22, parameterId: 101, value: .bool(false)))
        #expect(model.snapshot?.parameters.first { $0.id == 101 }?.value == .bool(true))
        #expect(driver.snapshotReads == 1)
    }

    @Test func laterPushWinsOverAnOlderWriteResult() async {
        let driver = FakeControlsDriver()
        let model = DeviceControlsViewModel(source: driver)
        model.reload()
        driver.confirm = { [weak model] value in
            model?.receive(.init(guid: Self.guid, revision: 7, sequence: 25,
                                parameterId: 100, value: .double(-5)))
            return value
        }
        await model.write(100, .double(-20))
        #expect(model.snapshot?.parameters.first { $0.id == 100 }?.value == .double(-5))
        #expect(driver.snapshotReads == 1)
    }

    @Test func snapshotSequenceRejectsAnAlreadyIncludedPush() throws {
        let driver = FakeControlsDriver()
        driver.snapshotSequence = 30
        let model = DeviceControlsViewModel(source: driver)
        model.reload()
        model.receive(.init(guid: Self.guid, revision: 7, sequence: 29, parameterId: 100, value: .double(-40)))
        #expect(model.snapshot?.parameters.first { $0.id == 100 }?.value == .double(-10.5))
    }

    @Test func staleRevisionRereadsAndSaysSo() async {
        let driver = FakeControlsDriver()
        let model = DeviceControlsViewModel(source: driver, pollInterval: .milliseconds(1))
        model.reload()
        driver.revision = 8 // rediscovered behind the app's back

        await model.write(101, .bool(true))

        #expect(model.writeState(101) == .failed(DeviceControlsViewModel.staleRevisionMessage))
        #expect(model.message == DeviceControlsViewModel.staleRevisionMessage)
        #expect(model.snapshot?.revision == 8)
        #expect(driver.polls == 0)
    }

    @Test func refusedValueFailsWithoutPolling() async {
        let driver = FakeControlsDriver()
        driver.refuseWith = kIOReturnBadArgument
        let model = DeviceControlsViewModel(source: driver, pollInterval: .milliseconds(1))
        model.reload()

        await model.write(100, .double(-3))

        guard case .failed = model.writeState(100) else { Issue.record("expected failure"); return }
        #expect(driver.polls == 0)
        #expect(model.snapshot?.parameters.first { $0.id == 100 }?.value == .double(-10.5))
    }

    @Test func timesOutWhenTheDeviceNeverAnswers() async {
        let driver = FakeControlsDriver()
        driver.pendingPolls = .max
        let model = DeviceControlsViewModel(source: driver, pollInterval: .milliseconds(5), writeTimeout: .milliseconds(40))
        model.reload()

        await model.write(101, .bool(true))

        guard case .failed(let text) = model.writeState(101) else { Issue.record("expected failure"); return }
        #expect(text.contains("did not answer"))
        #expect(model.snapshot?.parameters.first { $0.id == 101 }?.value == .bool(false))
    }

    @Test func deviceFailureIsShownOnTheControl() async {
        let driver = FakeControlsDriver()
        driver.failWith = kIOReturnTimeout
        let model = DeviceControlsViewModel(source: driver, pollInterval: .milliseconds(1))
        model.reload()

        await model.write(101, .bool(true))

        #expect(model.writeState(101) == .failed("The device did not answer the command."))
    }

    @Test func oneWriteInFlightPerParameterLatestWins() async {
        let driver = FakeControlsDriver()
        driver.pendingPolls = 3
        let model = DeviceControlsViewModel(source: driver, pollInterval: .milliseconds(5))
        model.reload()

        let first = Task { await model.write(100, .double(-20)) }
        // Let the first write reach its poll loop, then send two more while it is in flight.
        while driver.writes.isEmpty { await Task.yield() }
        await model.write(100, .double(-30))
        await model.write(100, .double(-40))
        await first.value

        #expect(driver.writes.map(\.value) == [.double(-20), .double(-40)])
        #expect(model.writeState(100) == .idle)
        #expect(model.snapshot?.parameters.first { $0.id == 100 }?.value == .double(-40))
    }

    @Test func deviceWithoutAProjectionIsUnsupported() {
        let driver = FakeControlsDriver()
        driver.unsupported = true
        let model = DeviceControlsViewModel(source: driver)
        model.reload()
        #expect(model.content == .unsupported)
    }
}

/// A driver that serves `ControlSnapshotTests.document` and answers writes the way ControlsHandler does.
@MainActor
final class FakeControlsDriver: DeviceControlsSource {
    let changes = PassthroughSubject<ControlParameterChange, Never>()
    var controlChanges: AnyPublisher<ControlParameterChange, Never> { changes.eraseToAnyPublisher() }

    struct Write: Equatable { var revision: UInt64; var parameterId: UInt32; var value: ControlValue }

    var isConnected = true
    var revision: UInt64 = 7
    var snapshotSequence: UInt64 = 0
    var unsupported = false
    /// Refuse SetControlParameter with this IOReturn.
    var refuseWith: kern_return_t?
    /// Fail the write result with this IOReturn once the device "answers".
    var failWith: kern_return_t?
    /// kIOReturnNotReady answers before the result is ready.
    var pendingPolls = 0
    /// The value the device confirms for a request; by default the one written.
    var confirm: (ControlValue) -> ControlValue = { $0 }

    private(set) var writes: [Write] = []
    private(set) var polls = 0
    var documentOverride: Data?
    private(set) var snapshotReads = 0
    private var level = "-10.5000"
    private var mute = "false"
    private var results: [UInt64: (remaining: Int, value: ControlValue, parameterId: UInt32)] = [:]

    func getAVCUnits() -> [AVCUnitInfo]? {
        [AVCUnitInfo(guid: ControlSnapshotTests.guid, nodeID: 0xFFC1, vendorID: 0x000AAC, modelID: 3, subunits: [],
                     isoInputPlugs: 1, isoOutputPlugs: 1, extInputPlugs: 0, extOutputPlugs: 0)]
    }

    func getDiscoveredDevices() -> [FWDeviceInfo]? { [] }

    func getControlSnapshot(guid: UInt64) -> Result<Data, ControlsDriverError> {
        snapshotReads += 1
        if unsupported { return .failure(.driver(kIOReturnUnsupported)) }
        let data = documentOverride ?? ControlSnapshotTests.document(revision: revision, masterLevel: level, masterMute: mute)
        let text = String(decoding: data, as: UTF8.self).replacingOccurrences(of: "\"revision\":", with: "\"sequence\":\(snapshotSequence),\"revision\":")
        return .success(Data(text.utf8))
    }

    func setControlParameter(guid: UInt64, revision: UInt64, parameterId: UInt32,
                             value: ControlValue) -> Result<UInt64, ControlsDriverError> {
        if revision != self.revision { return .failure(.driver(kIOReturnNotPermitted)) }
        if let refuseWith { return .failure(.driver(refuseWith)) }
        writes.append(Write(revision: revision, parameterId: parameterId, value: value))
        let id = UInt64(writes.count)
        results[id] = (pendingPolls, confirm(value), parameterId)
        return .success(id)
    }

    func getControlWriteResult(requestId: UInt64) -> Result<ControlValue, ControlsDriverError> {
        polls += 1
        guard let result = results[requestId] else { return .failure(.driver(kIOReturnNotFound)) }
        if result.remaining > 0 {
            results[requestId]?.remaining -= 1
            return .failure(.driver(kIOReturnNotReady))
        }
        results[requestId] = nil
        if let failWith { return .failure(.driver(failWith)) }
        // The driver republishes the confirmed value in its graph.
        switch (result.parameterId, result.value) {
        case (100, .double(let db)): level = String(format: "%.4f", db)
        case (101, .bool(let on)): mute = on ? "true" : "false"
        default: break
        }
        return .success(result.value)
    }
}

/// Renders the Device Controls content for the sample document to a PNG so the layout can be looked at
/// (set ASFW_RENDER_DIR to keep it). No device is involved: the fake driver serves the document.
@MainActor
struct DeviceControlsRenderTests {
    @Test func rendersTheSampleDevice() async throws {
        let driver = FakeControlsDriver()
        driver.refuseWith = kIOReturnBadArgument
        let model = DeviceControlsViewModel(source: driver)
        model.reload()
        await model.write(102, .double(-3)) // leaves a failed state on one control to show it

        let directory = ProcessInfo.processInfo.environment["ASFW_RENDER_DIR"].map { URL(fileURLWithPath: $0) }
            ?? FileManager.default.temporaryDirectory.appendingPathComponent("asfw-device-controls")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        for scheme in [ColorScheme.light, .dark] {
            let view = DeviceControlsContent(model: model)
                .padding(24)
                .frame(width: 1120)
                .background(Color(nsColor: .windowBackgroundColor))
                .environment(\.colorScheme, scheme)
            let renderer = ImageRenderer(content: view)
            renderer.scale = 2
            renderer.proposedSize = .init(width: 1120, height: nil)
            let image = try #require(renderer.nsImage)
            let tiff = try #require(image.tiffRepresentation)
            let png = try #require(NSBitmapImageRep(data: tiff)?.representation(using: .png, properties: [:]))
            #expect(png.count > 5_000)
            try png.write(to: directory.appendingPathComponent("device-controls-\(scheme == .dark ? "dark" : "light").png"))
        }
    }
}

extension ControlSnapshotTests {
    // Independent wire fixture from Apple's _sendAsyncResult64 contract:
    // 24-byte Mach header, 72-byte notification header, packed 4-byte result.
    static func statusMessage(_ scalars: [UInt64]) -> Data {
        var data = Data(repeating: 0, count: 100 + scalars.count * 8)
        func put<T>(_ value: T, at offset: Int) {
            withUnsafeBytes(of: value) { data.replaceSubrange(offset..<(offset + $0.count), with: $0) }
        }
        put(UInt32(data.count), at: 4)
        put(Int32(kOSNotificationMessageID), at: 20)
        put(UInt32(4 + scalars.count * 8), at: 24)
        put(UInt32(kIOAsyncCompletionNotificationType), at: 28)
        for (i, value) in scalars.enumerated() { put(value, at: 100 + i * 8) }
        return data
    }

    @Test func decodesIOKitStatusAndControlNotificationEnvelopes() {
        let values: [[UInt64]] = [
            [11, 1],
            [22, ControlParameterChange.notificationReason, Self.guid, 7, 0, 4, 0, 0]
        ]
        for scalars in values {
            let data = Self.statusMessage(scalars)
            #expect(data.withUnsafeBytes { decodeStatusNotification($0) } == scalars)
            // Receive buffers include a trailer/capacity beyond msgh_size.
            #expect((data + Data(repeating: 0, count: 32)).withUnsafeBytes { decodeStatusNotification($0) } == scalars)
        }
        let change = Self.statusMessage(values[1]).withUnsafeBytes { decodeStatusNotification($0) }
        #expect(change.flatMap(ControlParameterChange.init(scalars:))?.stateInvalidated == true)
    }

    @Test func rejectsTruncatedOrInvalidIOKitStatusEnvelopes() {
        let valid = Self.statusMessage([11, 1])
        for length in 0..<valid.count {
            #expect(Data(valid.prefix(length)).withUnsafeBytes { decodeStatusNotification($0) } == nil)
        }
        for (offset, value) in [(0, UInt32(MACH_MSGH_BITS_COMPLEX)), (4, UInt32(4096)),
                                (20, UInt32(0)), (24, UInt32(3)), (24, UInt32(21)),
                                (24, UInt32(4096)), (28, UInt32(0)), (96, UInt32(1))] {
            var data = valid
            withUnsafeBytes(of: value) { data.replaceSubrange(offset..<(offset + 4), with: $0) }
            #expect(data.withUnsafeBytes { decodeStatusNotification($0) } == nil)
        }
        #expect(Self.statusMessage([1]).withUnsafeBytes { decodeStatusNotification($0) } == nil)
        #expect(Self.statusMessage(Array(repeating: 1, count: 17)).withUnsafeBytes { decodeStatusNotification($0) } == nil)
    }
}
