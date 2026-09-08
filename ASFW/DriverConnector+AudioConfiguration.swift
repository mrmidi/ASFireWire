import Foundation
import IOKit

extension ASFWDriverConnector {
    func requestAudioConfigurationSnapshotAsync(
        completion: @escaping (AudioConfigurationSnapshot?, kern_return_t) -> Void
    ) {
        connectionQueue.async { [weak self] in
            guard let self, self.connection != 0,
                  self.asyncPort != mach_port_t(MACH_PORT_NULL) else {
                DispatchQueue.main.async { completion(nil, kIOReturnNotReady) }
                return
            }
            let requestID = self.nextAudioControlRequestID
            self.nextAudioControlRequestID &+= 1
            self.audioConfigurationSnapshotCompletions[requestID] = { status, values in
                let snapshot = status == KERN_SUCCESS
                    ? AudioAsyncSnapshotDecoder.configuration(values) : nil
                completion(snapshot, status)
            }
            var reference = DriverKitAsyncCompletionDecoder.reference(
                marker: Self.audioConfigurationSnapshotAsyncReference
            )
            var inputs = [requestID]
            let result = IOConnectCallAsyncScalarMethod(
                self.connection, Method.getAudioConfigurationAsync.rawValue,
                self.asyncPort, &reference, UInt32(reference.count),
                &inputs, UInt32(inputs.count), nil, nil)
            if result != KERN_SUCCESS {
                let callback = self.audioConfigurationSnapshotCompletions.removeValue(forKey: requestID)
                DispatchQueue.main.async { callback?(result, []) }
            }
        }
    }

    func requestAudioControlSurfaceSnapshotAsync(
        endpointID: AudioEndpointID,
        completion: @escaping (AudioControlSurfaceSnapshot?) -> Void
    ) {
        connectionQueue.async { [weak self] in
            guard let self, self.connection != 0,
                  self.asyncPort != mach_port_t(MACH_PORT_NULL), endpointID.rawValue != 0 else {
                DispatchQueue.main.async { completion(nil) }
                return
            }
            let requestID = self.nextAudioControlRequestID
            self.nextAudioControlRequestID &+= 1
            self.audioControlSnapshotCompletions[requestID] = { [weak self] status, values in
                // The async reply carries only a header: an async completion has
                // 16 scalar slots and the 1814's surface is 78 values. Treat it
                // as "the surface is ready at revision N" and read the whole
                // thing over the struct selector, still off the main queue.
                guard status == KERN_SUCCESS,
                      AudioAsyncSnapshotDecoder.controlsHeader(values) != nil,
                      let self else {
                    completion(nil)
                    return
                }
                self.connectionQueue.async {
                    let snapshot = self.getAudioControlSurface(endpointID: endpointID)
                    DispatchQueue.main.async { completion(snapshot) }
                }
            }
            var reference = DriverKitAsyncCompletionDecoder.reference(
                marker: Self.audioControlSnapshotAsyncReference
            )
            var inputs = [endpointID.rawValue, requestID]
            let result = IOConnectCallAsyncScalarMethod(
                self.connection, Method.getAudioControlSurfaceAsync.rawValue,
                self.asyncPort, &reference, UInt32(reference.count),
                &inputs, UInt32(inputs.count), nil, nil)
            if result != KERN_SUCCESS {
                let callback = self.audioControlSnapshotCompletions.removeValue(forKey: requestID)
                DispatchQueue.main.async { callback?(result, []) }
            }
        }
    }

    func requestAudioMeterSnapshotAsync(
        endpointID: AudioEndpointID,
        completion: @escaping (AudioMeterSnapshot?) -> Void
    ) {
        connectionQueue.async { [weak self] in
            guard let self, self.connection != 0,
                  self.asyncPort != mach_port_t(MACH_PORT_NULL), endpointID.rawValue != 0 else {
                DispatchQueue.main.async { completion(nil) }
                return
            }
            let requestID = self.nextAudioControlRequestID
            self.nextAudioControlRequestID &+= 1
            self.audioMeterSnapshotCompletions[requestID] = { status, values in
                completion(status == KERN_SUCCESS ? AudioAsyncSnapshotDecoder.meters(values) : nil)
            }
            var reference = DriverKitAsyncCompletionDecoder.reference(
                marker: Self.audioMeterSnapshotAsyncReference
            )
            var inputs = [endpointID.rawValue, requestID]
            let result = IOConnectCallAsyncScalarMethod(
                self.connection, Method.getAudioMeterSnapshotAsync.rawValue,
                self.asyncPort, &reference, UInt32(reference.count),
                &inputs, UInt32(inputs.count), nil, nil)
            if result != KERN_SUCCESS {
                let callback = self.audioMeterSnapshotCompletions.removeValue(forKey: requestID)
                DispatchQueue.main.async { callback?(result, []) }
            }
        }
    }

    func getAudioConfigurationEndpointIDs() -> [AudioEndpointID] {
        guard isConnected,
              let data = callStruct(.getAudioConfigurationEndpoints, initialCap: 72) else {
            return []
        }
        return AudioConfigurationWireDecoder.decodeEndpointIDs(data)
    }

    /// Every endpoint with runtime tuning and geometry to report.
    ///
    /// Distinct from `getAudioConfigurationEndpointIDs`, which additionally
    /// requires the device to advertise rate/optical capabilities. A device with
    /// none still has geometry, and gating discovery on the narrower list made
    /// the tuning panel claim no endpoint was published while its own telemetry
    /// streamed beside it. Same reply shape, so the same decoder serves both.
    func getAudioRuntimeTuningEndpointIDs() -> [AudioEndpointID] {
        guard isConnected,
              let data = callStruct(.getAudioRuntimeTuningEndpoints, initialCap: 72) else {
            return []
        }
        return AudioConfigurationWireDecoder.decodeEndpointIDs(data)
    }

    func getAudioConfiguration(endpointID: AudioEndpointID) -> AudioConfigurationSnapshot? {
        guard isConnected, connection != 0, endpointID.rawValue != 0 else { return nil }
        var scalarInput = endpointID.rawValue
        var output = Data(count: 168)
        var outputLength = output.count
        let result = output.withUnsafeMutableBytes { outputBytes in
            IOConnectCallMethod(
                connection,
                Method.getAudioConfiguration.rawValue,
                &scalarInput,
                1,
                nil,
                0,
                nil,
                nil,
                outputBytes.baseAddress,
                &outputLength
            )
        }
        guard result == KERN_SUCCESS, outputLength == output.count else { return nil }
        return AudioConfigurationWireDecoder.decode(output)
    }

    func requestAudioConfiguration(
        endpointID: AudioEndpointID,
        sampleRateHz: UInt32,
        inputOptical: AudioOpticalMode,
        outputOptical: AudioOpticalMode
    ) -> kern_return_t {
        guard isConnected, connection != 0, endpointID.rawValue != 0 else {
            return kIOReturnNotReady
        }
        var scalarInputs = [
            endpointID.rawValue,
            UInt64(sampleRateHz),
            UInt64(inputOptical.rawValue),
            UInt64(outputOptical.rawValue),
        ]
        return IOConnectCallScalarMethod(
            connection,
            Method.requestAudioConfiguration.rawValue,
            &scalarInputs,
            UInt32(scalarInputs.count),
            nil,
            nil
        )
    }

    /// Opens the AudioDriverKit configuration window through an async action.
    /// The completion means only that the request was accepted; the actual
    /// Core Audio transaction is observed through later snapshots.
    func requestAudioConfigurationAsync(
        endpointID: AudioEndpointID,
        sampleRateHz: UInt32,
        inputOptical: AudioOpticalMode,
        outputOptical: AudioOpticalMode,
        completion: @escaping (kern_return_t) -> Void
    ) {
        connectionQueue.async { [weak self] in
            guard let self, self.connection != 0,
                  self.asyncPort != mach_port_t(MACH_PORT_NULL), endpointID.rawValue != 0 else {
                DispatchQueue.main.async { completion(kIOReturnNotReady) }
                return
            }
            let requestID = self.nextAudioControlRequestID
            self.nextAudioControlRequestID &+= 1
            self.audioConfigurationCompletions[requestID] = completion
            var reference = DriverKitAsyncCompletionDecoder.reference(
                marker: Self.audioConfigurationAsyncReference
            )
            var inputs = [endpointID.rawValue, UInt64(sampleRateHz),
                          UInt64(inputOptical.rawValue), UInt64(outputOptical.rawValue), requestID]
            let result = IOConnectCallAsyncScalarMethod(
                self.connection, Method.requestAudioConfigurationAsync.rawValue,
                self.asyncPort, &reference, UInt32(reference.count),
                &inputs, UInt32(inputs.count), nil, nil)
            if result != KERN_SUCCESS {
                let callback = self.audioConfigurationCompletions.removeValue(forKey: requestID)
                DispatchQueue.main.async { callback?(result) }
            }
        }
    }

    func getAudioControlSurface(endpointID: AudioEndpointID) -> AudioControlSurfaceSnapshot? {
        guard isConnected, connection != 0, endpointID.rawValue != 0 else { return nil }
        var scalarInput = endpointID.rawValue
        var output = Data(count: 672)
        var outputLength = output.count
        let result = output.withUnsafeMutableBytes { outputBytes in
            IOConnectCallMethod(
                connection,
                Method.getAudioControlSurface.rawValue,
                &scalarInput,
                1,
                nil,
                0,
                nil,
                nil,
                outputBytes.baseAddress,
                &outputLength
            )
        }
        guard result == KERN_SUCCESS, outputLength == output.count else { return nil }
        return AudioControlSurfaceWireDecoder.decode(output)
    }

    func getAudioSemanticTopology(endpointID: AudioEndpointID) -> AudioSemanticTopologySnapshot? {
        guard isConnected, connection != 0, endpointID.rawValue != 0 else { return nil }
        var scalarInput = endpointID.rawValue
        var output = Data(count: 3944)
        var outputLength = output.count
        let result = output.withUnsafeMutableBytes { outputBytes in
            IOConnectCallMethod(
                connection,
                Method.getAudioSemanticTopology.rawValue,
                &scalarInput,
                1,
                nil,
                0,
                nil,
                nil,
                outputBytes.baseAddress,
                &outputLength
            )
        }
        guard result == KERN_SUCCESS, outputLength == output.count else { return nil }
        return AudioSemanticTopologyWireDecoder.decode(output)
    }

    func getAudioSemanticTopologyEndpointIDs() -> [AudioEndpointID] {
        guard isConnected,
              let data = callStruct(.getAudioSemanticTopologyEndpoints, initialCap: 72) else {
            return []
        }
        return AudioSemanticTopologyWireDecoder.decodeEndpointIDs(data)
    }

    /// Reads an immutable console layout off the connector queue. Live values
    /// continue through the asynchronous control/meter lane; this is requested
    /// only when a topology revision changes.
    func requestAudioSemanticConsoleLayout(
        endpointID: AudioEndpointID,
        completion: @escaping (AudioSemanticConsoleLayoutSnapshot?) -> Void
    ) {
        connectionQueue.async { [weak self] in
            guard let self else {
                DispatchQueue.main.async { completion(nil) }
                return
            }
            let layout = self.getAudioSemanticConsoleLayout(endpointID: endpointID)
            DispatchQueue.main.async { completion(layout) }
        }
    }

    private func getAudioSemanticConsoleLayout(
        endpointID: AudioEndpointID
    ) -> AudioSemanticConsoleLayoutSnapshot? {
        guard connection != 0, endpointID.rawValue != 0 else { return nil }
        var scalarInput = endpointID.rawValue
        var output = Data(count: 1480)
        var outputLength = output.count
        let result = output.withUnsafeMutableBytes { outputBytes in
            IOConnectCallMethod(
                connection,
                Method.getAudioSemanticConsoleLayout.rawValue,
                &scalarInput,
                1,
                nil,
                0,
                nil,
                nil,
                outputBytes.baseAddress,
                &outputLength
            )
        }
        guard result == KERN_SUCCESS, outputLength == output.count else { return nil }
        return AudioSemanticConsoleLayoutWireDecoder.decode(output)
    }

    /// Reads one coherent, driver-owned hardware-mixer matrix. The call runs
    /// off the main actor; callers can retry when the profile is still loading
    /// its initial hardware snapshot.
    func requestAudioSemanticMatrix(
        endpointID: AudioEndpointID,
        completion: @escaping (AudioSemanticMatrixSnapshot?) -> Void
    ) {
        connectionQueue.async { [weak self] in
            guard let self else {
                DispatchQueue.main.async { completion(nil) }
                return
            }
            let matrix = self.getAudioSemanticMatrix(endpointID: endpointID)
            DispatchQueue.main.async { completion(matrix) }
        }
    }

    /// Enumerates only endpoints whose protocol exposes a coherent semantic
    /// matrix. This is distinct from configuration endpoints: Saffire's mixer
    /// does not imply a rate/optical configuration card.
    func getAudioSemanticMatrixEndpointIDs() -> [AudioEndpointID] {
        guard isConnected,
              let data = callStruct(.getAudioSemanticMatrixEndpoints, initialCap: 72) else {
            return []
        }
        return AudioSemanticMatrixWireDecoder.decodeEndpointIDs(data)
    }

    private func getAudioSemanticMatrix(
        endpointID: AudioEndpointID
    ) -> AudioSemanticMatrixSnapshot? {
        guard connection != 0, endpointID.rawValue != 0 else { return nil }
        var scalarInput = endpointID.rawValue
        var output = Data(count: 1784)
        var outputLength = output.count
        let result = output.withUnsafeMutableBytes { outputBytes in
            IOConnectCallMethod(
                connection,
                Method.getAudioSemanticMatrix.rawValue,
                &scalarInput,
                1,
                nil,
                0,
                nil,
                nil,
                outputBytes.baseAddress,
                &outputLength
            )
        }
        guard result == KERN_SUCCESS, outputLength == output.count else { return nil }
        return AudioSemanticMatrixWireDecoder.decode(output)
    }

    func requestAudioControlValue(endpointID: AudioEndpointID,
                                  controlID: UInt32,
                                  value: Int32) -> kern_return_t {
        guard isConnected, connection != 0, endpointID.rawValue != 0 else {
            return kIOReturnNotReady
        }
        var scalarInputs = [endpointID.rawValue, UInt64(controlID),
                            UInt64(UInt32(bitPattern: value))]
        return IOConnectCallScalarMethod(
            connection,
            Method.requestAudioControlValue.rawValue,
            &scalarInputs,
            UInt32(scalarInputs.count),
            nil,
            nil
        )
    }
}

private enum AudioConfigurationWireDecoder {
    private static let wireSize = 168
    private static let capabilitySize = 16
    private static let endpointListSize = 72
    private static let maximumEndpointCount = 8

    static func decodeEndpointIDs(_ data: Data) -> [AudioEndpointID] {
        guard data.count == endpointListSize,
              let version = data.u32(at: 0), version == 2,
              let count = data.u32(at: 4), count <= maximumEndpointCount else {
            return []
        }
        return (0..<Int(count)).compactMap { index in
            guard let raw = data.u64(at: 8 + index * 8), raw != 0 else { return nil }
            return AudioEndpointID(rawValue: raw)
        }
    }

    static func decode(_ data: Data) -> AudioConfigurationSnapshot? {
        guard data.count == wireSize,
              let endpointRaw = data.u64(at: 8),
              let topologyRevision = data.u64(at: 16), topologyRevision != 0,
              let count = data.u32(at: 4),
              count <= 8,
              let committed = decodeCapability(data, at: 24) else {
            return nil
        }
        let capabilities = (0..<Int(count)).compactMap {
            decodeCapability(data, at: 40 + ($0 * capabilitySize))
        }
        guard capabilities.count == Int(count) else { return nil }
        return AudioConfigurationSnapshot(
            endpointID: AudioEndpointID(rawValue: endpointRaw),
            topologyRevision: topologyRevision,
            committed: committed,
            capabilities: capabilities
        )
    }

    private static func decodeCapability(_ data: Data, at offset: Int) -> AudioConfigurationCapability? {
        guard let rate = data.u32(at: offset),
              let inputChannels = data.u32(at: offset + 4),
              let outputChannels = data.u32(at: offset + 8),
              let inputRaw = data.u8(at: offset + 12),
              let outputRaw = data.u8(at: offset + 13),
              let input = AudioOpticalMode(rawValue: inputRaw),
              let output = AudioOpticalMode(rawValue: outputRaw) else { return nil }
        return AudioConfigurationCapability(
            sampleRateHz: rate,
            inputOptical: input,
            outputOptical: output,
            inputChannels: inputChannels,
            outputChannels: outputChannels
        )
    }
}

private enum AudioControlSurfaceWireDecoder {
    private static let wireSize = 672
    private static let maximumValueCount = 80

    static func decode(_ data: Data) -> AudioControlSurfaceSnapshot? {
        guard data.count == wireSize,
              let version = data.u32(at: 0), version == 3,
              let rawKind = data.u32(at: 4),
              let kind = AudioControlSurfaceKind(rawValue: rawKind),
              let endpointRaw = data.u64(at: 8), endpointRaw != 0,
              let topologyRevision = data.u64(at: 16), topologyRevision != 0,
              let stateRevision = data.u32(at: 24),
              let count = data.u32(at: 28), count <= maximumValueCount else {
            return nil
        }
        let values = (0..<Int(count)).compactMap { index -> AudioControlSurfaceValue? in
            let offset = 32 + index * 8
            guard let id = data.u32(at: offset), let value = data.i32(at: offset + 4) else {
                return nil
            }
            return AudioControlSurfaceValue(id: id, value: value)
        }
        guard values.count == Int(count) else { return nil }
        return AudioControlSurfaceSnapshot(
            endpointID: AudioEndpointID(rawValue: endpointRaw), kind: kind,
            topologyRevision: topologyRevision, stateRevision: stateRevision, values: values)
    }
}

/// Fixed ABI decoder for `AudioSemanticTopologySnapshotWire`. Its offsets are
/// locked by matching `static_assert`s in the DriverKit wire headers.
enum AudioSemanticTopologyWireDecoder {
    nonisolated private static let wireSize = 3944
    nonisolated private static let topologyStart = 16
    nonisolated private static let nodeOffset = 68
    nonisolated private static let portOffset = 260
    nonisolated private static let fixedLinkOffset = 1060
    nonisolated private static let routerOffset = 1380
    nonisolated private static let routeBundleOffset = 1508
    nonisolated private static let routeOffset = 1764
    nonisolated private static let crosspointOffset = 1956
    nonisolated private static let parameterOffset = 2532
    nonisolated private static let meterOffset = 3652

    nonisolated static func decodeEndpointIDs(_ data: Data) -> [AudioEndpointID] {
        guard data.count == 72,
              let version = data.u32(at: 0), version == 1,
              let count = data.u32(at: 4), count <= 8 else {
            return []
        }
        let endpointIDs = (0..<Int(count)).compactMap { index -> AudioEndpointID? in
            guard let raw = data.u64(at: 8 + index * 8), raw != 0 else { return nil }
            return AudioEndpointID(rawValue: raw)
        }
        return endpointIDs.count == Int(count) ? endpointIDs : []
    }

    nonisolated static func decode(_ data: Data) -> AudioSemanticTopologySnapshot? {
        guard data.count == wireSize,
              let wireVersion = data.u32(at: 0), wireVersion == 3,
              let endpointRaw = data.u64(at: 8), endpointRaw != 0,
              let topologyVersion = data.u32(at: topologyStart), topologyVersion == 3,
              let deviceKind = data.u32(at: topologyStart + 4), deviceKind != 0,
              let topologyRevision = data.u64(at: topologyStart + 8), topologyRevision != 0,
              let counts = counts(from: data) else {
            return nil
        }

        guard let nodes: [AudioSemanticTopologySnapshot.Node] = entries(
            data, count: counts.nodes, maximum: 16, offset: nodeOffset, stride: 12,
            decode: node),
              let ports: [AudioSemanticTopologySnapshot.Port] = entries(
                data, count: counts.ports, maximum: 40, offset: portOffset, stride: 20,
                decode: port),
              let fixedLinks: [AudioSemanticTopologySnapshot.FixedLink] = entries(
                data, count: counts.fixedLinks, maximum: 40, offset: fixedLinkOffset, stride: 8,
                decode: fixedLink),
              let routers: [AudioSemanticTopologySnapshot.Router] = entries(
                data, count: counts.routers, maximum: 8, offset: routerOffset, stride: 16,
                decode: router),
              let routeBundles: [AudioSemanticTopologySnapshot.RouteBundle] = entries(
                data, count: counts.routeBundles, maximum: 16, offset: routeBundleOffset, stride: 16,
                decode: routeBundle),
              let routes: [AudioSemanticTopologySnapshot.Route] = entries(
                data, count: counts.routes, maximum: 24, offset: routeOffset, stride: 8,
                decode: route),
              let crosspoints: [AudioSemanticTopologySnapshot.Crosspoint] = entries(
                data, count: counts.crosspoints, maximum: 24, offset: crosspointOffset, stride: 24,
                decode: crosspoint),
              let parameters: [AudioSemanticTopologySnapshot.Parameter] = entries(
                data, count: counts.parameters, maximum: 28, offset: parameterOffset, stride: 40,
                decode: parameter),
              let meters: [AudioSemanticTopologySnapshot.Meter] = entries(
                data, count: counts.meters, maximum: 12, offset: meterOffset, stride: 24,
                decode: meter) else {
            return nil
        }

        return AudioSemanticTopologySnapshot(
            endpointID: AudioEndpointID(rawValue: endpointRaw), deviceKind: deviceKind,
            topologyRevision: topologyRevision, nodes: nodes, ports: ports,
            fixedLinks: fixedLinks, routers: routers, routeBundles: routeBundles,
            routes: routes, crosspoints: crosspoints, parameters: parameters, meters: meters)
    }

    nonisolated private struct Counts {
        let nodes: UInt32
        let ports: UInt32
        let fixedLinks: UInt32
        let routers: UInt32
        let routeBundles: UInt32
        let routes: UInt32
        let crosspoints: UInt32
        let parameters: UInt32
        let meters: UInt32
    }

    nonisolated private static func counts(from data: Data) -> Counts? {
        guard let nodes = data.u32(at: topologyStart + 16), nodes <= 16,
              let ports = data.u32(at: topologyStart + 20), ports <= 40,
              let fixedLinks = data.u32(at: topologyStart + 24), fixedLinks <= 40,
              let routers = data.u32(at: topologyStart + 28), routers <= 8,
              let routeBundles = data.u32(at: topologyStart + 32), routeBundles <= 16,
              let routes = data.u32(at: topologyStart + 36), routes <= 24,
              let crosspoints = data.u32(at: topologyStart + 40), crosspoints <= 24,
              let parameters = data.u32(at: topologyStart + 44), parameters <= 28,
              let meters = data.u32(at: topologyStart + 48), meters <= 12 else {
            return nil
        }
        return Counts(nodes: nodes, ports: ports, fixedLinks: fixedLinks, routers: routers,
                      routeBundles: routeBundles, routes: routes, crosspoints: crosspoints,
                      parameters: parameters, meters: meters)
    }

    nonisolated private static func entries<T>(
        _ data: Data, count: UInt32, maximum: UInt32, offset: Int, stride: Int,
        decode: (Data, Int) -> T?
    ) -> [T]? {
        guard count <= maximum else { return nil }
        var result: [T] = []
        result.reserveCapacity(Int(count))
        for index in 0..<Int(count) {
            guard let entry = decode(data, offset + index * stride) else { return nil }
            result.append(entry)
        }
        return result
    }

    nonisolated private static func node(_ data: Data, _ offset: Int) -> AudioSemanticTopologySnapshot.Node? {
        guard let id = data.u32(at: offset), id != 0,
              let kindRaw = data.u32(at: offset + 4),
              let kind = AudioSemanticTopologySnapshot.NodeKind(rawValue: kindRaw),
              let endpointRaw = data.u32(at: offset + 8),
              let endpointKind = AudioSemanticTopologySnapshot.EndpointKind(rawValue: endpointRaw) else {
            return nil
        }
        return .init(id: id, kind: kind, endpointKind: endpointKind)
    }

    nonisolated private static func port(_ data: Data, _ offset: Int) -> AudioSemanticTopologySnapshot.Port? {
        guard let id = data.u32(at: offset), id != 0,
              let ownerNodeID = data.u32(at: offset + 4), ownerNodeID != 0,
              let directionRaw = data.u32(at: offset + 8),
              let direction = AudioSemanticTopologySnapshot.PortDirection(rawValue: directionRaw),
              let signalRaw = data.u32(at: offset + 12),
              let signalKind = AudioSemanticTopologySnapshot.SignalKind(rawValue: signalRaw),
              let signalIndex = data.u32(at: offset + 16) else {
            return nil
        }
        return .init(id: id, ownerNodeID: ownerNodeID, direction: direction,
                     signalKind: signalKind, signalIndex: signalIndex)
    }

    nonisolated private static func fixedLink(_ data: Data, _ offset: Int) -> AudioSemanticTopologySnapshot.FixedLink? {
        guard let sourcePortID = data.u32(at: offset), sourcePortID != 0,
              let destinationPortID = data.u32(at: offset + 4), destinationPortID != 0 else { return nil }
        return .init(sourcePortID: sourcePortID, destinationPortID: destinationPortID)
    }

    nonisolated private static func router(_ data: Data, _ offset: Int) -> AudioSemanticTopologySnapshot.Router? {
        guard let nodeID = data.u32(at: offset), nodeID != 0,
              let maxActiveBundles = data.u32(at: offset + 4),
              let maxSourcesPerOutput = data.u32(at: offset + 8),
              let maxDestinationsPerInput = data.u32(at: offset + 12) else { return nil }
        return .init(nodeID: nodeID, maxActiveBundles: maxActiveBundles,
                     maxSourcesPerOutput: maxSourcesPerOutput,
                     maxDestinationsPerInput: maxDestinationsPerInput)
    }

    nonisolated private static func routeBundle(_ data: Data, _ offset: Int) -> AudioSemanticTopologySnapshot.RouteBundle? {
        guard let routerNodeID = data.u32(at: offset), routerNodeID != 0,
              let bundleID = data.u32(at: offset + 4), bundleID != 0,
              let routeOffset = data.u32(at: offset + 8),
              let routeCount = data.u32(at: offset + 12) else { return nil }
        return .init(routerNodeID: routerNodeID, bundleID: bundleID,
                     routeOffset: routeOffset, routeCount: routeCount)
    }

    nonisolated private static func route(_ data: Data, _ offset: Int) -> AudioSemanticTopologySnapshot.Route? {
        guard let sourcePortID = data.u32(at: offset), sourcePortID != 0,
              let destinationPortID = data.u32(at: offset + 4), destinationPortID != 0 else { return nil }
        return .init(sourcePortID: sourcePortID, destinationPortID: destinationPortID)
    }

    nonisolated private static func crosspoint(_ data: Data, _ offset: Int) -> AudioSemanticTopologySnapshot.Crosspoint? {
        guard let id = data.u32(at: offset), id != 0,
              let sourcePortID = data.u32(at: offset + 4), sourcePortID != 0,
              let destinationPortID = data.u32(at: offset + 8), destinationPortID != 0,
              let presentationRaw = data.u32(at: offset + 12),
              let presentation = AudioSemanticTopologySnapshot.CrosspointPresentation(rawValue: presentationRaw),
              let groupRaw = data.u32(at: offset + 16),
              let presentationGroup = AudioSemanticTopologySnapshot.CrosspointGroup(rawValue: groupRaw),
              presentationGroup != .none,
              let presentationOrder = data.u32(at: offset + 20) else { return nil }
        return .init(id: id, sourcePortID: sourcePortID, destinationPortID: destinationPortID,
                     presentation: presentation, presentationGroup: presentationGroup,
                     presentationOrder: presentationOrder)
    }

    nonisolated private static func parameter(_ data: Data, _ offset: Int) -> AudioSemanticTopologySnapshot.Parameter? {
        guard let id = data.u32(at: offset), id != 0,
              let targetRaw = data.u32(at: offset + 4),
              let targetKind = AudioSemanticTopologySnapshot.TargetKind(rawValue: targetRaw),
              let targetID = data.u32(at: offset + 8), targetID != 0,
              let kindRaw = data.u32(at: offset + 12),
              let kind = AudioSemanticTopologySnapshot.ParameterKind(rawValue: kindRaw),
              let valueRaw = data.u32(at: offset + 16),
              let valueKind = AudioSemanticTopologySnapshot.ValueKind(rawValue: valueRaw),
              let unitRaw = data.u32(at: offset + 20),
              let unit = AudioSemanticTopologySnapshot.Unit(rawValue: unitRaw),
              let minimum = data.i32(at: offset + 24),
              let maximum = data.i32(at: offset + 28), minimum <= maximum,
              let step = data.i32(at: offset + 32), step > 0,
              let presentationRaw = data.u32(at: offset + 36),
              let presentation = AudioSemanticTopologySnapshot.Presentation(rawValue: presentationRaw) else {
            return nil
        }
        return .init(id: id, targetKind: targetKind, targetID: targetID, kind: kind,
                     valueKind: valueKind, unit: unit, minimum: minimum, maximum: maximum,
                     step: step, presentation: presentation)
    }

    nonisolated private static func meter(_ data: Data, _ offset: Int) -> AudioSemanticTopologySnapshot.Meter? {
        guard let id = data.u32(at: offset), id != 0,
              let targetPortID = data.u32(at: offset + 4), targetPortID != 0,
              let kindRaw = data.u32(at: offset + 8),
              let kind = AudioSemanticTopologySnapshot.MeterKind(rawValue: kindRaw),
              let unitRaw = data.u32(at: offset + 12),
              let unit = AudioSemanticTopologySnapshot.MeterUnit(rawValue: unitRaw),
              let minimum = data.i32(at: offset + 16),
              let maximum = data.i32(at: offset + 20), minimum <= maximum else { return nil }
        return .init(id: id, targetPortID: targetPortID, kind: kind, unit: unit,
                     minimum: minimum, maximum: maximum)
    }
}

/// Fixed ABI decoder for selector 1030. This layout is deliberately separate
/// from the general graph reply: dense consoles retain their complete semantic
/// strip/meter mapping without widening the 3,944-byte graph ABI.
private enum AudioSemanticConsoleLayoutWireDecoder {
    private static let wireSize = 1480
    private static let layoutStart = 16
    private static let stripOffset = 56
    private static let crosspointOffset = 856
    private static let maximumStrips = 20
    private static let maximumCrosspoints = 32

    static func decode(_ data: Data) -> AudioSemanticConsoleLayoutSnapshot? {
        guard data.count == wireSize,
              let wireVersion = data.u32(at: 0), wireVersion == 1,
              let endpoint = data.u64(at: 8), endpoint != 0,
              let layoutVersion = data.u32(at: layoutStart), layoutVersion == 1,
              let deviceKind = data.u32(at: layoutStart + 4), deviceKind != 0,
              let topologyRevision = data.u64(at: layoutStart + 8), topologyRevision != 0,
              let stripCount = data.u32(at: layoutStart + 16), stripCount <= maximumStrips,
              let crosspointCount = data.u32(at: layoutStart + 20),
              crosspointCount <= maximumCrosspoints else { return nil }

        let strips = (0..<Int(stripCount)).compactMap { strip(data, stripOffset + $0 * 40) }
        let crosspoints = (0..<Int(crosspointCount)).compactMap {
            crosspoint(data, crosspointOffset + $0 * 20)
        }
        guard strips.count == Int(stripCount), crosspoints.count == Int(crosspointCount),
              Set(strips.map(\.id)).count == strips.count,
              Set(crosspoints.map(\.id)).count == crosspoints.count else { return nil }
        return .init(endpointID: AudioEndpointID(rawValue: endpoint), deviceKind: deviceKind,
                     topologyRevision: topologyRevision, strips: strips, crosspoints: crosspoints)
    }

    private static func strip(_ data: Data, _ offset: Int) -> AudioSemanticConsoleLayoutSnapshot.Strip? {
        guard let id = data.u32(at: offset), id != 0,
              let kindRaw = data.u8(at: offset + 4),
              let kind = AudioSemanticConsoleLayoutSnapshot.StripKind(rawValue: kindRaw),
              let signalRaw = data.u32(at: offset + 8),
              let signalKind = AudioSemanticTopologySnapshot.SignalKind(rawValue: signalRaw),
              signalKind != .none,
              let channelCount = data.u8(at: offset + 12), channelCount > 0,
              let flags = data.u8(at: offset + 13), flags & ~15 == 0,
              let firstSignalIndex = data.u32(at: offset + 16), firstSignalIndex != 0,
              let levelControlID = data.u32(at: offset + 20), levelControlID != 0,
              let panControlID = data.u32(at: offset + 24),
              let auxControlID = data.u32(at: offset + 28),
              let sourceControlID = data.u32(at: offset + 32),
              let sourceKindRaw = data.u8(at: offset + 36),
              let sourceKind = AudioSemanticConsoleLayoutSnapshot.SourceKind(rawValue: sourceKindRaw),
              let meterCount = data.u8(at: offset + 37), meterCount <= channelCount,
              let meterFirstIndex = data.u16(at: offset + 38),
              (sourceControlID == 0) == (sourceKind == .none) else { return nil }
        return .init(id: id, kind: kind, signalKind: signalKind, channelCount: channelCount,
                     flags: flags, firstSignalIndex: firstSignalIndex, levelControlID: levelControlID,
                     panControlID: panControlID, auxControlID: auxControlID,
                     sourceControlID: sourceControlID, sourceKind: sourceKind,
                     meterCount: meterCount, meterFirstIndex: meterFirstIndex)
    }

    private static func crosspoint(_ data: Data, _ offset: Int) -> AudioSemanticConsoleLayoutSnapshot.Crosspoint? {
        guard let id = data.u32(at: offset), id != 0,
              let sourceStripID = data.u32(at: offset + 4), sourceStripID != 0,
              let busRaw = data.u8(at: offset + 8),
              let destinationBus = AudioSemanticConsoleLayoutSnapshot.Bus(rawValue: busRaw),
              let controlID = data.u32(at: offset + 12), controlID != 0,
              let enabledMask = data.u32(at: offset + 16), enabledMask != 0 else { return nil }
        return .init(id: id, sourceStripID: sourceStripID, destinationBus: destinationBus,
                     controlID: controlID, enabledMask: enabledMask)
    }
}

/// Fixed ABI decoder for selector 1031. This is intentionally separate from
/// control values: a matrix is a dense state snapshot with driver-declared
/// axes, not an app-side reconstruction of DICE records.
private enum AudioSemanticMatrixWireDecoder {
    private static let wireSize = 1784
    private static let matrixStart = 16
    private static let axisSize = 12
    private static let inputAxisOffset = matrixStart + 36
    private static let outputAxisOffset = matrixStart + 324
    private static let coefficientOffset = matrixStart + 612
    private static let maximumInputs = 24
    private static let maximumOutputs = 24

    static func decodeEndpointIDs(_ data: Data) -> [AudioEndpointID] {
        guard data.count == 72,
              let version = data.u32(at: 0), version == 1,
              let countRaw = data.u32(at: 4), countRaw <= 8 else {
            return []
        }
        let count = Int(countRaw)
        var endpoints: [AudioEndpointID] = []
        endpoints.reserveCapacity(count)
        for index in 0..<count {
            guard let raw = data.u64(at: 8 + index * MemoryLayout<UInt64>.size), raw != 0 else {
                return []
            }
            endpoints.append(AudioEndpointID(rawValue: raw))
        }
        return endpoints
    }

    static func decode(_ data: Data) -> AudioSemanticMatrixSnapshot? {
        guard data.count == wireSize,
              let wireVersion = data.u32(at: 0), wireVersion == 1,
              let endpoint = data.u64(at: 8), endpoint != 0,
              let matrixVersion = data.u32(at: matrixStart), matrixVersion == 2,
              let deviceKind = data.u32(at: matrixStart + 4), deviceKind != 0,
              let topologyRevision = data.u64(at: matrixStart + 8), topologyRevision != 0,
              let stateRevision = data.u32(at: matrixStart + 16),
              let kind = data.u32(at: matrixStart + 20), kind == 1,
              let inputCount = data.u32(at: matrixStart + 24), inputCount > 0,
              inputCount <= maximumInputs,
              let outputCount = data.u32(at: matrixStart + 28), outputCount > 0,
              outputCount <= maximumOutputs,
              let coefficientMaximum = data.u16(at: matrixStart + 32), coefficientMaximum > 0,
              let gainLawRaw = data.u16(at: matrixStart + 34),
              let gainLaw = AudioSemanticMatrixSnapshot.GainLaw(rawValue: gainLawRaw) else {
            return nil
        }
        let inputs = (0..<Int(inputCount)).compactMap { axis(data, inputAxisOffset + $0 * axisSize) }
        let outputs = (0..<Int(outputCount)).compactMap { axis(data, outputAxisOffset + $0 * axisSize) }
        guard inputs.count == Int(inputCount), outputs.count == Int(outputCount),
              Set(inputs.map(\.portID)).count == inputs.count,
              Set(outputs.map(\.portID)).count == outputs.count else { return nil }

        var coefficients: [UInt16] = []
        coefficients.reserveCapacity(Int(inputCount * outputCount))
        for output in 0..<Int(outputCount) {
            for input in 0..<Int(inputCount) {
                let offset = coefficientOffset + (output * maximumInputs + input) * MemoryLayout<UInt16>.size
                guard let value = data.u16(at: offset), value <= coefficientMaximum else { return nil }
                coefficients.append(value)
            }
        }
        return .init(endpointID: AudioEndpointID(rawValue: endpoint), deviceKind: deviceKind,
                     topologyRevision: topologyRevision, stateRevision: stateRevision,
                     coefficientMaximum: coefficientMaximum, gainLaw: gainLaw, inputs: inputs,
                     outputs: outputs, coefficients: coefficients)
    }

    private static func axis(_ data: Data, _ offset: Int) -> AudioSemanticMatrixSnapshot.Axis? {
        guard let portID = data.u32(at: offset), portID != 0,
              let signalRaw = data.u32(at: offset + 4),
              let signalKind = AudioSemanticTopologySnapshot.SignalKind(rawValue: signalRaw),
              signalKind != .none,
              let signalIndex = data.u32(at: offset + 8), signalIndex != 0 else { return nil }
        return .init(portID: portID, signalKind: signalKind, signalIndex: signalIndex)
    }
}

/// Mirrors the compact scalar layout of selectors 1023–1025. The layout is
/// deliberately fixed-size and bounded, so the async completion queue never
/// transports a pointer or allocates a variable-size reply.
private enum AudioAsyncSnapshotDecoder {
    static func configuration(_ values: [UInt64]) -> AudioConfigurationSnapshot? {
        guard values.count >= 5, values[1] != 0, values[2] != 0,
              let committed = capability(values[3]) else { return nil }
        let count = Int(values[4])
        guard count <= 8, values.count == 5 + count else { return nil }
        let capabilities = values.dropFirst(5).compactMap { capability($0) }
        guard capabilities.count == count else { return nil }
        return AudioConfigurationSnapshot(
            endpointID: AudioEndpointID(rawValue: values[1]), topologyRevision: values[2],
            committed: committed, capabilities: capabilities)
    }

    struct ControlsHeader {
        let endpointID: AudioEndpointID
        let topologyRevision: UInt64
        let kind: UInt32
        let stateRevision: UInt32
        let valueCount: Int
    }

    /// The async control-surface reply is a header only — see
    /// `HandleGetAudioControlSurfaceAsync`. Values come from the struct selector.
    static func controlsHeader(_ values: [UInt64]) -> ControlsHeader? {
        guard values.count == 5, values[1] != 0, values[2] != 0 else { return nil }
        let header = values[3]
        return ControlsHeader(
            endpointID: AudioEndpointID(rawValue: values[1]),
            topologyRevision: values[2],
            kind: UInt32(truncatingIfNeeded: header >> 32),
            stateRevision: UInt32(truncatingIfNeeded: header),
            valueCount: Int(values[4]))
    }

    static func meters(_ values: [UInt64]) -> AudioMeterSnapshot? {
        guard values.count >= 5, values[1] != 0, values[2] != 0 else { return nil }
        let rateAndSequence = values[3]
        let flagsAndCount = values[4]
        let telemetrySequence = UInt32(truncatingIfNeeded: rateAndSequence)
        let rate = UInt32(truncatingIfNeeded: rateAndSequence >> 32)
        let count = Int(UInt32(truncatingIfNeeded: flagsAndCount))
        let enabled = ((flagsAndCount >> 32) & 1) != 0
        let locked = ((flagsAndCount >> 33) & 1) != 0
        let external = ((flagsAndCount >> 34) & 1) != 0
        let hardwareSwitch = ((flagsAndCount >> 35) & 1) != 0
        let rotaryCount = Int((flagsAndCount >> 36) & 0xFF)
        let quadletCount = (count + 3) / 4
        // One trailing scalar carries the encoders, packed four to a scalar.
        guard count <= 40, rotaryCount <= 3, values.count == 5 + quadletCount + 1 else {
            return nil
        }
        var peaks: [Int16] = []
        peaks.reserveCapacity(count)
        for index in 0..<count {
            let packed = values[5 + index / 4]
            let raw = UInt16(truncatingIfNeeded: packed >> ((index % 4) * 16))
            peaks.append(Int16(bitPattern: raw))
        }
        let packedRotaries = values[5 + quadletCount]
        let rotaries = (0..<rotaryCount).map { index -> Int16 in
            Int16(bitPattern: UInt16(truncatingIfNeeded: packedRotaries >> (index * 16)))
        }
        return AudioMeterSnapshot(
            endpointID: AudioEndpointID(rawValue: values[1]), topologyRevision: values[2],
            telemetrySequence: telemetrySequence,
            detectedSampleRateHz: rate, isEnabled: enabled, isClockLocked: locked,
            isExternallySynced: external, hardwareSwitch: hardwareSwitch,
            rotaries: rotaries, values: peaks)
    }

    private static func capability(_ packed: UInt64) -> AudioConfigurationCapability? {
        let rate = UInt32(truncatingIfNeeded: packed)
        let inputChannels = UInt32((packed >> 32) & 0xff)
        let outputChannels = UInt32((packed >> 40) & 0xff)
        guard rate != 0,
              let input = AudioOpticalMode(rawValue: UInt8((packed >> 48) & 0xff)),
              let output = AudioOpticalMode(rawValue: UInt8((packed >> 56) & 0xff)) else {
            return nil
        }
        return AudioConfigurationCapability(
            sampleRateHz: rate, inputOptical: input, outputOptical: output,
            inputChannels: inputChannels, outputChannels: outputChannels)
    }
}

private extension Data {
    nonisolated func u8(at offset: Int) -> UInt8? {
        guard offset < count else { return nil }
        return self[startIndex + offset]
    }

    nonisolated func u32(at offset: Int) -> UInt32? {
        guard offset >= 0, offset + 4 <= count else { return nil }
        return withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self) }
    }

    nonisolated func u16(at offset: Int) -> UInt16? {
        guard offset >= 0, offset + 2 <= count else { return nil }
        return withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt16.self) }
    }

    nonisolated func u64(at offset: Int) -> UInt64? {
        guard offset >= 0, offset + 8 <= count else { return nil }
        return withUnsafeBytes { $0.loadUnaligned(fromByteOffset: offset, as: UInt64.self) }
    }

    nonisolated func i32(at offset: Int) -> Int32? {
        guard let value = u32(at: offset) else { return nil }
        return Int32(bitPattern: value)
    }
}
