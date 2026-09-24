import Foundation

/// Serializes complete BeBoB mailbox conversations. DM1000 mailbox requests are
/// single-occupancy; a second caller can otherwise consume the first caller's reply.
actor ASFWMCPBeBoBMailboxGate {
    static let shared = ASFWMCPBeBoBMailboxGate()
    private var occupied = false
    private var waiters: [CheckedContinuation<Void, Never>] = []
    private var nextId: UInt16 = 1

    func acquire() async {
        if !occupied {
            occupied = true
            return
        }
        await withCheckedContinuation { waiters.append($0) }
    }

    func release() {
        if !waiters.isEmpty {
            // Ownership passes directly to the resumed waiter. Keep occupied
            // true so a newcomer cannot slip between resume and continuation.
            waiters.removeFirst().resume()
        } else {
            occupied = false
        }
    }

    func commandId() -> UInt16 {
        let result = nextId
        nextId = nextId == .max ? 1 : nextId + 1
        return result
    }
}

enum ASFWMCPBeBoBShellClient {
    static func supports(_ command: String) -> Bool {
        let line = command.trimmingCharacters(in: .whitespacesAndNewlines)
        return !line.isEmpty && line.utf8.count <= 1024 &&
            line.utf8.allSatisfy { $0 >= 0x20 && $0 <= 0x7e }
    }
    private static let hi: UInt16 = 0xffff
    private static let req: UInt32 = 0xc802_1000
    private static let reqbuf: UInt32 = 0xc802_1040
    private static let resp: UInt32 = 0xc802_9000
    private static let respbuf: UInt32 = 0xc802_9040
    private static let version: UInt32 = 1
    private static let responseBackoffNs: [UInt64] = [
        500_000, 500_000, 1_000_000, 1_000_000, 2_000_000, 2_000_000,
        5_000_000, 5_000_000, 10_000_000, 10_000_000, 20_000_000, 20_000_000
    ]

    static func execute(driver: any ASFWDriverControlling, nodeId: UInt32,
                        generation: UInt32, command: String) async -> String? {
        // The shell tool carries one printable ASCII command line. MCP exposes
        // it as a developer write because the shell can alter device settings.
        let normalized = command.trimmingCharacters(in: .whitespacesAndNewlines)
        guard supports(normalized),
              await driver.listNodes().contains(where: {
                  $0.nodeId == nodeId && $0.protocolHints.contains("bebob") &&
                      Self.hex($0.vendorId) == 0x000d6c && Self.hex($0.modelId) == 0x010071
              }) else { return nil }
        await ASFWMCPBeBoBMailboxGate.shared.acquire()
        let result = await executeLocked(driver: driver, nodeId: nodeId,
                                         generation: generation, command: normalized)
        await ASFWMCPBeBoBMailboxGate.shared.release()
        return result
    }

    private static func executeLocked(driver: any ASFWDriverControlling, nodeId: UInt32,
                                      generation: UInt32, command normalized: String) async -> String? {
        // Discard a possibly partial old line and stale output before issuing
        // the command. Firmware accepts one shell character per opcode reliably.
        _ = await writeCharacters("\r\n", driver: driver, node: nodeId, generation: generation)
        _ = await drain(driver: driver, node: nodeId, generation: generation, limit: 24, quietStop: true)
        let line = normalized + "\r\n"
        guard await writeCharacters(line, driver: driver, node: nodeId, generation: generation) else { return nil }
        try? await Task.sleep(nanoseconds: 200_000_000)
        return await drain(driver: driver, node: nodeId, generation: generation, limit: 64, quietStop: false)
    }

    private static func address(_ lo: UInt32, node: UInt32, generation: UInt32) -> ASFWMCPAddress {
        ASFWMCPAddress(nodeId: node, generation: generation, addressHigh: hi, addressLow: lo)
    }

    private static func hex(_ value: String?) -> UInt32? {
        guard let value else { return nil }
        return UInt32(value.hasPrefix("0x") ? String(value.dropFirst(2)) : value, radix: 16)
    }

    private static func aligned(_ bytes: [UInt8]) -> [UInt8] {
        bytes + Array(repeating: 0, count: (4 - bytes.count % 4) % 4)
    }

    private static func write(_ bytes: [UInt8], at lo: UInt32, driver: any ASFWDriverControlling,
                              node: UInt32, generation: UInt32) async -> Bool {
        let data = aligned(bytes)
        let result = await driver.executeWriteBlock(ASFWMCPWriteBlockRequest(
            address: address(lo, node: node, generation: generation), payload: data))
        return result.ok
    }

    private static func read(_ length: Int, at lo: UInt32, driver: any ASFWDriverControlling,
                             node: UInt32, generation: UInt32) async -> [UInt8]? {
        let size = (length + 3) & ~3
        let result = await driver.executeReadBlock(ASFWMCPReadBlockRequest(
            address: address(lo, node: node, generation: generation), length: UInt32(size)))
        guard result.ok, let payload = result.payload, payload.count >= length else { return nil }
        return Array(payload.prefix(length))
    }

    private static func envelope(id: UInt16, opcode: UInt8, operandSize: UInt8, operand: UInt32) -> [UInt8] {
        [1, 0, 0, 0, UInt8(id & 0xff), UInt8(id >> 8), opcode, operandSize,
         UInt8(operand & 0xff), UInt8((operand >> 8) & 0xff),
         UInt8((operand >> 16) & 0xff), UInt8(operand >> 24)]
    }

    private static func mailbox(_ opcode: UInt8, size: UInt8, operand: UInt32,
                                driver: any ASFWDriverControlling, node: UInt32,
                                generation: UInt32) async -> [UInt8]? {
        guard [UInt8(0x07), 0x08, 0x09].contains(opcode) else { return nil }
        let id = await ASFWMCPBeBoBMailboxGate.shared.commandId()
        guard await write(envelope(id: id, opcode: opcode, operandSize: size, operand: operand),
                          at: req, driver: driver, node: node, generation: generation) else { return nil }
        for delay in responseBackoffNs {
            try? await Task.sleep(nanoseconds: delay)
            guard let bytes = await read(20, at: resp, driver: driver, node: node, generation: generation),
                  bytes[0] == 1, bytes[1] == 0, bytes[2] == 0, bytes[3] == 0,
                  bytes[4] == UInt8(id & 0xff), bytes[5] == UInt8(id >> 8),
                  bytes[6] == opcode, opcode != 0x08 || bytes[7] >= 3 else { continue }
            return bytes
        }
        return nil
    }

    private static func writeCharacters(_ text: String, driver: any ASFWDriverControlling,
                                        node: UInt32, generation: UInt32) async -> Bool {
        for byte in text.utf8 {
            guard await write([byte], at: reqbuf, driver: driver, node: node, generation: generation),
                  await mailbox(0x09, size: 1, operand: 1, driver: driver, node: node, generation: generation) != nil else { return false }
        }
        return true
    }

    private static func drain(driver: any ASFWDriverControlling, node: UInt32,
                              generation: UInt32, limit: Int, quietStop: Bool) async -> String? {
        var output: [UInt8] = []
        var quiet = 0
        for _ in 0..<limit {
            guard let reply = await mailbox(0x08, size: 1, operand: 1024,
                                            driver: driver, node: node, generation: generation) else { return nil }
            let available = Int(reply[8]) | Int(reply[9]) << 8 | Int(reply[10]) << 16 | Int(reply[11]) << 24
            if available == 0 {
                quiet += 1
                if quiet >= (quietStop ? 2 : 5) { break }
                try? await Task.sleep(nanoseconds: 40_000_000)
                continue
            }
            quiet = 0
            guard let page = await read(min(available, 1024), at: respbuf,
                                        driver: driver, node: node, generation: generation) else { return nil }
            output += page
            if !quietStop, output.last == 0x3e { break }
        }
        return String(decoding: output, as: UTF8.self)
    }
}
