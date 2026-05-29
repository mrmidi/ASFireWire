//
//  ASFWDiagnosticsClient.swift
//  ASFW
//
//  Created by ASFireWire Project on 29.05.2026.
//

import Foundation
import IOKit

/// Snapshot of all diagnostic data collected consistently.
struct ASFWDiagnosticsSnapshot {
    let busContract: ASFWDiagBusContract
    let topology: ASFWDiagTopology
    let roleCoordinator: ASFWDiagRoleCoordinator
    let ohci: ASFWDiagOHCI
    let phy: ASFWDiagPHY
    let csrContract: ASFWDiagCSRContract
    let asyncTrace: ASFWDiagAsyncTrace
    let inboundCSRStats: ASFWDiagInboundCSRStats
}

/// Client to invoke diagnostic selectors on the ASFW driver.
final class ASFWDiagnosticsClient {
    private let connector: ASFWDriverConnector
    
    init(connector: ASFWDriverConnector) {
        self.connector = connector
    }
    
    /// Fetches all diagnostics telemetry. Retries the entire collection up to 3 times
    /// if a generation mismatch is detected during collection to guarantee consistency.
    func fetchSnapshot() throws -> ASFWDiagnosticsSnapshot {
        var retries = 0
        let maxRetries = 3
        
        while true {
            do {
                return try fetchSnapshotOnce()
            } catch DiagnosticsError.staleGeneration {
                retries += 1
                if retries > maxRetries {
                    throw DiagnosticsError.staleGeneration
                }
                print("[DiagClient] 🔄 Generation mismatch detected. Retrying whole collection (attempt \(retries)/\(maxRetries))...")
                // Small backoff before retrying
                Thread.sleep(forTimeInterval: 0.05)
            }
        }
    }
    
    /// Clears the async transaction trace ring buffer on the driver.
    func clearAsyncTrace() throws {
        guard connector.isConnected else {
            throw DiagnosticsError.notConnected
        }
        
        guard let _ = connector.transport.callStruct(selector: 1008, input: nil, initialCap: 64) else {
            throw DiagnosticsError.callFailed(selector: 1008)
        }
    }
    
    // MARK: - Private Collection Methods
    
    private func fetchSnapshotOnce() throws -> ASFWDiagnosticsSnapshot {
        // Collect all diagnostic structures.
        // MemoryLayout<T>.size is used to ensure we match the driver's layout expectation.
        let busContract: ASFWDiagBusContract = try loadDiagStruct(
            selector: 1000,
            expectedSize: MemoryLayout<ASFWDiagBusContract>.size
        )
        
        let topology: ASFWDiagTopology = try loadDiagStruct(
            selector: 1001,
            expectedSize: MemoryLayout<ASFWDiagTopology>.size
        )
        
        let roleCoordinator: ASFWDiagRoleCoordinator = try loadDiagStruct(
            selector: 1002,
            expectedSize: MemoryLayout<ASFWDiagRoleCoordinator>.size
        )
        
        let ohci: ASFWDiagOHCI = try loadDiagStruct(
            selector: 1003,
            expectedSize: MemoryLayout<ASFWDiagOHCI>.size
        )
        
        let phy: ASFWDiagPHY = try loadDiagStruct(
            selector: 1004,
            expectedSize: MemoryLayout<ASFWDiagPHY>.size
        )
        
        let csrContract: ASFWDiagCSRContract = try loadDiagStruct(
            selector: 1005,
            expectedSize: MemoryLayout<ASFWDiagCSRContract>.size
        )
        
        let asyncTrace: ASFWDiagAsyncTrace = try loadDiagStruct(
            selector: 1006,
            expectedSize: MemoryLayout<ASFWDiagAsyncTrace>.size
        )
        
        let inboundCSRStats: ASFWDiagInboundCSRStats = try loadDiagStruct(
            selector: 1007,
            expectedSize: MemoryLayout<ASFWDiagInboundCSRStats>.size
        )
        
        // Verify cross-struct generation consistency.
        // We compare generation IDs across all collected structures.
        let gen = busContract.header.generation
        if topology.header.generation != gen ||
           roleCoordinator.header.generation != gen ||
           phy.header.generation != gen ||
           asyncTrace.header.generation != gen {
            print("[DiagClient] ⚠️ Cross-struct generation mismatch. BusContract: \(gen), Topology: \(topology.header.generation), RoleCoord: \(roleCoordinator.header.generation), PHY: \(phy.header.generation), Trace: \(asyncTrace.header.generation)")
            throw DiagnosticsError.staleGeneration
        }
        
        return ASFWDiagnosticsSnapshot(
            busContract: busContract,
            topology: topology,
            roleCoordinator: roleCoordinator,
            ohci: ohci,
            phy: phy,
            csrContract: csrContract,
            asyncTrace: asyncTrace,
            inboundCSRStats: inboundCSRStats
        )
    }
    
    private func loadDiagStruct<T>(selector: UInt32, expectedSize: Int) throws -> T {
        guard connector.isConnected else {
            throw DiagnosticsError.notConnected
        }
        
        // Call struct method with expected size as capacity.
        guard let data = connector.transport.callStruct(selector: selector, input: nil, initialCap: expectedSize) else {
            throw DiagnosticsError.callFailed(selector: selector)
        }
        
        guard data.count >= MemoryLayout<ASFWDiagHeader>.size else {
            throw DiagnosticsError.invalidDataSize(selector: selector, received: data.count, expectedAtLeast: MemoryLayout<ASFWDiagHeader>.size)
        }
        
        // Read header to validate
        let header = data.withUnsafeBytes { ptr in
            ptr.load(as: ASFWDiagHeader.self)
        }
        
        guard header.abiVersion == ASFW_DIAG_ABI_VERSION else {
            throw DiagnosticsError.abiVersionMismatch(selector: selector, received: header.abiVersion, expected: ASFW_DIAG_ABI_VERSION)
        }
        
        guard header.structSize == expectedSize else {
            throw DiagnosticsError.structSizeMismatch(selector: selector, received: Int(header.structSize), expected: expectedSize)
        }
        
        guard data.count >= expectedSize else {
            throw DiagnosticsError.invalidDataSize(selector: selector, received: data.count, expectedAtLeast: expectedSize)
        }
        
        // Check internal status code
        if header.status == 2 { // ASFWDiagStatusStaleGeneration
            throw DiagnosticsError.staleGeneration
        } else if header.status != 0 { // ASFWDiagStatusStatusOK
            throw DiagnosticsError.driverError(status: header.status)
        }
        
        // Copy to aligned storage and load as T
        return data.withUnsafeBytes { ptr in
            ptr.load(as: T.self)
        }
    }
}

// MARK: - Diagnostics DiagnosticsError Definition

enum DiagnosticsError: Error, LocalizedError {
    case notConnected
    case callFailed(selector: UInt32)
    case abiVersionMismatch(selector: UInt32, received: UInt32, expected: UInt32)
    case structSizeMismatch(selector: UInt32, received: Int, expected: Int)
    case invalidDataSize(selector: UInt32, received: Int, expectedAtLeast: Int)
    case staleGeneration
    case driverError(status: UInt32)
    
    var errorDescription: String? {
        switch self {
        case .notConnected:
            return "Not connected to ASFW driver."
        case .callFailed(let selector):
            return "Driver call failed for selector \(selector)."
        case .abiVersionMismatch(let selector, let received, let expected):
            return "ABI version mismatch on selector \(selector): driver returned \(received), app expected \(expected)."
        case .structSizeMismatch(let selector, let received, let expected):
            return "Struct size mismatch on selector \(selector): driver returned \(received) bytes, app expected \(expected) bytes."
        case .invalidDataSize(let selector, let received, let expected):
            return "Data buffer too small on selector \(selector): received \(received) bytes, expected at least \(expected) bytes."
        case .staleGeneration:
            return "Stale topology generation. The bus topology updated during retrieval."
        case .driverError(let status):
            let statusStr: String
            switch status {
            case 1: statusStr = "Unavailable"
            case 2: statusStr = "Stale Generation"
            case 3: statusStr = "Buffer Too Small"
            case 4: statusStr = "Unsupported"
            case 5: statusStr = "Busy"
            case 6: statusStr = "Failed"
            default: statusStr = "Unknown (\(status))"
            }
            return "Driver returned error status: \(statusStr)."
        }
    }
}
