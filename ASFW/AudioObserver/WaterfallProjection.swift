import simd

nonisolated struct WaterfallCameraUniforms {
    var matrix: simd_float4x4
    var dimensions: SIMD4<Float>
}

/// One camera matrix shared by Metal geometry and the screen-space axis annotations.
nonisolated enum WaterfallProjection {
    static let historyRows = 65
    static var uniforms: WaterfallCameraUniforms {
        // Oblique orthographic camera: the front FFT has horizontal frequency and
        // vertical level axes; older measurements recede diagonally up and right.
        let matrix = simd_float4x4(columns: (
            SIMD4(0.42, 0, 0, 0), SIMD4(0, 0.42, -0.02, 0),
            SIMD4(-0.07, -0.13, -0.10, 0), SIMD4(-0.22, -0.80, 0.20, 1)))
        return WaterfallCameraUniforms(matrix: matrix, dimensions: SIMD4(3.4, 2.0, 6, Float(historyRows)))
    }
    static func point(frequency: Double, age: Double, level: Double = 0) -> SIMD2<Double> {
        let camera = uniforms
        let p = camera.matrix * SIMD4(Float(frequency - 0.5) * camera.dimensions.x,
                                     Float(level) * camera.dimensions.y, -Float(age) * camera.dimensions.z, 1)
        return SIMD2(Double(p.x / p.w), Double(p.y / p.w))
    }
}
