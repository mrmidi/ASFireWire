import Foundation

@main
struct RomParserUnitTests {
    static func main() {
        var failures = 0

        func assert(_ condition: @autoclosure () -> Bool, _ message: String) {
            if !condition() {
                failures += 1
                fputs("TEST FAIL: \(message)\n", stderr)
            }
        }

        do {
            // Minimal synthetic ROM: 4 quadlet bus-info + empty root directory
            var rom = Data()
            rom.append(Data([0x04, 0x00, 0x00, 0x00])) // header: 4 quadlets businfo
            rom.append(Data([0x31, 0x33, 0x39, 0x34])) // '1394'
            rom.append(Data([0x80, 0x00, 0x00, 0x00])) // meta1: irmc=1
            rom.append(Data([0x00, 0x00, 0xab, 0xcd])) // vendor=0x0000ab, chip hi=0xcd 
            rom.append(Data([0x01, 0x23, 0x45, 0x67])) // chip low
            rom.append(Data([0x00, 0x00, 0x00, 0x00])) // empty root dir

            let tmp = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("rom-unit.bin")
            try rom.write(to: tmp)
            let tree = try RomParser.parse(fileURL: tmp)
            assert(tree.busInfo.irmc == 1, "irmc should be 1")
            assert(tree.busInfo.nodeVendorID == 0x0000ab, "vendor id parse")
            assert(tree.busInfo.chipID == 0xcd01234567, "chip id parse")
            assert(tree.rootDirectory.isEmpty, "empty root dir")
        } catch {
            failures += 1
            fputs("Exception in minimal test: \(error)\n", stderr)
        }

        do {
            // Directory with one leaf: root dir [length=1 entry], entry points to leaf at offset 0x01
            var rom = Data()
            rom.append(Data([0x04, 0x00, 0x00, 0x00])) // 4 quadlets bus-info
            rom.append(Data([0x31, 0x33, 0x39, 0x34]))
            rom.append(Data([0x00, 0x00, 0x00, 0x00]))
            rom.append(Data([0x00, 0x00, 0x00, 0x00]))
            rom.append(Data([0x00, 0x00, 0x00, 0x00]))
            // root dir header: 1 quadlet of entries
            rom.append(Data([0x00, 0x01, 0x00, 0x00]))
            // entry: type=leaf(0x02), key=0x00, value=0x000001
            // dir base + 4 (header) + value*4 = 4 + 4 = 8 -> leaf header
            rom.append(Data([0x80, 0x00, 0x00, 0x01]))
            // leaf at that offset: [len=1 quadlet][data=0xaa 0xbb 0xcc 0xdd]
            rom.append(Data([0x00, 0x01, 0x00, 0x00]))
            rom.append(Data([0xaa, 0xbb, 0xcc, 0xdd]))

            let tmp = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("rom-leaf.bin")
            try rom.write(to: tmp)
            let tree = try RomParser.parse(fileURL: tmp)
            assert(tree.rootDirectory.count == 1, "one root entry")
            switch tree.rootDirectory[0].value {
            case .leafData(let payload):
                assert(payload == Data([0xaa, 0xbb, 0xcc, 0xdd]), "leaf payload matches (got: \(Array(payload)))")
            default:
                failures += 1
                fputs("Expected raw leaf payload entry\n", stderr)
            }
        } catch {
            failures += 1
            fputs("Exception in leaf test: \(error)\n", stderr)
        }

        if failures > 0 { exit(1) }
        print("All tests passed")
    }
}
