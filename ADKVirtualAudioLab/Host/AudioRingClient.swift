import Foundation
import IOKit
import Metal

struct AudioViewSnapshot {
    var writeEndFrame: UInt64 = 0
    var activeRingFrames: UInt32 = 0
    var channels: UInt32 = 0
    var sampleRate: UInt32 = 0
    var ioRunning = false
    var mappedFrames: UInt64 = 0
    var lastSamples: [Float] = []
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
            return "The waveform Metal functions were not found in the app library."
        case .pipelineFailed:
            return "Metal could not create the waveform render pipeline."
        case .zeroCopyImportFailed:
            return "ZERO-COPY IMPORT FAILED: Metal rejected the mapped ADK output ring."
        }
    }
}

@MainActor
final class AudioRingClient {
    private enum Wire {
        static let userClientType: UInt32 = 0x4C44_4247 // 'LDBG'
        static let outputRingMemoryType: UInt32 = 0
        static let getAudioViewStateSelector: UInt32 = 1
        static let stateScalarCount = 5
    }

    private var connection: io_connect_t = IO_OBJECT_NULL
    private var mappedAddress: mach_vm_address_t = 0
    private var mappedSize: mach_vm_size_t = 0
    private var mappingLifetime: AudioRingMappingLifetime?

    private(set) var snapshot = AudioViewSnapshot()
    let renderState = WaveformRenderState()
    private(set) var metalDevice: MTLDevice?
    private(set) var metalBuffer: MTLBuffer?
    private(set) var renderPipeline: MTLRenderPipelineState?

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

        guard let device = MTLCreateSystemDefaultDevice() else {
            closeConnection()
            throw AudioRingClientError.metalUnavailable
        }
        metalDevice = device

        // This intentionally imports the IOConnect mapping itself. There is
        // no PCM-copy fallback: acceptance is the experiment's key result.
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
              let vertex = library.makeFunction(name: "waveformVertex"),
              let fragment = library.makeFunction(name: "waveformFragment") else {
            closeConnection()
            throw AudioRingClientError.shaderUnavailable
        }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex
        descriptor.fragmentFunction = fragment
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        do {
            renderPipeline = try device.makeRenderPipelineState(descriptor: descriptor)
        } catch {
            closeConnection()
            throw AudioRingClientError.pipelineFailed
        }
    }

    func poll() throws -> AudioViewSnapshot {
        guard connection != IO_OBJECT_NULL, mappedAddress != 0 else {
            throw AudioRingClientError.serviceNotFound
        }

        var values = [UInt64](repeating: 0, count: Wire.stateScalarCount)
        var outputCount = UInt32(values.count)
        let result = values.withUnsafeMutableBufferPointer { buffer in
            IOConnectCallScalarMethod(
                connection, Wire.getAudioViewStateSelector,
                nil, 0, buffer.baseAddress, &outputCount)
        }
        guard result == KERN_SUCCESS, outputCount == Wire.stateScalarCount else {
            throw AudioRingClientError.stateQueryFailed(result)
        }

        let writeEndFrame = values[0]
        let activeRingFrames = UInt32(truncatingIfNeeded: values[1])
        let channels = UInt32(truncatingIfNeeded: values[2])
        let sampleRate = UInt32(truncatingIfNeeded: values[3])
        guard channels > 0, activeRingFrames > 0, sampleRate > 0 else {
            throw AudioRingClientError.invalidGeometry
        }

        let bytesPerFrame = UInt64(channels) * UInt64(MemoryLayout<Float>.stride)
        let mappedFrames = UInt64(mappedSize) / bytesPerFrame
        guard mappedFrames >= UInt64(activeRingFrames),
              let rawPointer = UnsafeRawPointer(bitPattern: UInt(mappedAddress)) else {
            throw AudioRingClientError.invalidGeometry
        }

        let floatPointer = rawPointer.assumingMemoryBound(to: Float.self)
        var samples: [Float] = []
        if writeEndFrame >= 4 {
            samples.reserveCapacity(4)
            for distance in stride(from: 4, through: 1, by: -1) {
                let absoluteFrame = writeEndFrame - UInt64(distance)
                let frame = absoluteFrame % UInt64(activeRingFrames)
                samples.append(floatPointer[
                    Int(frame * UInt64(channels))])
            }
        }

        snapshot = AudioViewSnapshot(
            writeEndFrame: writeEndFrame,
            activeRingFrames: activeRingFrames,
            channels: channels,
            sampleRate: sampleRate,
            ioRunning: values[4] != 0,
            mappedFrames: mappedFrames,
            lastSamples: samples)
        renderState.update(snapshot)
        return snapshot
    }

    private func closeConnection() {
        metalBuffer = nil
        renderPipeline = nil
        metalDevice = nil
        if mappingLifetime != nil {
            // Metal's no-copy deallocator retains this lease until every
            // MTLBuffer reference is gone; the lease then unmaps and closes.
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
