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
    func subunitCapabilitiesBlob(guid: UInt64, type: UInt8, id: UInt8) -> Result<Data, AvcBlobUnavailable>
    func subunitDescriptorBlob(guid: UInt64, type: UInt8, id: UInt8) -> Result<Data, AvcBlobUnavailable>
    func getFCPExchangeLog(guid: UInt64) -> AvcReportSnapshot.ExchangeLog?
    /// The unit's discovery document bytes, or nil when the driver has none
    /// or its pages could not be assembled consistently.
    func getAVCDiscoveryDocument(guid: UInt64) -> Data?
}

/// Why the driver returned no bytes for a report blob, in words for the report.
struct AvcBlobUnavailable: Error, Sendable, Equatable {
    var reason: String
}

extension ASFWDriverConnector: AvcReportSource {}
