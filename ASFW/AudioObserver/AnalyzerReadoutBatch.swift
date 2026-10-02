import AppKit
import Metal

/// Immutable geometry survives draws until text, layout or atlas changes.
/// Each font/colour group shares one draw; all groups share one vertex buffer.
final class AnalyzerReadoutBatch {
    struct Item {
        let key: Int
        let text: String
        let rect: CGRect
        let atlas: AnalyzerGlyphAtlas
        let alignment: AnalyzerTextAlignment
        let tone: AnalyzerTextSpec.Tone
    }
    struct Draw {
        let texture: MTLTexture
        let tone: AnalyzerTextSpec.Tone
        let start: Int
        let count: Int
    }
    private struct Entry {
        let item: Item
        let vertices: [AnalyzerGlyphVertex]
        func matches(_ other: Item) -> Bool {
            item.text == other.text && item.rect == other.rect && item.atlas === other.atlas &&
                item.alignment == other.alignment && item.tone == other.tone
        }
    }
    private var entries: [Int: Entry] = [:]
    private var order: [Int] = []
    private(set) var buffer: MTLBuffer?
    private(set) var draws: [Draw] = []
    private(set) var rebuilds = 0

    func update(_ items: [Item], device: MTLDevice) {
        var changed = items.count != order.count
        for (index, item) in items.enumerated() {
            if index >= order.count || order[index] != item.key { changed = true }
            if entries[item.key]?.matches(item) != true {
                entries[item.key] = Entry(item: item,
                    vertices: item.atlas.vertices(for: item.text, in: item.rect, alignment: item.alignment))
                changed = true
            }
        }
        guard changed else { return }
        order = items.map(\.key)
        entries = entries.filter { order.contains($0.key) }
        var vertices: [AnalyzerGlyphVertex] = []
        var nextDraws: [Draw] = []
        // Six font styles and two colours at most, independent of the readout count.
        var pending = items
        while let first = pending.first {
            let group = pending.filter { $0.atlas === first.atlas && $0.tone == first.tone }
            pending.removeAll { $0.atlas === first.atlas && $0.tone == first.tone }
            guard let texture = first.atlas.texture else { continue }
            let start = vertices.count
            for item in group { vertices.append(contentsOf: entries[item.key]?.vertices ?? []) }
            if vertices.count > start {
                nextDraws.append(Draw(texture: texture, tone: first.tone, start: start, count: vertices.count - start))
            }
        }
        // Replace rather than mutate a buffer that an in-flight command may still read.
        let nextBuffer = vertices.withUnsafeBytes { bytes in
            bytes.isEmpty ? nil : device.makeBuffer(bytes: bytes.baseAddress!, length: bytes.count, options: .storageModeShared)
        }
        guard vertices.isEmpty || nextBuffer != nil else { order = []; return }
        buffer = nextBuffer; draws = nextDraws; rebuilds += 1
    }
}
