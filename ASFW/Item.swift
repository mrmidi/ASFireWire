//
//  Item.swift
//  ASFW
//
//  Created by Alexander Shabelnikov on 21.09.2025.
//

import Foundation

#if false
import SwiftData

@Model
final class Item {
    var timestamp: Date

    init(timestamp: Date) {
        self.timestamp = timestamp
    }
}
#else
final class Item {
    var timestamp: Date

    init(timestamp: Date) {
        self.timestamp = timestamp
    }
}
#endif
