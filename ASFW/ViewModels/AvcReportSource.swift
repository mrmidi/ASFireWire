import Foundation

/// Manual discovery and exports of the resulting driver-owned parsed data.
@MainActor
protocol AvcReportSource {
    var isConnected: Bool { get }
    func reScanAVCUnits() -> Bool
    func getDiscoveredDevices() -> [FWDeviceInfo]?
    func getAVCUnits() -> [AVCUnitInfo]?
    func getDriverVersion() -> DriverVersionInfo?
    func getConfigROM(nodeId: UInt8, generation: UInt16) -> ASFWDriverConnector.ConfigROMFetchResult?
    func getSubunitCapabilitiesData(guid: UInt64, type: UInt8, id: UInt8) -> Data?
    func getSubunitDescriptor(guid: UInt64, type: UInt8, id: UInt8) -> Data?
}

extension ASFWDriverConnector: AvcReportSource {}
