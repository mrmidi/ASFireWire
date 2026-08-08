import Foundation
import Testing
@testable import ASFW

@MainActor
struct LogCategoryCatalogTests {
    @Test func decodesDriverCategoriesAndPresetMask() throws {
        let catalog = try #require(
            ASFWDriverConnector.decodeLogCategoryCatalog(makeCatalogData())
        )

        #expect(catalog.categories == [
            ASFWLogCategoryDescriptor(id: 15, name: "Isoch"),
            ASFWLogCategoryDescriptor(id: 17, name: "DirectAudio"),
        ])
        let preset = try #require(catalog.presets.first)
        #expect(preset.name == "Audio Transmit")
        #expect(preset.contains(category: 15))
        #expect(preset.contains(category: 17))
        #expect(!preset.contains(category: 4))
    }

    @Test func rejectsTruncatedCatalog() {
        let truncated = makeCatalogData().dropLast()
        #expect(ASFWDriverConnector.decodeLogCategoryCatalog(Data(truncated)) == nil)
    }

    @Test func rejectsPresetBitsMissingFromCategoryCatalog() {
        var data = makeCatalogData()
        // Preset starts at byte 80. Add Metrics (ID 4), which is not present
        // in this two-category fixture.
        write(UInt32((1 << 15) | (1 << 17) | (1 << 4)), at: 80, into: &data)
        #expect(ASFWDriverConnector.decodeLogCategoryCatalog(data) == nil)
    }

    private func makeCatalogData() -> Data {
        let totalBytes = 16 + (2 * 32) + 32
        var data = Data(repeating: 0, count: totalBytes)
        write(UInt32(1), at: 0, into: &data)
        write(UInt16(2), at: 4, into: &data)
        write(UInt16(1), at: 6, into: &data)
        write(UInt16(32), at: 8, into: &data)
        write(UInt16(32), at: 10, into: &data)
        write(UInt32(totalBytes), at: 12, into: &data)

        writeCategory(id: 15, name: "Isoch", at: 16, into: &data)
        writeCategory(id: 17, name: "DirectAudio", at: 48, into: &data)

        write(UInt32((1 << 15) | (1 << 17)), at: 80, into: &data)
        writeName("Audio Transmit", lengthOffset: 84, bytesOffset: 88, into: &data)
        return data
    }

    private func writeCategory(
        id: UInt8,
        name: String,
        at offset: Int,
        into data: inout Data
    ) {
        data[offset] = id
        writeName(name, lengthOffset: offset + 1, bytesOffset: offset + 4, into: &data)
    }

    private func writeName(
        _ name: String,
        lengthOffset: Int,
        bytesOffset: Int,
        into data: inout Data
    ) {
        let bytes = Array(name.utf8)
        data[lengthOffset] = UInt8(bytes.count)
        data.replaceSubrange(bytesOffset..<bytesOffset + bytes.count, with: bytes)
    }

    private func write<T: FixedWidthInteger>(
        _ value: T,
        at offset: Int,
        into data: inout Data
    ) {
        var littleEndian = value.littleEndian
        withUnsafeBytes(of: &littleEndian) { bytes in
            data.replaceSubrange(offset..<offset + bytes.count, with: bytes)
        }
    }
}
