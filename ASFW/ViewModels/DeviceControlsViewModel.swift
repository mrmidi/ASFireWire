import Combine
import Foundation
import IOKit

/// What the Device Controls screen needs from the driver connection.
@MainActor
protocol DeviceControlsSource: AnyObject {
    var isConnected: Bool { get }
    var controlChanges: AnyPublisher<ControlParameterChange, Never> { get }
    func getAVCUnits() -> [AVCUnitInfo]?
    func getDiscoveredDevices() -> [FWDeviceInfo]?
    func getControlSnapshot(guid: UInt64) -> Result<Data, ControlsDriverError>
    func setControlParameter(guid: UInt64, revision: UInt64, parameterId: UInt32,
                             value: ControlValue) -> Result<UInt64, ControlsDriverError>
    func getControlWriteResult(requestId: UInt64) -> Result<ControlValue, ControlsDriverError>
}

extension ASFWDriverConnector: DeviceControlsSource {}

/// The Device Controls screen: one device's control snapshot, and writes that
/// show only what the hardware confirmed.
///
/// Nothing polls while idle. The snapshot is read on appear, on Refresh, on a
/// device change. Confirmed changes arrive by push, without a snapshot read.
/// A write polls only its own result, every
/// `pollInterval`, until the device answers or `writeTimeout` passes.
@MainActor
final class DeviceControlsViewModel: ObservableObject {
    struct Device: Identifiable, Equatable, Sendable {
        let guid: UInt64
        let title: String
        var id: UInt64 { guid }
        var guidHex: String { String(format: "0x%016llX", guid) }
    }

    enum Content: Equatable {
        case idle
        case notConnected
        case loaded(ControlSnapshot)
        /// The device has no control projection (today only AV/C devices have one).
        case unsupported
        case failed(String)
    }

    enum WriteState: Equatable {
        case idle
        /// The value sent; the screen keeps showing the confirmed value until the device answers.
        case writing(ControlValue)
        case failed(String)
    }

    static let staleRevisionMessage = "The device was rediscovered; try again."

    @Published private(set) var devices: [Device] = []
    @Published private(set) var selectedGUID: UInt64?
    @Published private(set) var content: Content = .idle
    @Published private(set) var writeStates: [UInt32: WriteState] = [:]
    @Published private(set) var message: String?
    @Published private(set) var lastUpdated: Date?

    private let source: any DeviceControlsSource
    private let pollInterval: Duration
    private let writeTimeout: Duration
    private var changeSubscription: AnyCancellable?
    private var lastChangeSequence: [UInt32: UInt64] = [:]
    /// The latest value asked for while a write to the same parameter was in
    /// flight; sent when that write finishes. One write per parameter at a time.
    private var queued: [UInt32: ControlValue] = [:]

    init(source: any DeviceControlsSource, pollInterval: Duration = .milliseconds(50),
         writeTimeout: Duration = .seconds(3)) {
        self.source = source
        self.pollInterval = pollInterval
        self.writeTimeout = writeTimeout
        changeSubscription = source.controlChanges.receive(on: DispatchQueue.main).sink { [weak self] change in
            // receive(on:) is the GCD/Combine boundary; the delivery is on main.
            MainActor.assumeIsolated { self?.receive(change) }
        }
    }

    var isConnected: Bool { source.isConnected }

    var snapshot: ControlSnapshot? {
        if case .loaded(let snapshot) = content { snapshot } else { nil }
    }

    var selectedDevice: Device? { devices.first { $0.guid == selectedGUID } }

    func writeState(_ parameterId: UInt32) -> WriteState { writeStates[parameterId] ?? .idle }

    // MARK: Loading

    /// Re-read the device list (the AV/C units the driver knows) and the selected device's snapshot.
    func reload() {
        guard source.isConnected else {
            devices = []
            writeStates = [:]
            lastChangeSequence = [:]
            queued = [:]
            content = .notConnected
            return
        }
        let devicesByGUID = Dictionary((source.getDiscoveredDevices() ?? []).map { ($0.guid, $0) },
                                       uniquingKeysWith: { first, _ in first })
        devices = (source.getAVCUnits() ?? []).map { unit in
            let model = devicesByGUID[unit.guid]?.modelName ?? ""
            return Device(guid: unit.guid, title: model.isEmpty ? "AV/C unit \(unit.guidHex)" : model)
        }
        if selectedGUID == nil || !devices.contains(where: { $0.guid == selectedGUID }) {
            select(devices.first?.guid)
        } else {
            refreshSnapshot()
        }
    }

    func select(_ guid: UInt64?) {
        if guid != selectedGUID {
            writeStates = [:]
            lastChangeSequence = [:]
            queued = [:]
            message = nil
        }
        selectedGUID = guid
        refreshSnapshot()
    }

    /// Read the selected device's snapshot. Write states of parameters that no
    /// longer exist are dropped; a failure stays on its control until the next write.
    func refreshSnapshot() {
        guard source.isConnected else {
            writeStates = [:]
            lastChangeSequence = [:]
            queued = [:]
            content = .notConnected
            return
        }
        guard let guid = selectedGUID else {
            content = .idle
            return
        }
        switch source.getControlSnapshot(guid: guid) {
        case .success(let data):
            do {
                let snapshot = try ControlSnapshot.decode(data)
                if self.snapshot?.revision != snapshot.revision {
                    lastChangeSequence = [:]
                    // Keep actionable refusals visible through the refresh;
                    // only writes belonging to the old revision are withdrawn.
                    writeStates = writeStates.filter {
                        if case .failed = $0.value { true } else { false }
                    }
                    queued = [:]
                }
                content = .loaded(snapshot)
                let ids = Set(snapshot.parameters.map(\.id))
                writeStates = writeStates.filter { ids.contains($0.key) }
                lastUpdated = Date()
            } catch {
                content = .failed("The driver's control document could not be read: \(error.localizedDescription)")
            }
        case .failure(let error) where error.isUnsupported:
            content = .unsupported
        case .failure(let error):
            content = .failed("Could not read the device's controls: \(Self.describe(error)).")
        }
    }

    // MARK: Writing

    /// Write `requested` to a parameter of the selected device. A scalar is
    /// moved onto its step grid first (the driver refuses off-grid values).
    /// When a write to the same parameter is already in flight, this value
    /// replaces any queued one and is sent when that write ends.
    func write(_ parameterId: UInt32, _ requested: ControlValue) async {
        guard let guid = selectedGUID,
              let parameter = snapshot?.parameters.first(where: { $0.id == parameterId }) else { return }
        guard let value = parameter.legalValue(requested) else {
            writeStates[parameterId] = .failed("That value does not fit this control.")
            return
        }
        if case .writing = writeState(parameterId) {
            queued[parameterId] = value
            return
        }
        var next: ControlValue? = value
        while let value = next, selectedGUID == guid {
            await perform(guid: guid, parameterId: parameterId, value: value)
            next = queued.removeValue(forKey: parameterId)
        }
    }

    private func perform(guid: UInt64, parameterId: UInt32, value: ControlValue) async {
        guard let revision = snapshot?.revision else { return }
        let sequenceBeforeWrite = lastChangeSequence[parameterId]
        writeStates[parameterId] = .writing(value)
        let requestId: UInt64
        switch source.setControlParameter(guid: guid, revision: revision, parameterId: parameterId, value: value) {
        case .success(let id):
            requestId = id
        case .failure(let error):
            queued[parameterId] = nil
            writeStates[parameterId] = .failed(Self.refusalText(error))
            if error.isStaleRevision { message = Self.staleRevisionMessage }
            // Anything but a refused value means the snapshot is out of date.
            if error.code != kIOReturnBadArgument { refreshSnapshot() }
            return
        }

        let result = await awaitConfirmation(requestId)
        guard selectedGUID == guid, snapshot?.revision == revision else { return }
        switch result {
        case .success(let confirmed):
            // A later push may already supersede this write result.
            if lastChangeSequence[parameterId] == sequenceBeforeWrite {
                applyConfirmed(parameterId, confirmed)
            }
            writeStates[parameterId] = .idle
            message = nil
        case .failure(let failure):
            queued[parameterId] = nil
            writeStates[parameterId] = .failed(failure.text)
        }
    }

    /// Poll the write's result until the device answers or the timeout passes.
    private func awaitConfirmation(_ requestId: UInt64) async -> Result<ControlValue, WriteFailure> {
        let deadline = ContinuousClock.now.advanced(by: writeTimeout)
        while true {
            switch source.getControlWriteResult(requestId: requestId) {
            case .success(let confirmed):
                return .success(confirmed)
            case .failure(let error) where error.isNotReady:
                break
            case .failure(let error):
                return .failure(WriteFailure(Self.deviceFailureText(error)))
            }
            guard ContinuousClock.now < deadline else {
                return .failure(WriteFailure("The device did not answer within \(writeTimeout.formatted(.units(allowed: [.seconds, .milliseconds], width: .abbreviated)))."))
            }
            do { try await Task.sleep(for: pollInterval) } catch {
                return .failure(WriteFailure("The write was cancelled before the device answered."))
            }
        }
    }

    /// Show only a hardware-confirmed value.
    private func applyConfirmed(_ parameterId: UInt32, _ confirmed: ControlValue, origin: ControlOrigin = .observed) {
        guard var snapshot,
              let index = snapshot.parameters.firstIndex(where: { $0.id == parameterId }) else { return }
        snapshot.parameters[index].value = confirmed
        snapshot.parameters[index].origin = origin
        content = .loaded(snapshot)
    }

    /// Changes from CoreAudio, another app write or a device readback all
    /// arrive here. Never issue a write in response to a notification.
    func receive(_ change: ControlParameterChange) {
        guard source.isConnected, change.guid == selectedGUID, let snapshot else { return }
        if change.revision > snapshot.revision {
            // Topology changed, rather than just a value. Fetch it once.
            refreshSnapshot()
            guard self.snapshot?.revision == change.revision else { return }
        } else if change.revision != snapshot.revision { return }
        guard change.sequence > max(lastChangeSequence[change.parameterId] ?? 0, self.snapshot?.sequence ?? 0),
              let parameter = self.snapshot?.parameters.first(where: { $0.id == change.parameterId }),
              parameter.legalValue(change.value) == change.value else { return }
        lastChangeSequence[change.parameterId] = change.sequence
        applyConfirmed(change.parameterId, change.value, origin: change.origin)
        lastUpdated = Date()
    }

    struct WriteFailure: Error, Equatable {
        let text: String
        init(_ text: String) { self.text = text }
    }

    // MARK: Words

    /// Why the driver refused a write before sending anything to the device.
    static func refusalText(_ error: ControlsDriverError) -> String {
        switch error.code {
        case kIOReturnNotPermitted: return staleRevisionMessage
        case kIOReturnNotFound: return "This control no longer exists on the device."
        case kIOReturnBadArgument: return "The driver refused the value (out of range or off the step grid)."
        case kIOReturnUnsupported: return "This device has no controls the driver can write."
        case kIOReturnNotReady: return "The device is not reachable right now (bus reset or rediscovery)."
        default: return "The write was not sent: \(describe(error))."
        }
    }

    /// Why a sent write did not end with a confirmed value.
    static func deviceFailureText(_ error: ControlsDriverError) -> String {
        switch error.code {
        case kIOReturnNotFound: return "The driver no longer has this write's result."
        case kIOReturnUnsupported: return "The device rejected the command (not implemented)."
        case kIOReturnTimeout: return "The device did not answer the command."
        default: return "The device did not confirm the write: \(describe(error))."
        }
    }

    static func describe(_ error: ControlsDriverError) -> String {
        switch error {
        case .notConnected: return "not connected to the driver"
        case .inconsistentPages: return "the device kept changing while it was read"
        case .malformed(let what): return "unexpected reply (\(what))"
        case .driver(let code):
            if let text = mach_error_string(code) { return "\(String(cString: text)) (0x\(String(UInt32(bitPattern: code), radix: 16)))" }
            return "IOReturn 0x\(String(UInt32(bitPattern: code), radix: 16))"
        }
    }
}
