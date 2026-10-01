import AppKit
import CoreText
import Metal

/// Text styles drawn by Metal instead of SwiftUI. Each matches the SwiftUI
/// font it replaces, so the hidden SwiftUI template that reserves the slot and
/// the Metal glyphs have the same metrics.
nonisolated enum AnalyzerTextStyle: Hashable, Sendable {
    /// `.system(size: 28, weight: .semibold, design: .rounded).monospacedDigit()`
    case loudnessHero

    func font(scale: CGFloat) -> CTFont {
        switch self {
        case .loudnessHero:
            let size = 28 * scale
            let base = NSFont.systemFont(ofSize: size, weight: .semibold).fontDescriptor
            let rounded = base.withDesign(.rounded) ?? base
            let monospacedDigits = rounded.addingAttributes([
                .featureSettings: [[
                    NSFontDescriptor.FeatureKey.typeIdentifier: kNumberSpacingType,
                    NSFontDescriptor.FeatureKey.selectorIdentifier: kMonospacedNumbersSelector,
                ]],
            ])
            return (NSFont(descriptor: monospacedDigits, size: size) ?? .systemFont(ofSize: size)) as CTFont
        }
    }
}

/// One readout drawn into a canvas slot (mode `canvasMode`, index = rawValue).
nonisolated enum AnalyzerTextReadout: UInt32, CaseIterable, Sendable {
    case momentaryLUFS = 0
    case shortTermLUFS = 1
    case integratedLUFS = 2

    static let canvasMode: UInt32 = 6
    /// Same cadence as the SwiftUI readouts it replaces.
    static let refreshInterval: Double = 0.25

    var style: AnalyzerTextStyle { .loudnessHero }

    func text(_ metrics: AudioObserverMetrics) -> String {
        let loudness = metrics.analysis.loudness
        let measurement: AudioMeasurement<Float>
        switch self {
        case .momentaryLUFS: measurement = loudness.momentaryLUFS
        case .shortTermLUFS: measurement = loudness.shortTermLUFS
        case .integratedLUFS: measurement = loudness.integratedLUFS
        }
        return measurement.value.map { $0.isFinite ? String(format: "%.1f", $0) : "−∞" } ?? "—"
    }
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

    static let characters: [Character] = Array("0123456789-+−.,%∞—: ")

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

    /// Quads for `text`, horizontally centred in `rect` (drawable pixels, y
    /// down) with the line vertically centred on its ascent and descent.
    /// Pen positions are rounded to whole pixels.
    func vertices(for text: String, centredIn rect: CGRect) -> [AnalyzerGlyphVertex] {
        let total = advance(of: text)
        var pen = Float(rect.midX) - total / 2
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
