// GPU integration check for the production analyzer shader. No audio hardware required.
import Foundation
import Metal
struct Params { var w: UInt64; var ring: UInt32; var channels: UInt32; var channel: UInt32; var rate: UInt32 }
let device = MTLCreateSystemDefaultDevice()!
guard CommandLine.arguments.count == 2 else { fatalError("Usage: swift tools/validate_audio_spectrum.swift /path/to/default.metallib") }
let library = try device.makeLibrary(URL: URL(fileURLWithPath: CommandLine.arguments[1]))
let pipeline = try device.makeComputePipelineState(function: library.makeFunction(name: "asfwSpectrumFFT")!)
let queue = device.makeCommandQueue()!
let ringFrames = 12288, n = 2048
let ring = device.makeBuffer(length: ringFrames * 2 * 4, options: .storageModeShared)!
let out = device.makeBuffer(length: 1025 * 4, options: .storageModeShared)!
let pointer = ring.contents().assumingMemoryBound(to: Float.self)
func run(_ channel: UInt32, _ w: Int, _ left: (Int) -> Float, _ right: (Int) -> Float) -> [Float] {
    for i in 0..<n {
        let frame = (w - n + i) % ringFrames
        pointer[frame * 2] = left(i); pointer[frame * 2 + 1] = right(i)
    }
    var p = Params(w: UInt64(w), ring: UInt32(ringFrames), channels: 2, channel: channel, rate: 48000)
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
