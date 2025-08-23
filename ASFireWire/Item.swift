//
//  Item.swift
//  ASFireWire
//
//  Created by Aleksandr Shabelnikov on 23.08.2025.
//

import Foundation
import SwiftData

@Model
final class Item {
    var timestamp: Date
    
    init(timestamp: Date) {
        self.timestamp = timestamp
    }
}
