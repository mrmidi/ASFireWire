import simd

nonisolated struct WaterfallCameraUniforms {
    var matrix: simd_float4x4
    var dimensions: SIMD4<Float>
}

/// One camera matrix shared by Metal geometry and the screen-space axis annotations.
nonisolated enum WaterfallProjection {
    static let ridgeCount = 48
    static var uniforms: WaterfallCameraUniforms {
        let eye = SIMD3<Float>(3, 9, 5)
        let target = SIMD3<Float>(0, 0.4, -3)
        let forward = simd_normalize(target - eye)
        let right = simd_normalize(simd_cross(forward, SIMD3<Float>(0, 1, 0)))
        let up = simd_cross(right, forward)
        let view = simd_float4x4(columns: (
            SIMD4(right.x, up.x, -forward.x, 0),
            SIMD4(right.y, up.y, -forward.y, 0),
            SIMD4(right.z, up.z, -forward.z, 0),
            SIMD4(-simd_dot(right, eye), -simd_dot(up, eye), simd_dot(forward, eye), 1)))
        // Orthographic elevated view keeps frequency spacing legible at every age.
        let near: Float = 0.1, far: Float = 40
        let projection = simd_float4x4(columns: (
            SIMD4(1 / 3.3, 0, 0, 0), SIMD4(0, 1 / 3.1, 0, 0),
            SIMD4(0, 0, -1 / (far - near), 0), SIMD4(0, 0, -near / (far - near), 1)))
        return WaterfallCameraUniforms(matrix: projection * view, dimensions: SIMD4(3.6, 0.85, 6, Float(ridgeCount)))
    }
    static func point(frequency: Double, age: Double, level: Double = 0) -> SIMD2<Double> {
        let camera = uniforms
        let p = camera.matrix * SIMD4(Float(frequency - 0.5) * camera.dimensions.x,
                                     Float(level) * camera.dimensions.y, -Float(age) * camera.dimensions.z, 1)
        return SIMD2(Double(p.x / p.w), Double(p.y / p.w))
    }
}
