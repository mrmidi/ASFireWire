import Foundation

struct ASFWLogCategoryPreset: Identifiable, Equatable, Sendable {
    let id: Int
    let name: String
    let categoryMask: UInt32

    func contains(category: UInt8) -> Bool {
        category < 32 && (categoryMask & (1 << UInt32(category))) != 0
    }
}
