import Foundation
import IOKit
import Metal

struct AudioViewSnapshot: Sendable {
    var writeEndFrame: UInt64 = 0
    var activeRingFrames: UInt32 = 0
    var channels: UInt32 = 0
    var sampleRate: UInt32 = 0
    var ioRunning = false
    var epoch: UInt64 = 0
    var validHistoryFrames: UInt32 = 0
    var mappedFrames: UInt64 = 0
    var lastLeftSamples: [Float] = []
    var lastRightSamples: [Float] = []
}

struct ScopeMetricsSnapshot: Sendable {
    var cpuEncodeMs: Double?
    var scheduledToStartMs: Double?
    var gpuMs: Double?
    var completionMs: Double?
    var sampleAgeMs: Double?
    var overwriteMarginMs: Double?
    var leftPeak: Float = 0
    var rightPeak: Float = 0
    var correlation: Float = 0
    var inFlight = 0
    var windowsRendered: UInt64 = 0
    var windowsCrossingWrap: UInt64 = 0
    var badWindows: UInt64 = 0
    var cpuGpuValidationChecks: UInt64 = 0
    var cpuGpuValidationMismatches: UInt64 = 0
}

struct AudioRingWireState: Sendable {
    let writeEndFrame: UInt64
    let activeRingFrames: UInt32
    let channels: UInt32
    let sampleRate: UInt32
    let ioRunning: Bool
    let epoch: UInt64
    let validHistoryFrames: UInt32
}

private final class AudioRingMappingLifetime: @unchecked Sendable {
    private let connection: io_connect_t
    private let address: mach_vm_address_t

    init(connection: io_connect_t, address: mach_vm_address_t) {
        self.connection = connection
        self.address = address
    }

    deinit {
        IOConnectUnmapMemory64(connection, 0, mach_task_self_, address)
        IOServiceClose(connection)
    }
}

final class WaveformRenderState {
    private let lock = NSLock()
    private var snapshot = AudioViewSnapshot()

    func update(_ value: AudioViewSnapshot) {
        lock.lock()
        snapshot = value
        lock.unlock()
    }

    func read() -> AudioViewSnapshot {
        lock.lock()
        defer { lock.unlock() }
        return snapshot
    }
}

final class ScopeDiagnostics: @unchecked Sendable {
    private let lock = NSLock()
    private var value = ScopeMetricsSnapshot()

    func read() -> ScopeMetricsSnapshot {
        lock.lock()
        defer { lock.unlock() }
        return value
    }

    func submitted(crossesWrap: Bool) {
        lock.lock()
        value.inFlight += 1
        if crossesWrap { value.windowsCrossingWrap += 1 }
        lock.unlock()
    }

    func completed(cpuEncodeMs: Double,
                   scheduledToStartMs: Double?,
                   gpuMs: Double?,
                   completionMs: Double,
                   sampleAgeMs: Double?,
                   overwriteMarginMs: Double?,
                   leftPeak: Float,
                   rightPeak: Float,
                   correlation: Float,
                   validationPassed: Bool?,
                   windowWasSafe: Bool) {
        lock.lock()
        value.inFlight = max(0, value.inFlight - 1)
        value.windowsRendered += 1
        value.cpuEncodeMs = cpuEncodeMs
        value.scheduledToStartMs = scheduledToStartMs
        value.gpuMs = gpuMs
        value.completionMs = completionMs
        value.sampleAgeMs = sampleAgeMs
        value.overwriteMarginMs = overwriteMarginMs
        value.leftPeak = leftPeak
        value.rightPeak = rightPeak
        value.correlation = correlation
        if let validationPassed {
            value.cpuGpuValidationChecks += 1
            if !validationPassed {
                value.cpuGpuValidationMismatches += 1
            }
        }
        if !windowWasSafe || validationPassed == false {
            value.badWindows += 1
        }
        lock.unlock()
    }

    func failed() {
        lock.lock()
        value.inFlight = max(0, value.inFlight - 1)
        value.badWindows += 1
        lock.unlock()
    }
}

enum AudioRingClientError: LocalizedError {
    case serviceNotFound
    case openFailed(kern_return_t)
    case mappingFailed(kern_return_t)
    case stateQueryFailed(kern_return_t)
    case invalidGeometry
    case metalUnavailable
    case shaderUnavailable
    case pipelineFailed
    case zeroCopyImportFailed
    case analysisBufferUnavailable

    var errorDescription: String? {
        switch self {
        case .serviceNotFound:
            return "VirtualAudioDriver service not found. Activate the lab extension first."
        case .openFailed(let result):
            return String(format: "Opening LDBG failed (0x%08x). Check the host userclient-access entitlement.", result)
        case .mappingFailed(let result):
            return String(format: "Mapping the output ring failed (0x%08x).", result)
        case .stateQueryFailed(let result):
            return String(format: "Reading audio ring state failed (0x%08x).", result)
        case .invalidGeometry:
            return "The output ring returned invalid geometry or a non-page-aligned mapping."
        case .metalUnavailable:
            return "No Metal device is available."
        case .shaderUnavailable:
            return "The phase-scope Metal functions were not found in the app library."
        case .pipelineFailed:
            return "Metal could not create the phase-scope pipelines."
        case .zeroCopyImportFailed:
            return "ZERO-COPY IMPORT FAILED: Metal rejected the mapped ADK output ring."
        case .analysisBufferUnavailable:
            return "Metal could not allocate the small analyzer readback buffer."
        }
    }
}

final class AudioRingStateReader: @unchecked Sendable {
    private static let selector: UInt32 = 1
    private static let outputCount: UInt32 = 7
    private let connection: io_connect_t
    private let lock = NSLock()

    init(connection: io_connect_t) {
        self.connection = connection
    }

    func read() throws -> AudioRingWireState {
        lock.lock()
        defer { lock.unlock() }
        var values = [UInt64](repeating: 0, count: Int(Self.outputCount))
        var outputCount = Self.outputCount
        let result = values.withUnsafeMutableBufferPointer { buffer in
            IOConnectCallScalarMethod(connection, Self.selector, nil, 0,
                                      buffer.baseAddress, &outputCount)
        }
        guard result == KERN_SUCCESS, outputCount == Self.outputCount else {
            throw AudioRingClientError.stateQueryFailed(result)
        }
        let state = AudioRingWireState(
            writeEndFrame: values[0],
            activeRingFrames: UInt32(truncatingIfNeeded: values[1]),
            channels: UInt32(truncatingIfNeeded: values[2]),
            sampleRate: UInt32(truncatingIfNeeded: values[3]),
            ioRunning: values[4] != 0,
            epoch: values[5],
            validHistoryFrames: UInt32(truncatingIfNeeded: values[6]))
        guard state.activeRingFrames > 0, state.channels >= 2,
              state.sampleRate > 0,
              state.validHistoryFrames <= state.activeRingFrames else {
            throw AudioRingClientError.invalidGeometry
        }
        return state
    }
}

@MainActor
final class AudioRingClient {
    private enum Wire {
        static let userClientType: UInt32 = 0x4C44_4247 // 'LDBG'
        static let outputRingMemoryType: UInt32 = 0
    }

    private var connection: io_connect_t = IO_OBJECT_NULL
    private var mappedAddress: mach_vm_address_t = 0
    private var mappedSize: mach_vm_size_t = 0
    private var mappingLifetime: AudioRingMappingLifetime?
    private(set) var stateReader: AudioRingStateReader?

    private(set) var snapshot = AudioViewSnapshot()
    let renderState = WaveformRenderState()
    let diagnostics = ScopeDiagnostics()
    private(set) var metalDevice: MTLDevice?
    private(set) var metalBuffer: MTLBuffer?
    private(set) var waveformPipeline: MTLRenderPipelineState?
    private(set) var renderPipeline: MTLRenderPipelineState?
    private(set) var analysisPipeline: MTLComputePipelineState?
    private(set) var analysisBuffer: MTLBuffer?

    func open() throws {
        guard connection == IO_OBJECT_NULL else { return }

        let service = findService()
        guard service != IO_OBJECT_NULL else {
            throw AudioRingClientError.serviceNotFound
        }
        defer { IOObjectRelease(service) }

        let openResult = IOServiceOpen(service, mach_task_self_,
                                       Wire.userClientType, &connection)
        guard openResult == KERN_SUCCESS, connection != IO_OBJECT_NULL else {
            connection = IO_OBJECT_NULL
            throw AudioRingClientError.openFailed(openResult)
        }

        let mapResult = IOConnectMapMemory64(
            connection, Wire.outputRingMemoryType, mach_task_self_,
            &mappedAddress, &mappedSize, UInt32(kIOMapAnywhere))
        guard mapResult == KERN_SUCCESS else {
            closeConnection()
            throw AudioRingClientError.mappingFailed(mapResult)
        }

        guard mappedAddress % UInt64(vm_page_size) == 0,
              mappedSize != 0,
              mappedSize % UInt64(vm_page_size) == 0,
              mappedSize <= UInt64(Int.max),
              let rawPointer = UnsafeMutableRawPointer(
                bitPattern: UInt(mappedAddress)) else {
            closeConnection()
            throw AudioRingClientError.invalidGeometry
        }
        let lifetime = AudioRingMappingLifetime(
            connection: connection, address: mappedAddress)
        mappingLifetime = lifetime
        stateReader = AudioRingStateReader(connection: connection)

        guard let device = MTLCreateSystemDefaultDevice() else {
            closeConnection()
            throw AudioRingClientError.metalUnavailable
        }
        metalDevice = device

        // The imported pointer is the IOConnect mapping itself. There is no
        // PCM-copy fallback; the Metal allocation retains the map lease.
        guard let buffer = device.makeBuffer(
            bytesNoCopy: rawPointer,
            length: Int(mappedSize),
            options: .storageModeShared,
            deallocator: { [lifetime] _, _ in
                withExtendedLifetime(lifetime) {}
            }) else {
            closeConnection()
            throw AudioRingClientError.zeroCopyImportFailed
        }
        metalBuffer = buffer

        guard let library = device.makeDefaultLibrary(),
              let waveformVertex = library.makeFunction(name: "waveformVertex"),
              let waveformFragment = library.makeFunction(name: "waveformFragment"),
              let vertex = library.makeFunction(name: "phaseScopeVertex"),
              let fragment = library.makeFunction(name: "phaseScopeFragment"),
              let analysis = library.makeFunction(name: "analyzeRing") else {
            closeConnection()
            throw AudioRingClientError.shaderUnavailable
        }
        let waveformDescriptor = MTLRenderPipelineDescriptor()
        waveformDescriptor.vertexFunction = waveformVertex
        waveformDescriptor.fragmentFunction = waveformFragment
        waveformDescriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        let renderDescriptor = MTLRenderPipelineDescriptor()
        renderDescriptor.vertexFunction = vertex
        renderDescriptor.fragmentFunction = fragment
        renderDescriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        do {
            waveformPipeline = try device.makeRenderPipelineState(
                descriptor: waveformDescriptor)
            renderPipeline = try device.makeRenderPipelineState(
                descriptor: renderDescriptor)
            analysisPipeline = try device.makeComputePipelineState(
                function: analysis)
        } catch {
            closeConnection()
            throw AudioRingClientError.pipelineFailed
        }
        guard let analysisBuffer = device.makeBuffer(
            length: 36 * MemoryLayout<UInt32>.stride,
            options: .storageModeShared) else {
            closeConnection()
            throw AudioRingClientError.analysisBufferUnavailable
        }
        self.analysisBuffer = analysisBuffer
    }

    func poll() throws -> AudioViewSnapshot {
        guard mappedAddress != 0, let stateReader else {
            throw AudioRingClientError.serviceNotFound
        }
        let state = try stateReader.read()
        let bytesPerFrame = UInt64(state.channels) * UInt64(MemoryLayout<Float>.stride)
        let mappedFrames = UInt64(mappedSize) / bytesPerFrame
        guard mappedFrames >= UInt64(state.activeRingFrames),
              let rawPointer = UnsafeRawPointer(bitPattern: UInt(mappedAddress)) else {
            throw AudioRingClientError.invalidGeometry
        }

        let floatPointer = rawPointer.assumingMemoryBound(to: Float.self)
        let available = min(state.validHistoryFrames, 4)
        var left: [Float] = []
        var right: [Float] = []
        if available > 0 {
            left.reserveCapacity(Int(available))
            right.reserveCapacity(Int(available))
            for distance in stride(from: Int(available), through: 1, by: -1) {
                let absoluteFrame = state.writeEndFrame - UInt64(distance)
                let frame = absoluteFrame % UInt64(state.activeRingFrames)
                let sampleIndex = Int(frame * UInt64(state.channels))
                left.append(floatPointer[sampleIndex])
                right.append(floatPointer[sampleIndex + 1])
            }
        }

        snapshot = AudioViewSnapshot(
            writeEndFrame: state.writeEndFrame,
            activeRingFrames: state.activeRingFrames,
            channels: state.channels,
            sampleRate: state.sampleRate,
            ioRunning: state.ioRunning,
            epoch: state.epoch,
            validHistoryFrames: state.validHistoryFrames,
            mappedFrames: mappedFrames,
            lastLeftSamples: left,
            lastRightSamples: right)
        renderState.update(snapshot)
        return snapshot
    }

    func cpuValidationBits(snapshot: AudioViewSnapshot,
                           windowFrames: UInt32) -> [UInt32] {
        guard windowFrames > 1,
              snapshot.validHistoryFrames >= windowFrames,
              let rawPointer = UnsafeRawPointer(bitPattern: UInt(mappedAddress)) else {
            return []
        }
        let floats = rawPointer.assumingMemoryBound(to: Float.self)
        let firstFrame = snapshot.writeEndFrame - UInt64(windowFrames)
        var result: [UInt32] = []
        result.reserveCapacity(32)
        for sampleNumber in 0..<16 {
            let delta = UInt64(sampleNumber) * UInt64(windowFrames - 1) / 15
            let frame = (firstFrame + delta) % UInt64(snapshot.activeRingFrames)
            let base = Int(frame * UInt64(snapshot.channels))
            result.append(floats[base].bitPattern)
            result.append(floats[base + 1].bitPattern)
        }
        return result
    }

    private func closeConnection() {
        metalBuffer = nil
        waveformPipeline = nil
        renderPipeline = nil
        analysisPipeline = nil
        analysisBuffer = nil
        metalDevice = nil
        stateReader = nil
        if mappingLifetime != nil {
            // The no-copy MTLBuffer's deallocator retains this mapping lease
            // until GPU and renderer references have all gone away.
            mappingLifetime = nil
            connection = IO_OBJECT_NULL
            mappedAddress = 0
            mappedSize = 0
            return
        }
        if connection != IO_OBJECT_NULL {
            if mappedSize != 0 {
                IOConnectUnmapMemory64(connection, Wire.outputRingMemoryType,
                                       mach_task_self_, mappedAddress)
            }
            IOServiceClose(connection)
        }
        connection = IO_OBJECT_NULL
        mappedAddress = 0
        mappedSize = 0
    }

    private func findService() -> io_service_t {
        let direct = IOServiceGetMatchingService(
            kIOMainPortDefault, IOServiceNameMatching("VirtualAudioDriver"))
        if direct != IO_OBJECT_NULL { return direct }

        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(
            kIOMainPortDefault, IOServiceMatching("IOUserService"),
            &iterator) == KERN_SUCCESS else { return IO_OBJECT_NULL }
        defer { IOObjectRelease(iterator) }

        while case let service = IOIteratorNext(iterator), service != IO_OBJECT_NULL {
            if let name = IORegistryEntryCreateCFProperty(
                service, "IOUserServerName" as CFString, kCFAllocatorDefault, 0)?
                .takeRetainedValue() as? String,
                name.contains("ADKVirtualAudioLab") {
                return service
            }
            IOObjectRelease(service)
        }
        return IO_OBJECT_NULL
    }
}
