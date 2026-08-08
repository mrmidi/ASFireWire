import Foundation

/// Fixed-capacity FIFO storage with O(1) append and eviction.
///
/// This avoids the repeated element shifting caused by `Array.removeFirst()`
/// in continuously updated log views.
struct BoundedDeque<Element> {
    private var storage: [Element?]
    private var head = 0

    private(set) var count = 0

    var capacity: Int { storage.count }
    var isEmpty: Bool { count == 0 }

    init(capacity: Int) {
        precondition(capacity > 0, "BoundedDeque capacity must be positive")
        storage = Array(repeating: nil, count: capacity)
    }

    /// Appends an element and returns true when the oldest element was evicted.
    @discardableResult
    mutating func append(_ element: Element) -> Bool {
        if count < capacity {
            storage[physicalIndex(for: count)] = element
            count += 1
            return false
        }

        storage[head] = element
        head = (head + 1) % capacity
        return true
    }

    /// Appends all elements and returns the number of oldest elements evicted.
    @discardableResult
    mutating func append<S: Sequence>(contentsOf elements: S) -> Int where S.Element == Element {
        var evicted = 0
        for element in elements {
            if append(element) {
                evicted += 1
            }
        }
        return evicted
    }

    /// Changes capacity while preserving the newest retained elements.
    /// Returns the number of elements discarded by shrinking.
    @discardableResult
    mutating func resize(to newCapacity: Int) -> Int {
        precondition(newCapacity > 0, "BoundedDeque capacity must be positive")
        guard newCapacity != capacity else { return 0 }

        let current = elements
        let discarded = max(0, current.count - newCapacity)
        let retained = current.suffix(newCapacity)

        storage = Array(repeating: nil, count: newCapacity)
        head = 0
        count = 0
        append(contentsOf: retained)
        return discarded
    }

    mutating func removeAll() {
        for offset in 0..<count {
            storage[physicalIndex(for: offset)] = nil
        }
        head = 0
        count = 0
    }

    /// An oldest-to-newest snapshot suitable for publishing to SwiftUI.
    var elements: [Element] {
        var result: [Element] = []
        result.reserveCapacity(count)
        for offset in 0..<count {
            guard let element = storage[physicalIndex(for: offset)] else {
                preconditionFailure("BoundedDeque internal storage invariant failed")
            }
            result.append(element)
        }
        return result
    }

    private func physicalIndex(for logicalOffset: Int) -> Int {
        (head + logicalOffset) % capacity
    }
}
