import Combine
import Foundation

@MainActor
final class DiceDiagnosticsStore: ObservableObject {
    @Published private(set) var devices: [ASFWDriverConnector.FWDeviceInfo] = []
    @Published var selectedGUID: UInt64?
    @Published private(set) var snapshot: DiceDiagnosticsSnapshot?
    @Published private(set) var baseline: DiceDiagnosticsSnapshot?
    @Published private(set) var isCapturing = false
    @Published private(set) var error: String?
    private let connector: ASFWDriverConnector
    private let control: LiveASFWDriverControl
    private var captureTask: Task<Void, Never>?

    init(connector: ASFWDriverConnector) {
        self.connector = connector
        control = LiveASFWDriverControl(backend: connector)
    }

    deinit { captureTask?.cancel() }

    func refreshDevices() {
        let discovered = connector.getDiscoveredDevices() ?? []
        devices = BusInspectorNode.inventory(topology: connector.getTopologySnapshot(), devices: discovered)
            .filter(\.isDICEDevice).compactMap(\.device)
        if !devices.contains(where: { $0.guid == selectedGUID }) { selectedGUID = devices.first?.guid }
    }

    var canCaptureHardware: Bool {
        #if DEBUG
        // The driver decides whether this device may be read (verified PEEK
        // firmware, TCD22xx ASIC, loader present) and reports why not.
        return devices.contains { $0.guid == selectedGUID }
        #else
        return false
        #endif
    }

    func pinBaseline() { baseline = snapshot }
    func clearBaseline() { baseline = nil }
    func cancel() { captureTask?.cancel() }

    func capture(hardware: Bool = false, fifoEvents: Bool = false) {
        guard !isCapturing else { return }
        refreshDevices()
        guard let device = devices.first(where: { $0.guid == selectedGUID }) else {
            error = "No connected DICE device selected."
            return
        }
        guard !hardware || canCaptureHardware else {
            error = "Receiver hardware capture is available only in Debug builds."
            return
        }
        isCapturing = true
        error = nil
        captureTask = Task { [weak self] in
            guard let self else { return }
            defer { isCapturing = false }
            do {
                let reader = DiceDiagnosticsReader(
                    read: { [control] offset, length in
                        let address = Self.address(offset, device: device)
                        let result = length == 4
                            ? await control.executeReadQuadlet(.init(address: address))
                            : await control.executeReadBlock(.init(address: address, length: length))
                        guard result.ok, let payload = result.payload else {
                            throw Self.failure("Read failed: \(result.status.rawValue) at \(String(offset, radix: 16)).")
                        }
                        return Data(payload)
                    },
                    hardwareDump: { [connector] fifoEvents in
                        guard connector.requestDiceReceiverDump(guid: device.guid, fifoEvents: fifoEvents) else {
                            throw Self.failure("The driver did not start a receiver dump (busy, no protocol, or disconnected).")
                        }
                        let deadline = ContinuousClock.now.advanced(by: .seconds(15))
                        while true {
                            try Task.checkCancellation()
                            if let dump = connector.getDiceReceiverDump(guid: device.guid), !dump.isRunning {
                                guard dump.isComplete else { throw Self.failure(dump.outcome) }
                                return dump.values
                            }
                            guard ContinuousClock.now < deadline else {
                                throw Self.failure("The driver's receiver dump did not finish within 15 s.")
                            }
                            try await Task.sleep(for: .milliseconds(50))
                        }
                    },
                    validate: { [connector] in
                        guard connector.isConnected,
                              connector.getControllerStatus()?.generation == device.generation,
                              connector.getDiscoveredDevices()?.contains(where: {
                                  $0.guid == device.guid && $0.nodeId == device.nodeId && $0.generation == device.generation
                              }) == true else {
                            throw Self.failure("Device disconnected or bus generation changed. Capture discarded.")
                        }
                    })
                let values = try await reader.capture(hardware: hardware, fifoEvents: fifoEvents)
                try Task.checkCancellation()
                snapshot = DiceDiagnosticsSnapshot(guid: device.guid, generation: device.generation,
                    deviceName: "\(device.vendorName) \(device.modelName)", capturedAt: Date(),
                    includesHardware: hardware, clearedFIFOEvents: hardware && fifoEvents, values: values)
            } catch is CancellationError {
                error = "Capture cancelled; previous snapshot retained."
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    private static func address(_ offset: UInt64, device: ASFWDriverConnector.FWDeviceInfo) -> ASFWMCPAddress {
        .init(nodeId: UInt32(device.nodeId), generation: device.generation,
              addressHigh: UInt16(truncatingIfNeeded: offset >> 32), addressLow: UInt32(truncatingIfNeeded: offset))
    }

    private static func failure(_ message: String) -> NSError {
        NSError(domain: "DiceDiagnostics", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
    }
}
