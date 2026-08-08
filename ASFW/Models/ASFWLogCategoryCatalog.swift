import Foundation

struct ASFWLogCategoryCatalog: Equatable, Sendable {
    let categories: [ASFWLogCategoryDescriptor]
    let presets: [ASFWLogCategoryPreset]

    var categoryIDs: Set<UInt8> {
        Set(categories.map(\.id))
    }

    func name(for category: UInt8) -> String {
        categories.first(where: { $0.id == category })?.name
            ?? "Unknown(\(category))"
    }
}
