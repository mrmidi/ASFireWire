import CoreText
import Metal
import Testing
@testable import ASFW

struct AnalyzerGlyphAtlasTests {
    private let atlas = AnalyzerGlyphAtlas(font: AnalyzerTextStyle.loudnessHero.font(scale: 2), device: nil)

    @Test func everyDigitIsRasterisedIntoItsOwnCell() throws {
        for character in "0123456789-.−∞—" {
            let glyph = try #require(atlas.glyph(character), "missing \(character)")
            #expect(glyph.width > 0)
            var inked = 0
            for row in 0..<atlas.height {
                for column in glyph.x..<(glyph.x + glyph.width) where atlas.coverage[row * atlas.width + column] > 0 {
                    inked += 1
                }
            }
            #expect(inked > 0, "no coverage for \(character)")
        }
    }

    @Test func digitsShareOneAdvanceSoValuesDoNotJitter() throws {
        let advances = try "0123456789".map { try #require(atlas.glyph($0)).advance }
        #expect(Set(advances).count == 1)
    }

    @Test func textIsCentredOnWholePixels() {
        let rect = CGRect(x: 100, y: 40, width: 160, height: 70)
        let vertices = atlas.vertices(for: "-14.9", in: rect)
        #expect(vertices.count == 5 * 6)
        #expect(vertices.allSatisfy { $0.position.x == $0.position.x.rounded() && $0.position.y == $0.position.y.rounded() })
        let advance = atlas.advance(of: "-14.9")
        let penStart = Float(rect.midX) - advance / 2
        let leftmost = vertices.map(\.position.x).min()!
        let firstLeft = Float(atlas.glyph("-")!.left)
        #expect(abs(leftmost - (penStart.rounded() + firstLeft)) <= 1)
    }

    @Test func aSpaceAdvancesWithoutDrawing() {
        #expect(atlas.vertices(for: " ", in: CGRect(x: 0, y: 0, width: 10, height: 10)).isEmpty)
    }

    @Test func alignmentPlacesTheRunAtTheEdgeOrCentre() {
        let rect = CGRect(x: 100, y: 0, width: 200, height: 40)
        let advance = atlas.advance(of: "12.3")
        func penStart(_ alignment: AnalyzerTextAlignment) -> Float {
            let first = atlas.vertices(for: "12.3", in: rect, alignment: alignment)[0].position.x
            return first - Float(atlas.glyph("1")!.left)
        }
        #expect(penStart(.leading) == 100)
        #expect(abs(penStart(.trailing) - (300 - advance).rounded()) <= 0.5)
        #expect(abs(penStart(.center) - (200 - advance / 2).rounded()) <= 0.5)
    }

    @Test func everyStyleCoversEveryCharacterReadoutsUse() {
        let styles: [AnalyzerTextStyle] = [.loudnessHero, .tileValue, .caption, .captionMono, .caption2, .meterLabel]
        for style in styles {
            let atlas = AnalyzerGlyphAtlas(font: style.font(scale: 2), device: nil)
            for character in "-+−0123456789.%∞— dBTPLUFSHoldWarmingupDiscontinuous/·→" {
                #expect(atlas.glyph(character) != nil, "\(style) lacks \(character)")
            }
        }
    }

    @MainActor
    @Test func formattingRunsOncePerIntervalAndKeysAreStable() {
        var calls = 0
        let spec = AnalyzerTextSpec(id: "tile.L True Peak", style: .tileValue, alignment: .leading,
                                    interval: 0.1) { _, _ in calls += 1; return "\(calls)" }
        #expect(spec.key == AnalyzerTextSpec.key(for: "tile.L True Peak"))
        #expect(spec.key != AnalyzerTextSpec.key(for: "tile.R True Peak"))
        #expect(spec.key >= 1 << 20)
        var cache = AnalyzerTextCache()
        let metrics = { AudioObserverMetrics() }
        let snapshot = { AudioObserverSnapshot() }
        #expect(cache.text(for: spec, now: 0, metrics: metrics, snapshot: snapshot) == "1")
        #expect(cache.text(for: spec, now: 0.05, metrics: metrics, snapshot: snapshot) == "1")
        // A draw landing a little early still refreshes (80% of the interval).
        #expect(cache.text(for: spec, now: 0.085, metrics: metrics, snapshot: snapshot) == "2")
        #expect(calls == 2)
    }

    @Test func theAtlasUploadsToTheGPU() throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let gpu = AnalyzerGlyphAtlas(font: AnalyzerTextStyle.loudnessHero.font(scale: 2), device: device)
        let texture = try #require(gpu.texture)
        #expect(texture.width == gpu.width && texture.height == gpu.height)
        #expect(texture.pixelFormat == .r8Unorm)
    }
    @MainActor @Test func readoutBatchReusesBuffersAndInvalidatesChangedGeometry() throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let atlas = AnalyzerGlyphAtlas(font: AnalyzerTextStyle.captionMono.font(scale: 2), device: device)
        let batch = AnalyzerReadoutBatch()
        func item(_ key: Int, _ text: String, x: CGFloat = 0,
                  tone: AnalyzerTextSpec.Tone = .primary) -> AnalyzerReadoutBatch.Item {
            .init(key: key, text: text, rect: CGRect(x: x, y: 0, width: 100, height: 30),
                  atlas: atlas, alignment: .trailing, tone: tone)
        }
        let items = [item(1, "1.23"), item(2, "4.56", x: 100)]
        batch.update(items, device: device)
        let first = try #require(batch.buffer)
        #expect(batch.draws.count == 1)
        #expect(batch.draws[0].count == 48)
        batch.update(items, device: device)
        #expect(batch.rebuilds == 1 && batch.buffer === first)
        batch.update([item(1, "7.89"), items[1]], device: device)
        #expect(batch.rebuilds == 2 && batch.buffer !== first)
        batch.update([item(1, "7.89", x: 10), item(2, "4.56", x: 100, tone: .secondary)], device: device)
        #expect(batch.rebuilds == 3 && batch.draws.count == 2)
        batch.update([], device: device)
        #expect(batch.buffer == nil && batch.draws.isEmpty)
    }

}
