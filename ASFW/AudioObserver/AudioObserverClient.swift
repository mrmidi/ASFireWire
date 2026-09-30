import Foundation
import IOKit
import Metal

struct AudioObserverSnapshot: Sendable {
    var writeEndFrame: UInt64 = 0
    var oldestValidFrame: UInt64 = 0
    var sessionEpoch: UInt64 = 0
    var discontinuityEpoch: UInt64 = 0
    var memoryGeneration: UInt64 = 0
    var activeRingFrames: UInt32 = 0
    var channels: UInt32 = 0
    var sampleRateHz: UInt32 = 0
    var mappedFrames: UInt64 = 0
    var validHistoryFrames: UInt32 = 0
    var ioRunning = false
}

struct AudioObserverMetrics: Sendable {
    var leftPeak: Float = 0
    var rightPeak: Float = 0
    var correlation: Float = 0
    var cpuEncodeMilliseconds: Double?
    var scheduledToStartMilliseconds: Double?
    var gpuMilliseconds: Double?
    var completionMilliseconds: Double?
    var sampleAgeMilliseconds: Double?
    var overwriteMarginMilliseconds: Double?
    var windowsRendered: UInt64 = 0
    var windowsCrossingWrap: UInt64 = 0
    var unsafeWindows: UInt64 = 0
    var inFlight = 0
}

struct AudioObserverWireState: Sendable {
    let writeEndFrame: UInt64
    let oldestValidFrame: UInt64
    let sessionEpoch: UInt64
    let discontinuityEpoch: UInt64
    let memoryGeneration: UInt64
    let mappedGeneration: UInt64
    let activeRingFrames: UInt32
    let channels: UInt32
    let sampleRateHz: UInt32
}

private final class AudioObserverMappingLifetime: @unchecked Sendable {
    private let connection: io_connect_t
    private let address: mach_vm_address_t

    init(connection: io_connect_t, address: mach_vm_address_t) {
        self.connection = connection
        self.address = address
    }

    deinit {
        IOConnectUnmapMemory64(connection, 2, mach_task_self_, address)
        IOServiceClose(connection)
    }
}

final class AudioObserverStateReader: @unchecked Sendable {
    private static let selector: UInt32 = 67
    private let connection: io_connect_t
    private let guid: UInt64
    private let lock = NSLock()

    init(connection: io_connect_t, guid: UInt64) {
        self.connection = connection
        self.guid = guid
    }

    func read() throws -> AudioObserverWireState {
        lock.lock()
        defer { lock.unlock() }
        var values = [UInt64](repeating: 0, count: 9)
        var outputCount: UInt32 = UInt32(values.count)
        let result = values.withUnsafeMutableBufferPointer { buffer in
            IOConnectCallScalarMethod(connection, Self.selector, nil, 0,
                                      buffer.baseAddress, &outputCount)
        }
        guard result == KERN_SUCCESS, outputCount == 9 else {
            throw AudioObserverError.stateQuery(result)
        }
        let state = AudioObserverWireState(
            writeEndFrame: values[0],
            oldestValidFrame: values[1],
            sessionEpoch: values[2],
            discontinuityEpoch: values[3],
            memoryGeneration: values[4],
            mappedGeneration: values[5],
            activeRingFrames: UInt32(truncatingIfNeeded: values[6]),
            channels: UInt32(truncatingIfNeeded: values[7]),
            sampleRateHz: UInt32(truncatingIfNeeded: values[8]))
        guard state.activeRingFrames > 0, state.channels >= 2,
              state.sampleRateHz > 0 else {
            throw AudioObserverError.invalidGeometry
        }
        return state
    }
}

enum AudioObserverError: LocalizedError {
    case serviceUnavailable
    case openFailed(kern_return_t)
    case selectionFailed(kern_return_t)
    case mappingFailed(kern_return_t)
    case stateQuery(kern_return_t)
    case invalidGeometry
    case memoryGenerationChanged
    case zeroCopyImportFailed
    case metalUnavailable
    case shaderUnavailable
    case pipelineFailed

    var errorDescription: String? {
        switch self {
        case .serviceUnavailable:
            return "ASFWDriver is not available. Start the driver and select an ASFW Core Audio device."
        case .openFailed(let result):
            return String(format: "Opening the ASFW audio observer failed (0x%08x).", result)
        case .selectionFailed(let result):
            return String(format: "Selecting this audio endpoint failed (0x%08x).", result)
        case .mappingFailed(let result):
            return String(format: "Mapping the ASFW output ring failed (0x%08x). The audio graph may not be ready yet.", result)
        case .stateQuery(let result):
            return String(format: "Reading ASFW output-ring state failed (0x%08x).", result)
        case .invalidGeometry:
            return "The ASFW output ring returned invalid geometry."
        case .memoryGenerationChanged:
            return "The audio buffer changed during reconfiguration; reconnecting the observer."
        case .zeroCopyImportFailed:
            return "ZERO-COPY IMPORT FAILED: Metal rejected the mapped ASFW output ring."
        case .metalUnavailable:
            return "No Metal device is available."
        case .shaderUnavailable:
            return "The ASFW audio observer Metal functions are missing."
        case .pipelineFailed:
            return "Metal could not create the ASFW audio observer pipelines."
        }
    }
}

final class AudioObserverRenderState: @unchecked Sendable {
    private let lock = NSLock()
    private var snapshot = AudioObserverSnapshot()

    func update(_ value: AudioObserverSnapshot) {
        lock.lock()
        snapshot = value
        lock.unlock()
    }

    func read() -> AudioObserverSnapshot {
        lock.lock()
        defer { lock.unlock() }
        return snapshot
    }
}

final class AudioObserverMetricsState: @unchecked Sendable {
    private let lock = NSLock()
    private var value = AudioObserverMetrics()

    func read() -> AudioObserverMetrics {
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

    func completed(leftPeak: Float,
                   rightPeak: Float,
                   correlation: Float,
                   cpuEncodeMilliseconds: Double,
                   scheduledToStartMilliseconds: Double?,
                   gpuMilliseconds: Double?,
                   completionMilliseconds: Double,
                   sampleAgeMilliseconds: Double?,
                   overwriteMarginMilliseconds: Double?,
                   safe: Bool) {
        lock.lock()
        value.inFlight = max(0, value.inFlight - 1)
        value.windowsRendered += 1
        value.leftPeak = leftPeak
        value.rightPeak = rightPeak
        value.correlation = correlation
        value.cpuEncodeMilliseconds = cpuEncodeMilliseconds
        value.scheduledToStartMilliseconds = scheduledToStartMilliseconds
        value.gpuMilliseconds = gpuMilliseconds
        value.completionMilliseconds = completionMilliseconds
        value.sampleAgeMilliseconds = sampleAgeMilliseconds
        value.overwriteMarginMilliseconds = overwriteMarginMilliseconds
        if !safe { value.unsafeWindows += 1 }
        lock.unlock()
    }
}

@MainActor
final class ASFWAudioObserverClient {
    private let guid: UInt64
    private var connection: io_connect_t = IO_OBJECT_NULL
    private var mappedAddress: mach_vm_address_t = 0
    private var mappedSize: mach_vm_size_t = 0
    private var mappingLifetime: AudioObserverMappingLifetime?
    private(set) var stateReader: AudioObserverStateReader?
    private(set) var snapshot = AudioObserverSnapshot()

    let renderState = AudioObserverRenderState()
    let metrics = AudioObserverMetricsState()
    private(set) var metalDevice: MTLDevice?
    private(set) var ringBuffer: MTLBuffer?
    private(set) var phaseRenderPipeline: MTLRenderPipelineState?
    private(set) var waveformRenderPipeline: MTLRenderPipelineState?
    private(set) var analysisPipeline: MTLComputePipelineState?
    private(set) var analysisBuffer: MTLBuffer?

    init(guid: UInt64) {
        self.guid = guid
    }

    func open() throws {
        guard connection == IO_OBJECT_NULL else { return }
        let service = IOServiceGetMatchingService(
            kIOMainPortDefault, IOServiceNameMatching("ASFWDriver"))
        guard service != IO_OBJECT_NULL else {
            throw AudioObserverError.serviceUnavailable
        }
        defer { IOObjectRelease(service) }

        let openResult = IOServiceOpen(service, mach_task_self_, 0, &connection)
        guard openResult == KERN_SUCCESS, connection != IO_OBJECT_NULL else {
            connection = IO_OBJECT_NULL
            throw AudioObserverError.openFailed(openResult)
        }

        let input = [guid]
        let selectResult = input.withUnsafeBufferPointer { buffer in
            IOConnectCallScalarMethod(connection, 66, buffer.baseAddress, 1,
                                      nil, nil)
        }
        guard selectResult == KERN_SUCCESS else {
            closeConnection()
            throw AudioObserverError.selectionFailed(selectResult)
        }

        stateReader = AudioObserverStateReader(connection: connection, guid: guid)
        var address: mach_vm_address_t = 0
        var length: mach_vm_size_t = 0
        let mapResult = IOConnectMapMemory64(
            connection, 2, mach_task_self_, &address, &length,
            UInt32(kIOMapAnywhere | kIOMapDefaultCache))
        guard mapResult == KERN_SUCCESS else {
            closeConnection()
            throw AudioObserverError.mappingFailed(mapResult)
        }
        guard address % UInt64(vm_page_size) == 0, length > 0,
              length % UInt64(vm_page_size) == 0, length <= UInt64(Int.max),
              let rawPointer = UnsafeMutableRawPointer(bitPattern: UInt(address)) else {
            closeConnection(mappedAddress: address, mappedSize: length)
            throw AudioObserverError.invalidGeometry
        }
        mappedAddress = address
        mappedSize = length
        let lifetime = AudioObserverMappingLifetime(connection: connection,
                                                    address: address)
        mappingLifetime = lifetime

        guard let initialState = try? stateReader?.read() else {
            closeConnection()
            throw AudioObserverError.invalidGeometry
        }
        guard initialState.memoryGeneration == initialState.mappedGeneration else {
            closeConnection()
            throw AudioObserverError.memoryGenerationChanged
        }
        let bytesPerFrame = UInt64(initialState.channels) * UInt64(MemoryLayout<Float>.stride)
        let mappedFrames = UInt64(length) / bytesPerFrame
        guard mappedFrames >= UInt64(initialState.activeRingFrames) else {
            closeConnection()
            throw AudioObserverError.invalidGeometry
        }

        guard let device = MTLCreateSystemDefaultDevice() else {
            closeConnection()
            throw AudioObserverError.metalUnavailable
        }
        metalDevice = device
        guard let ring = device.makeBuffer(
            bytesNoCopy: rawPointer,
            length: Int(length),
            options: .storageModeShared,
            deallocator: { [lifetime] _, _ in withExtendedLifetime(lifetime) {} }) else {
            closeConnection()
            throw AudioObserverError.zeroCopyImportFailed
        }
        ringBuffer = ring

        guard let library = device.makeDefaultLibrary(),
              let phaseVertex = library.makeFunction(name: "asfwPhaseVertex"),
              let waveformVertex = library.makeFunction(name: "asfwWaveformVertex"),
              let fragment = library.makeFunction(name: "asfwAudioFragment"),
              let analysis = library.makeFunction(name: "asfwAnalyzeRing") else {
            closeConnection()
            throw AudioObserverError.shaderUnavailable
        }
        let phaseDescriptor = Self.renderDescriptor(vertex: phaseVertex,
                                                    fragment: fragment)
        let waveformDescriptor = Self.renderDescriptor(vertex: waveformVertex,
                                                       fragment: fragment)
        do {
            phaseRenderPipeline = try device.makeRenderPipelineState(descriptor: phaseDescriptor)
            waveformRenderPipeline = try device.makeRenderPipelineState(descriptor: waveformDescriptor)
            analysisPipeline = try device.makeComputePipelineState(function: analysis)
        } catch {
            closeConnection()
            throw AudioObserverError.pipelineFailed
        }
        guard let analysisBuffer = device.makeBuffer(
            length: 36 * MemoryLayout<UInt32>.stride,
            options: .storageModeShared) else {
            closeConnection()
            throw AudioObserverError.pipelineFailed
        }
        self.analysisBuffer = analysisBuffer
        snapshot = Self.makeSnapshot(initialState, mappedFrames: mappedFrames)
        renderState.update(snapshot)
    }

    func poll() throws -> AudioObserverSnapshot {
        guard mappedAddress != 0, let stateReader else {
            throw AudioObserverError.serviceUnavailable
        }
        let state = try stateReader.read()
        guard state.memoryGeneration == state.mappedGeneration else {
            throw AudioObserverError.memoryGenerationChanged
        }
        let bytesPerFrame = UInt64(state.channels) * UInt64(MemoryLayout<Float>.stride)
        let mappedFrames = UInt64(mappedSize) / bytesPerFrame
        guard mappedFrames >= UInt64(state.activeRingFrames) else {
            throw AudioObserverError.invalidGeometry
        }
        snapshot = Self.makeSnapshot(state, mappedFrames: mappedFrames)
        renderState.update(snapshot)
        return snapshot
    }

    func close() {
        closeConnection()
    }

    private static func makeSnapshot(_ state: AudioObserverWireState,
                                     mappedFrames: UInt64) -> AudioObserverSnapshot {
        let valid = state.writeEndFrame >= state.oldestValidFrame
            ? min(UInt64(state.activeRingFrames), state.writeEndFrame - state.oldestValidFrame)
            : 0
        return AudioObserverSnapshot(
            writeEndFrame: state.writeEndFrame,
            oldestValidFrame: state.oldestValidFrame,
            sessionEpoch: state.sessionEpoch,
            discontinuityEpoch: state.discontinuityEpoch,
            memoryGeneration: state.memoryGeneration,
            activeRingFrames: state.activeRingFrames,
            channels: state.channels,
            sampleRateHz: state.sampleRateHz,
            mappedFrames: mappedFrames,
            validHistoryFrames: UInt32(valid),
            ioRunning: valid > 0)
    }

    private static func renderDescriptor(vertex: MTLFunction,
                                         fragment: MTLFunction) -> MTLRenderPipelineDescriptor {
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex
        descriptor.fragmentFunction = fragment
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        return descriptor
    }

    private func closeConnection(mappedAddress address: mach_vm_address_t? = nil,
                                 mappedSize size: mach_vm_size_t? = nil) {
        ringBuffer = nil
        phaseRenderPipeline = nil
        waveformRenderPipeline = nil
        analysisPipeline = nil
        analysisBuffer = nil
        metalDevice = nil
        stateReader = nil
        if mappingLifetime != nil {
            mappingLifetime = nil
            connection = IO_OBJECT_NULL
            mappedAddress = 0
            mappedSize = 0
            return
        }
        let address = address ?? mappedAddress
        let size = size ?? mappedSize
        if connection != IO_OBJECT_NULL {
            if size != 0, address != 0 {
                IOConnectUnmapMemory64(connection, 2, mach_task_self_, address)
            }
            IOServiceClose(connection)
        }
        connection = IO_OBJECT_NULL
        mappedAddress = 0
        mappedSize = 0
    }
}

extension ASFWAudioObserverClient {
    static func guid(fromDeviceUID uid: String) -> UInt64? {
        guard uid.hasPrefix("ASFW-") else { return nil }
        return UInt64(uid.dropFirst(5), radix: 16)
    }
}
