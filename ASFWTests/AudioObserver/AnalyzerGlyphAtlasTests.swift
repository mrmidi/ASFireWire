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
        let vertices = atlas.vertices(for: "-14.9", centredIn: rect)
        #expect(vertices.count == 5 * 6)
        #expect(vertices.allSatisfy { $0.position.x == $0.position.x.rounded() && $0.position.y == $0.position.y.rounded() })
        let advance = atlas.advance(of: "-14.9")
        let penStart = Float(rect.midX) - advance / 2
        let leftmost = vertices.map(\.position.x).min()!
        let firstLeft = Float(atlas.glyph("-")!.left)
        #expect(abs(leftmost - (penStart.rounded() + firstLeft)) <= 1)
    }

    @Test func aSpaceAdvancesWithoutDrawing() {
        #expect(atlas.vertices(for: " ", centredIn: CGRect(x: 0, y: 0, width: 10, height: 10)).isEmpty)
    }

    @Test func readoutsFormatLikeTheSwiftUIReadoutsTheyReplace() {
        var metrics = AudioObserverMetrics()
        metrics.analysis.loudness.momentaryLUFS = .valid(-14.94)
        metrics.analysis.loudness.shortTermLUFS = .valid(-.infinity)
        metrics.analysis.loudness.integratedLUFS = .warmingUp
        #expect(AnalyzerTextReadout.momentaryLUFS.text(metrics) == "-14.9")
        #expect(AnalyzerTextReadout.shortTermLUFS.text(metrics) == "−∞")
        #expect(AnalyzerTextReadout.integratedLUFS.text(metrics) == "—")
        for readout in AnalyzerTextReadout.allCases {
            #expect(readout.text(metrics).allSatisfy { AnalyzerGlyphAtlas.characters.contains($0) })
        }
    }

    @Test func theAtlasUploadsToTheGPU() throws {
        let device = try #require(MTLCreateSystemDefaultDevice())
        let gpu = AnalyzerGlyphAtlas(font: AnalyzerTextStyle.loudnessHero.font(scale: 2), device: device)
        let texture = try #require(gpu.texture)
        #expect(texture.width == gpu.width && texture.height == gpu.height)
        #expect(texture.pixelFormat == .r8Unorm)
    }
}
