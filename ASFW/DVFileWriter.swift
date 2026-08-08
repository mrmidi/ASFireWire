import Foundation

actor DVFileWriter {
    private let handle: FileHandle
    private var closed = false

    init(url: URL) throws {
        guard FileManager.default.createFile(atPath: url.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown)
        }
        handle = try FileHandle(forWritingTo: url)
    }

    func write(_ frames: [Data]) throws -> UInt64 {
        guard !closed else { throw CocoaError(.fileNoSuchFile) }
        var bytes: UInt64 = 0
        for frame in frames {
            try handle.write(contentsOf: frame)
            bytes += UInt64(frame.count)
        }
        return bytes
    }

    func close() throws {
        guard !closed else { return }
        try handle.synchronize()
        try handle.close()
        closed = true
    }
}
