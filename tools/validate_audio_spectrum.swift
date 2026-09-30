// GPU integration check for the production analyzer shader. No audio hardware required.
import Foundation
import Metal
struct Params { var w: UInt64; var ring: UInt32; var channels: UInt32; var channel: UInt32; var rate: UInt32; var other: UInt32 = 1; var transform: UInt32 = 0 }
let device = MTLCreateSystemDefaultDevice()!
guard CommandLine.arguments.count == 2 else { fatalError("Usage: swift tools/validate_audio_spectrum.swift /path/to/default.metallib") }
let library = try device.makeLibrary(URL: URL(fileURLWithPath: CommandLine.arguments[1]))
let pipeline = try device.makeComputePipelineState(function: library.makeFunction(name: "asfwSpectrumFFT")!)
let queue = device.makeCommandQueue()!
let ringFrames = 12288, n = 2048
let ring = device.makeBuffer(length: ringFrames * 2 * 4, options: .storageModeShared)!
let out = device.makeBuffer(length: 1025 * 4, options: .storageModeShared)!
let pointer = ring.contents().assumingMemoryBound(to: Float.self)
func run(_ channel: UInt32, _ w: Int, _ left: (Int) -> Float, _ right: (Int) -> Float, transform: UInt32 = 0) -> [Float] {
    for i in 0..<n {
        let frame = (w - n + i) % ringFrames
        pointer[frame * 2] = left(i); pointer[frame * 2 + 1] = right(i)
    }
    var p = Params(w: UInt64(w), ring: UInt32(ringFrames), channels: 2, channel: channel, rate: 48000, transform: transform)
    let command = queue.makeCommandBuffer()!
    let encoder = command.makeComputeCommandEncoder()!
    encoder.setComputePipelineState(pipeline)
    encoder.setBuffer(ring, offset: 0, index: 0); encoder.setBuffer(out, offset: 0, index: 1)
    encoder.setBytes(&p, length: MemoryLayout<Params>.stride, index: 2)
    encoder.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
    encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
    precondition(command.status == .completed, "GPU FFT failed: \(String(describing: command.error))")
    return Array(UnsafeBufferPointer(start: out.contents().assumingMemoryBound(to: Float.self), count: 1025))
}
let left: (Int) -> Float = { Float(sin(2 * Double.pi * 43 * Double($0) / Double(n))) }
let right: (Int) -> Float = { 0.25 * Float(sin(2 * Double.pi * 97 * Double($0) / Double(n))) }
for w in [12000, 12300, 25000] {
    let a = run(0, w, left, right), b = run(1, w, left, right)
    precondition(abs(a[43] - 1) < 0.0001 && abs(b[97] - 0.25) < 0.0001)
    precondition(a[97] < 0.0001 && b[43] < 0.0001)
    print("W=\(w): left \(20*log10(a[43])) dBFS; right \(20*log10(b[97])) dBFS; channels isolated")
}
let dc = run(0, 12300, { _ in 0.3 }, { _ in 0 })
precondition(abs(dc[0] - 0.3) < 0.0001)
let nyquist = run(0, 12300, { $0 % 2 == 0 ? 0.5 : -0.5 }, { _ in 0 })
precondition(abs(nyquist[1024] - 0.5) < 0.0001)
let silence = run(0, 12300, { _ in 0 }, { _ in 0 })
precondition(silence.allSatisfy { $0 == 0 })
print("GPU FFT: DC / Nyquist normalization and silence passed")

let mid = run(0, 12300, left, left, transform: 1)
let side = run(0, 12300, left, left, transform: 2)
precondition(abs(mid[43] - sqrt(2)) < 0.0001 && side.allSatisfy { $0 == 0 })
let inverse: (Int) -> Float = { -left($0) }
let cancelledMid = run(0, 12300, left, inverse, transform: 1)
let wideSide = run(0, 12300, left, inverse, transform: 2)
precondition(cancelledMid.allSatisfy { $0 == 0 } && abs(wideSide[43] - sqrt(2)) < 0.0001)
print("M/S: mono +3.0103 dBFS / zero Side; opposite phase zero Mid / +3.0103 dBFS Side passed")

struct SmoothParams { var alpha: Float; var elapsed: Float; var reset: UInt32; var unused: UInt32 = 0 }
let smoothPipeline = try device.makeComputePipelineState(function: library.makeFunction(name: "asfwSpectrumSmooth")!)
let history = device.makeBuffer(length: 1025 * 16, options: .storageModeShared)!
let average = device.makeBuffer(length: 1025 * 4, options: .storageModeShared)!
let peaks = device.makeBuffer(length: 1025 * 4, options: .storageModeShared)!
func smooth(_ amplitude: Float, alpha: Float, elapsed: Float, reset: UInt32) -> (Float, Float) {
    out.contents().assumingMemoryBound(to: Float.self).update(repeating: amplitude, count: 1025)
    var p = SmoothParams(alpha: alpha, elapsed: elapsed, reset: reset)
    let command = queue.makeCommandBuffer()!, encoder = command.makeComputeCommandEncoder()!
    encoder.setComputePipelineState(smoothPipeline)
    for (i, buffer) in [out, history, average, peaks].enumerated() { encoder.setBuffer(buffer, offset: 0, index: i) }
    encoder.setBytes(&p, length: MemoryLayout<SmoothParams>.stride, index: 4)
    encoder.dispatchThreads(MTLSize(width: 1025, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 256, height: 1, depth: 1))
    encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
    precondition(command.status == .completed)
    return (average.contents().assumingMemoryBound(to: Float.self)[43], peaks.contents().assumingMemoryBound(to: Float.self)[43])
}
let reset = smooth(1, alpha: 0.5, elapsed: 0, reset: 1)
precondition(reset.0 == 1 && reset.1 == 1)
let decay = smooth(0, alpha: 0.5, elapsed: 1, reset: 0)
precondition(abs(decay.0 - sqrt(0.5)) < 0.0001 && decay.1 == 1)
let held = smooth(0, alpha: 0.5, elapsed: 1, reset: 0)
precondition(held.1 == 1)
let falling = smooth(0, alpha: 0.5, elapsed: 0.1, reset: 0)
precondition(abs(falling.1 - pow(10, -1.2/20)) < 0.0001)
let cleared = smooth(0, alpha: 0.5, elapsed: 0, reset: 1)
precondition(cleared.0 == 0 && cleared.1 == 0)
print("Power averaging / peak hold / decay / reset passed")

struct ObserverParams { var w: UInt64; var ring: UInt32; var channels: UInt32; var count: UInt32; var left: UInt32; var right: UInt32 }
let meterPipeline = try device.makeComputePipelineState(function: library.makeFunction(name: "asfwAnalyzeRing")!)
let meterOutput = device.makeBuffer(length: 36 * 4, options: .storageModeShared)!
func meters(_ l: (Int) -> Float, _ r: (Int) -> Float) -> [Float] {
    _ = run(0, 12300, l, r)
    var p = ObserverParams(w: 12300, ring: 12288, channels: 2, count: 2048, left: 0, right: 1)
    let command = queue.makeCommandBuffer()!, encoder = command.makeComputeCommandEncoder()!
    encoder.setComputePipelineState(meterPipeline)
    encoder.setBuffer(ring, offset: 0, index: 0); encoder.setBuffer(meterOutput, offset: 0, index: 1)
    encoder.setBytes(&p, length: MemoryLayout<ObserverParams>.stride, index: 2)
    encoder.dispatchThreads(MTLSize(width: 1, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
    encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
    precondition(command.status == .completed)
    return Array(UnsafeBufferPointer(start: meterOutput.contents().assumingMemoryBound(to: Float.self), count: 12))
}
let monoMeters = meters(left, left)
precondition(abs(monoMeters[2] - 1) < 0.0001 && abs(monoMeters[6] - sqrt(0.5)) < 0.0001)
precondition(abs(monoMeters[8] - 1) < 0.0001 && monoMeters[9] == 0 && monoMeters[10] == 0 && monoMeters[11] == 0)
let antiMeters = meters(left, inverse)
precondition(abs(antiMeters[2] + 1) < 0.0001 && antiMeters[8] == 0 && abs(antiMeters[11] - 1) < 0.0001)
let leftOnly = meters(left, { _ in 0 })
precondition(abs(leftOnly[10] + 1) < 0.0001 && abs(leftOnly[11] - 0.5) < 0.0001)
let silentMeters = meters({ _ in 0 }, { _ in 0 })
precondition(silentMeters.enumerated().allSatisfy { $0.offset == 3 || $0.element == 0 })
print("GPU RMS / M/S / correlation / balance / Side energy / silence passed")
