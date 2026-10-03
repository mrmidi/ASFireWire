// motu_input_meter.swift - per-channel sample peaks of a CoreAudio input device.
//
// Shows what an application actually receives on each input channel, which no
// driver counter can: the driver's RX telemetry counts frames, not sample
// content, and the in-app Audio Analyzer reads only the output ring. Use it to
// map physical inputs to CoreAudio channels (feed one input at a time) and to
// tell "the driver delivers silence" apart from "macOS withholds the input".
//
// STARTS IO on the device. Do not run it during an endurance or fault run.
//
// Run it from Terminal.app. macOS grants microphone access per responsible
// application, Terminal holds that grant, and a tool it launches inherits it;
// a process launched by another host (an agent, an IDE) gets that host's
// decision instead. A denied process receives exact zeros, printed here as
// "-inf" on every channel, so read the first line before concluding that the
// device is silent. A machine booted with amfi_get_out_of_my_way=1 denies the
// grant to every non-Apple process without asking.
//
//   swift tools/motu_input_meter.swift [device-name-substring] [seconds]
//   defaults: "MOTU", 10
//
// Exit codes: 0 success, 1 no matching device or IO failure, 2 bad argument.

import AVFoundation
import AudioToolbox
import CoreAudio
import Foundation

let args = CommandLine.arguments
let needle = args.count > 1 ? args[1] : "MOTU"
let seconds: Double
if args.count > 2 {
    guard let value = Double(args[2]), value > 0 else {
        print("seconds must be a positive number, got \"\(args[2])\"")
        exit(2)
    }
    seconds = value
} else {
    seconds = 10
}

// Peaks at or above this level count as signal in the closing summary.
let signalThresholdDbfs: Float = -60

func deviceName(_ id: AudioDeviceID) -> String? {
    var addr = AudioObjectPropertyAddress(mSelector: kAudioObjectPropertyName,
                                          mScope: kAudioObjectPropertyScopeGlobal,
                                          mElement: kAudioObjectPropertyElementMain)
    var name: Unmanaged<CFString>?
    var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &name) == noErr,
          let value = name?.takeRetainedValue() else { return nil }
    return value as String
}

func findDevice(_ needle: String) -> (AudioDeviceID, String)? {
    var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                          mScope: kAudioObjectPropertyScopeGlobal,
                                          mElement: kAudioObjectPropertyElementMain)
    let system = AudioObjectID(kAudioObjectSystemObject)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(system, &addr, 0, nil, &size) == noErr else { return nil }
    var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
    guard AudioObjectGetPropertyData(system, &addr, 0, nil, &size, &ids) == noErr else { return nil }
    for id in ids {
        if let name = deviceName(id), name.localizedCaseInsensitiveContains(needle) {
            return (id, name)
        }
    }
    return nil
}

func dbfs(_ value: Float) -> String {
    value <= 0 ? "  -inf" : String(format: "%6.1f", 20 * log10(value))
}

func rightAligned(_ text: String, width: Int) -> String {
    String(repeating: " ", count: max(0, width - text.count)) + text
}

let authorization = AVCaptureDevice.authorizationStatus(for: .audio)
let authorizationText: String
switch authorization {
case .authorized: authorizationText = "authorized"
case .denied: authorizationText = "DENIED"
case .restricted: authorizationText = "RESTRICTED"
case .notDetermined: authorizationText = "not determined"
@unknown default: authorizationText = "unknown"
}
print("microphone access for this process: \(authorizationText)")
if authorization != .authorized {
    print("note: without microphone access macOS delivers exact zeros, printed as -inf")
}

guard let (deviceID, name) = findDevice(needle) else {
    print("no CoreAudio device whose name contains \"\(needle)\"")
    exit(1)
}

let engine = AVAudioEngine()
let input = engine.inputNode
guard let unit = input.audioUnit else {
    print("input node has no audio unit")
    exit(1)
}
var device = deviceID
let setStatus = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                     kAudioUnitScope_Global, 0, &device,
                                     UInt32(MemoryLayout<AudioDeviceID>.size))
guard setStatus == noErr else {
    print("selecting \(name) failed: OSStatus \(setStatus)")
    exit(1)
}

let format = input.outputFormat(forBus: 0)
let channels = Int(format.channelCount)
print("device: \(name)  channels: \(channels)  rate: \(Int(format.sampleRate)) Hz  seconds: \(seconds)")
guard channels > 0 else {
    print("device reports no input channels")
    exit(1)
}

// Filled by the tap thread, drained once per second by the main thread.
let lock = NSLock()
var intervalPeaks = [Float](repeating: 0, count: channels)
var intervalFrames = 0

input.installTap(onBus: 0, bufferSize: 4800, format: format) { buffer, _ in
    guard let data = buffer.floatChannelData else { return }
    let count = Int(buffer.frameLength)
    let channelCount = Int(buffer.format.channelCount)
    let interleaved = buffer.format.isInterleaved
    lock.lock()
    for ch in 0..<min(channelCount, intervalPeaks.count) {
        var peak = intervalPeaks[ch]
        for i in 0..<count {
            let magnitude = abs(interleaved ? data[0][i * channelCount + ch] : data[ch][i])
            if magnitude > peak { peak = magnitude }
        }
        intervalPeaks[ch] = peak
    }
    intervalFrames += count
    lock.unlock()
}

do {
    try engine.start()
} catch {
    print("engine start failed: \(error)")
    exit(1)
}

var header = "  t  frames"
for ch in 1...channels { header += " " + rightAligned("ch\(ch)", width: 6) }
print(header)

var runPeaks = [Float](repeating: 0, count: channels)
let start = Date()
var tick = 0
while Date().timeIntervalSince(start) < seconds {
    RunLoop.current.run(until: Date().addingTimeInterval(1))
    tick += 1
    lock.lock()
    let peaks = intervalPeaks
    let frames = intervalFrames
    intervalPeaks = [Float](repeating: 0, count: channels)
    intervalFrames = 0
    lock.unlock()
    var line = String(format: "%3d %7d", tick, frames)
    for (ch, value) in peaks.enumerated() {
        line += " " + dbfs(value)
        runPeaks[ch] = max(runPeaks[ch], value)
    }
    print(line)
}

engine.stop()
input.removeTap(onBus: 0)

let threshold = powf(10, signalThresholdDbfs / 20)
let withSignal = runPeaks.indices.filter { runPeaks[$0] >= threshold }.map { "ch\($0 + 1)" }
print("channels with signal (peak >= \(Int(signalThresholdDbfs)) dBFS): "
      + (withSignal.isEmpty ? "none" : withSignal.joined(separator: " ")))
