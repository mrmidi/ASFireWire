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
        // Canonical identities: AudioDeviceIds.hpp. Mailbox implementation has
        // been verified only in Pro24DSPFirmware.elf, not other DICE firmware.
        return devices.contains { $0.guid == selectedGUID && $0.vendorId == 0x00130E && $0.modelId == 0x000008 }
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
            error = "Receiver hardware capture is available only for Saffire Pro 24 DSP in Debug builds."
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
                    write: { [control] offset, payload in
                        #if DEBUG
                        let address = Self.address(offset, device: device)
                        let result: ASFWMCPTransactionResult
                        if payload.count == 4, let value = DiceWire.u32(Data(payload), 0) {
                            result = await control.executeWriteQuadlet(.init(address: address, value: value))
                        } else {
                            result = await control.executeWriteBlock(.init(address: address, payload: payload))
                        }
                        guard result.ok else { throw Self.failure("Mailbox write failed: \(result.status.rawValue).") }
                        #else
                        throw Self.failure("Hardware capture requires a Debug build.")
                        #endif
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
