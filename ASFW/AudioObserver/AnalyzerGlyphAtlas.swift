import AppKit
import CoreText
import Metal
import SwiftUI

/// Text styles drawn by Metal instead of SwiftUI. Each matches the SwiftUI
/// font it replaces (`swiftUIFont`), so the hidden SwiftUI template that
/// reserves a readout's slot and the Metal glyphs share metrics.
nonisolated enum AnalyzerTextStyle: Hashable, Sendable {
    /// Loudness cards: 28 pt rounded semibold, tabular digits.
    case loudnessHero
    /// Value tiles: callout, monospaced, semibold.
    case tileValue
    /// Caption, tabular digits.
    case caption
    /// Caption, monospaced (diagnostics rows).
    case captionMono
    /// Caption 2.
    case caption2
    /// Meter dB labels: 10 pt monospaced.
    case meterLabel

    var swiftUIFont: Font {
        switch self {
        case .loudnessHero: .system(size: 28, weight: .semibold, design: .rounded).monospacedDigit()
        case .tileValue: .system(.callout, design: .monospaced).weight(.semibold)
        case .caption: .caption.monospacedDigit()
        case .captionMono: .system(.caption, design: .monospaced)
        case .caption2: .caption2
        case .meterLabel: .system(size: 10, design: .monospaced)
        }
    }

    func font(scale: CGFloat) -> CTFont {
        func size(_ style: NSFont.TextStyle) -> CGFloat { NSFont.preferredFont(forTextStyle: style).pointSize * scale }
        func tabular(_ font: NSFont) -> NSFont {
            let descriptor = font.fontDescriptor.addingAttributes([
                .featureSettings: [[
                    NSFontDescriptor.FeatureKey.typeIdentifier: kNumberSpacingType,
                    NSFontDescriptor.FeatureKey.selectorIdentifier: kMonospacedNumbersSelector,
                ]],
            ])
            return NSFont(descriptor: descriptor, size: font.pointSize) ?? font
        }
        let font: NSFont
        switch self {
        case .loudnessHero:
            let base = NSFont.systemFont(ofSize: 28 * scale, weight: .semibold)
            let rounded = base.fontDescriptor.withDesign(.rounded).flatMap { NSFont(descriptor: $0, size: 28 * scale) } ?? base
            font = tabular(rounded)
        case .tileValue: font = .monospacedSystemFont(ofSize: size(.callout), weight: .semibold)
        case .caption: font = tabular(.systemFont(ofSize: size(.caption1)))
        case .captionMono: font = .monospacedSystemFont(ofSize: size(.caption1), weight: .regular)
        case .caption2: font = .systemFont(ofSize: size(.caption2))
        case .meterLabel: font = .monospacedSystemFont(ofSize: 10 * scale, weight: .regular)
        }
        return font as CTFont
    }
}

/// One readout drawn by a panel canvas. `format` runs on the main actor at
/// most every `interval` seconds; every frame redraws the cached string.
nonisolated enum AnalyzerTextAlignment: Sendable { case leading, center, trailing }

struct AnalyzerTextSpec {
    typealias Alignment = AnalyzerTextAlignment
    enum Tone: Sendable { case primary, secondary }

    /// Canvas-anchor key: unique per readout id within a canvas.
    let key: Int
    let id: String
    let style: AnalyzerTextStyle
    let tone: Tone
    let alignment: Alignment
    let interval: Double
    let format: (AudioObserverMetrics, AudioObserverSnapshot) -> String

    /// Readouts redraw at 10 Hz: momentary loudness advances every 100 ms and
    /// EBU Tech 3341 asks loudness displays to refresh at least that often.
    static let metering: Double = 0.1
    /// Engineering values (ring, timings): readable, not metering.
    static let diagnostics: Double = 0.5

    init(id: String, style: AnalyzerTextStyle, tone: Tone = .primary, alignment: Alignment,
         interval: Double = AnalyzerTextSpec.metering,
         format: @escaping (AudioObserverMetrics, AudioObserverSnapshot) -> String) {
        self.id = id; self.style = style; self.tone = tone; self.alignment = alignment
        self.interval = interval; self.format = format
        self.key = Self.key(for: id)
    }

    /// FNV-1a of the id, placed above the plot slots' mode * 256 + index keys.
    static func key(for id: String) -> Int {
        var hash: UInt32 = 2_166_136_261
        for byte in id.utf8 { hash = (hash ^ UInt32(byte)) &* 16_777_619 }
        return (1 << 20) + Int(hash & 0x3fff_ffff)
    }
}

/// Formatted strings per readout key, refreshed on each readout's own
/// interval. A refresh is due once 80% of the interval has passed, because
/// the canvas also draws on a fixed cadence and a draw that lands slightly
/// early must not skip a whole period.
struct AnalyzerTextCache {
    private var entries: [Int: (text: String, at: Double)] = [:]

    mutating func text(for spec: AnalyzerTextSpec, now: Double,
                       metrics: () -> AudioObserverMetrics, snapshot: () -> AudioObserverSnapshot) -> String {
        if let entry = entries[spec.key], now - entry.at < spec.interval * 0.8 { return entry.text }
        let text = spec.format(metrics(), snapshot())
        entries[spec.key] = (text, now)
        return text
    }

    mutating func removeAll() { entries.removeAll() }
}

/// Vertex of one glyph quad corner: drawable pixels and atlas UV.
nonisolated struct AnalyzerGlyphVertex: Equatable, Sendable {
    var position: SIMD2<Float>
    var uv: SIMD2<Float>
}

/// One font at one pixel size, rasterised once by Core Text into an R8
/// coverage texture. Glyph cells are placed on whole pixels and sampled with
/// nearest filtering, so a glyph drawn at a whole-pixel pen position is a
/// copy of what Core Text rendered.
nonisolated final class AnalyzerGlyphAtlas: @unchecked Sendable {
    struct Glyph: Equatable {
        /// Texture rectangle in pixels.
        var x: Int, width: Int
        /// Quad left edge relative to the pen, in pixels.
        var left: Int
        var advance: Float
    }

    /// Printable ASCII plus the typographic symbols readouts use.
    static let characters: [Character] = (0x20...0x7e).map { Character(UnicodeScalar($0)!) } + Array("−∞—–·→")

    let texture: MTLTexture?
    let width: Int
    let height: Int
    /// Rows from the atlas top to the baseline.
    let baselineFromTop: Int
    let ascent: Float
    let descent: Float
    private let glyphs: [Character: Glyph]

    /// Rasterises `characters` from `font`. `device` nil builds the metrics
    /// only (tests of layout need no GPU).
    init(font: CTFont, device: MTLDevice?) {
        let pad = 2
        var cells: [(Character, CGGlyph, CGRect, CGFloat)] = []
        for character in Self.characters {
            // Shape each character: font features such as tabular digits
            // substitute glyphs, which a plain character-to-glyph lookup skips.
            let line = CTLineCreateWithAttributedString(
                NSAttributedString(string: String(character), attributes: [.font: font]))
            guard let run = (CTLineGetGlyphRuns(line) as? [CTRun])?.first,
                  CTRunGetGlyphCount(run) == 1 else { continue }
            var glyph = CGGlyph(0)
            var advance = CGSize.zero
            var bounds = CGRect.zero
            CTRunGetGlyphs(run, CFRange(location: 0, length: 1), &glyph)
            CTRunGetAdvances(run, CFRange(location: 0, length: 1), &advance)
            CTFontGetBoundingRectsForGlyphs(font, .horizontal, &glyph, &bounds, 1)
            cells.append((character, glyph, bounds, advance.width))
        }
        let above = Int(ceil(cells.map { $0.2.maxY }.max() ?? 0))
        let below = Int(ceil(max(0, -(cells.map { $0.2.minY }.min() ?? 0))))
        let height = above + below + 2 * pad
        var x = 0
        var table: [Character: Glyph] = [:]
        var placements: [(CGGlyph, CGPoint)] = []
        for (character, glyph, bounds, advance) in cells {
            if bounds.isEmpty {
                table[character] = Glyph(x: 0, width: 0, left: 0, advance: Float(advance))
                continue
            }
            let left = Int(floor(bounds.minX)) - pad
            let width = Int(ceil(bounds.maxX)) - Int(floor(bounds.minX)) + 2 * pad
            table[character] = Glyph(x: x, width: width, left: left, advance: Float(advance))
            // Core Graphics draws bottom-up; the baseline sits `below + pad`
            // above the bottom row, which is `above + pad` rows from the top.
            placements.append((glyph, CGPoint(x: CGFloat(x - left), y: CGFloat(below + pad))))
            x += width
        }
        let atlasWidth = max(1, x)
        let atlasHeight = max(1, height)
        var pixels = [UInt8](repeating: 0, count: atlasWidth * atlasHeight)
        pixels.withUnsafeMutableBytes { raw in
            guard let context = CGContext(data: raw.baseAddress, width: atlasWidth, height: atlasHeight,
                                          bitsPerComponent: 8, bytesPerRow: atlasWidth,
                                          space: CGColorSpaceCreateDeviceGray(),
                                          bitmapInfo: CGImageAlphaInfo.none.rawValue) else { return }
            context.setFillColor(gray: 1, alpha: 1)
            context.setAllowsFontSmoothing(false)
            for (glyph, origin) in placements {
                var g = glyph
                var p = origin
                CTFontDrawGlyphs(font, &g, &p, 1, context)
            }
        }
        var texture: MTLTexture?
        if let device {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm,
                width: atlasWidth, height: atlasHeight, mipmapped: false)
            descriptor.usage = .shaderRead
            descriptor.storageMode = .shared
            texture = device.makeTexture(descriptor: descriptor)
            texture?.replace(region: MTLRegionMake2D(0, 0, atlasWidth, atlasHeight), mipmapLevel: 0,
                             withBytes: pixels, bytesPerRow: atlasWidth)
        }
        self.texture = texture
        self.width = atlasWidth
        self.height = atlasHeight
        self.baselineFromTop = above + pad
        self.ascent = Float(CTFontGetAscent(font))
        self.descent = Float(CTFontGetDescent(font))
        self.glyphs = table
        self.coverage = pixels
    }

    /// Rasterised coverage, kept for tests.
    let coverage: [UInt8]

    func glyph(_ character: Character) -> Glyph? { glyphs[character] }

    func advance(of text: String) -> Float {
        text.reduce(0) { $0 + (glyphs[$1]?.advance ?? 0) }
    }

    /// Quads for `text` in `rect` (drawable pixels, y down), aligned
    /// horizontally as asked, with the line vertically centred on its ascent
    /// and descent. Pen positions are rounded to whole pixels.
    func vertices(for text: String, in rect: CGRect,
                  alignment: AnalyzerTextAlignment = .center) -> [AnalyzerGlyphVertex] {
        let total = advance(of: text)
        var pen: Float
        switch alignment {
        case .leading: pen = Float(rect.minX)
        case .center: pen = Float(rect.midX) - total / 2
        case .trailing: pen = Float(rect.maxX) - total
        }
        let baseline = (Float(rect.midY) + (ascent - descent) / 2).rounded()
        let top = baseline - Float(baselineFromTop)
        let atlasWidth = Float(width), atlasHeight = Float(height)
        var out: [AnalyzerGlyphVertex] = []
        out.reserveCapacity(text.count * 6)
        for character in text {
            guard let glyph = glyphs[character] else { continue }
            defer { pen += glyph.advance }
            guard glyph.width > 0 else { continue }
            let x0 = pen.rounded() + Float(glyph.left)
            let x1 = x0 + Float(glyph.width)
            let y1 = top + Float(height)
            let u0 = Float(glyph.x) / atlasWidth, u1 = Float(glyph.x + glyph.width) / atlasWidth
            let v1 = Float(height) / atlasHeight
            let a = AnalyzerGlyphVertex(position: [x0, top], uv: [u0, 0])
            let b = AnalyzerGlyphVertex(position: [x1, top], uv: [u1, 0])
            let c = AnalyzerGlyphVertex(position: [x0, y1], uv: [u0, v1])
            let d = AnalyzerGlyphVertex(position: [x1, y1], uv: [u1, v1])
            out += [a, b, c, b, d, c]
        }
        return out
    }
}
