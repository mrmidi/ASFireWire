import Foundation
import IOKit

/// Why a controls call failed. `.driver` keeps the IOReturn so the caller can
/// tell "not ready yet" and "stale revision" from a real failure.
enum ControlsDriverError: Error, Sendable, Equatable {
    case notConnected
    case driver(kern_return_t)
    /// The snapshot's pages never agreed: the device kept changing while it was read.
    case inconsistentPages
    /// A page or the reply did not have the expected shape.
    case malformed(String)

    var code: kern_return_t? { if case .driver(let code) = self { code } else { nil } }
    var isUnsupported: Bool { code == kIOReturnUnsupported }
    var isNotReady: Bool { code == kIOReturnNotReady }
    var isStaleRevision: Bool { code == kIOReturnNotPermitted }
}

extension ASFWDriverConnector {
    // MARK: - Device controls (selectors 80-82)

    /// The device's "asfw.controls.v1" document, read page by page like the
    /// AV/C discovery document. Pages that disagree (the device was
    /// rediscovered or a write landed between reads) restart the read, at most
    /// three times. `kIOReturnUnsupported` means the device has no control
    /// projection.
    func getControlSnapshot(guid: UInt64) -> Result<Data, ControlsDriverError> {
        guard isConnected, connection != 0 else { return .failure(.notConnected) }
        var lastCode: kern_return_t = KERN_SUCCESS
        let fetch: (UInt32) -> Data? = { [self] offset in
            let scalarInputs: [UInt64] = [guid >> 32, guid & 0xFFFF_FFFF, UInt64(offset)]
            var outSize = 4 * 1024
            var out = Data(count: outSize)
            let kr = out.withUnsafeMutableBytes { outPtr in
                scalarInputs.withUnsafeBufferPointer { scalarPtr in
                    IOConnectCallMethod(connection, Method.getControlSnapshot.rawValue,
                                        scalarPtr.baseAddress, 3, nil, 0, nil, nil,
                                        outPtr.baseAddress?.assumingMemoryBound(to: UInt8.self), &outSize)
                }
            }
            lastCode = kr
            guard kr == KERN_SUCCESS else { return nil }
            out.count = outSize
            return out
        }
        for _ in 0..<3 {
            switch AvcDiscoveryDocument.assemble(fetch: fetch) {
            case .success(let document): return .success(document)
            case .failure(.mixedPages), .failure(.checksumMismatch): continue
            case .failure(.unavailable):
                return .failure(.driver(lastCode))
            case .failure(let error):
                print("[Connector] ❌ getControlSnapshot failed: \(error)")
                return .failure(.malformed("snapshot pages: \(error)"))
            }
        }
        return .failure(.inconsistentPages)
    }

    /// Ask the driver to write `value` to parameter `parameterId`. Returns the
    /// request id at once; the device has not answered yet. Refusals (stale
    /// revision, unknown id, wrong kind, off the domain) fail here, before
    /// anything is sent to the device.
    func setControlParameter(guid: UInt64, revision: UInt64, parameterId: UInt32,
                             value: ControlValue) -> Result<UInt64, ControlsDriverError> {
        guard isConnected, connection != 0 else { return .failure(.notConnected) }
        let wire = value.wire
        let scalarInputs: [UInt64] = [guid >> 32, guid & 0xFFFF_FFFF, revision, UInt64(parameterId),
                                      wire.kind, wire.bits]
        var requestId: UInt64 = 0
        var outputCount: UInt32 = 1
        let kr = scalarInputs.withUnsafeBufferPointer { scalarPtr in
            IOConnectCallScalarMethod(connection, Method.setControlParameter.rawValue,
                                      scalarPtr.baseAddress, UInt32(scalarInputs.count),
                                      &requestId, &outputCount)
        }
        guard kr == KERN_SUCCESS else { return .failure(.driver(kr)) }
        guard outputCount >= 1 else { return .failure(.malformed("no request id")) }
        return .success(requestId)
    }

    /// The hardware-confirmed value of write `requestId`. `kIOReturnNotReady`
    /// while the device has not answered. A result is handed out once; asking
    /// again gives `kIOReturnNotFound`.
    func getControlWriteResult(requestId: UInt64) -> Result<ControlValue, ControlsDriverError> {
        guard isConnected, connection != 0 else { return .failure(.notConnected) }
        var input = requestId
        var output: [UInt64] = [0, 0]
        var outputCount: UInt32 = 2
        let kr = output.withUnsafeMutableBufferPointer { outPtr in
            IOConnectCallScalarMethod(connection, Method.getControlWriteResult.rawValue,
                                      &input, 1, outPtr.baseAddress, &outputCount)
        }
        guard kr == KERN_SUCCESS else { return .failure(.driver(kr)) }
        guard outputCount >= 2, let value = ControlValue(kind: output[0], bits: output[1]) else {
            return .failure(.malformed("result kind \(output[0]) with \(outputCount) outputs"))
        }
        return .success(value)
    }
}
