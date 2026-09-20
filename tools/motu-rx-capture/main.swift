import Foundation
import IOKit

// Selector 64: scalar inputs [full 64-bit GUID, stream index, command].
// Commands: 0 arm, 1 disarm, 2 freeze and export UTF-8 text. No wire writes.
func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}
let arguments = CommandLine.arguments
if arguments.count < 3 || arguments.count > 4 {
    fail("Usage: motu-rx-capture arm|stop|dump GUID_HEX [stream=0]")
}
let commands: [String: UInt64] = ["arm": 0, "stop": 1, "dump": 2]
guard let command = commands[arguments[1]],
      let guid = UInt64(arguments[2].replacingOccurrences(of: "0x", with: ""), radix: 16),
      guid != 0,
      let stream = UInt64(arguments.count == 4 ? arguments[3] : "0"), stream < 4 else {
    fail("Invalid command, GUID, or stream index")
}
var iterator: io_iterator_t = 0
let matched = IOServiceGetMatchingServices(kIOMainPortDefault,
    IOServiceMatching("net_mrmidi_ASFW_ASFWDriver"), &iterator)
guard matched == KERN_SUCCESS else { fail("Cannot enumerate ASFWDriver: \(matched)") }
defer { IOObjectRelease(iterator) }
var lastError: kern_return_t = kIOReturnNotFound
while true {
    let service = IOIteratorNext(iterator)
    if service == 0 { break }
    defer { IOObjectRelease(service) }
    var connection: io_connect_t = 0
    let opened = IOServiceOpen(service, mach_task_self_, 0, &connection)
    if opened != KERN_SUCCESS { lastError = opened; continue }
    defer { IOServiceClose(connection) }
    let inputs = [guid, stream, command]
    var output = [UInt8](repeating: 0, count: 131072)
    var outputSize = command == 2 ? output.count : 0
    let result = inputs.withUnsafeBufferPointer { input in
        output.withUnsafeMutableBytes { buffer in
            IOConnectCallMethod(connection, 64, input.baseAddress, UInt32(input.count),
                nil, 0, nil, nil, command == 2 ? buffer.baseAddress : nil,
                &outputSize)
        }
    }
    if result != KERN_SUCCESS { lastError = result; continue }
    if command == 2 {
        FileHandle.standardOutput.write(Data(output.prefix(outputSize)))
    } else {
        print(command == 0 ? "Armed: next 64 RX packets; use dump to export." : "Capture stopped.")
    }
    exit(0)
}
fail(String(format: "Capture command failed: 0x%08x. Busy means retry; unsupported may mean an older driver or non-MOTU endpoint.", UInt32(bitPattern: lastError)))
