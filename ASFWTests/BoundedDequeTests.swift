import Testing
@testable import ASFW

struct BoundedDequeTests {
    @Test func appendingPastCapacityEvictsOldestInConstantStorage() {
        var deque = BoundedDeque<Int>(capacity: 3)

        let firstEvicted = deque.append(1)
        let secondEvicted = deque.append(2)
        let thirdEvicted = deque.append(3)
        let fourthEvicted = deque.append(4)

        #expect(firstEvicted == false)
        #expect(secondEvicted == false)
        #expect(thirdEvicted == false)
        #expect(fourthEvicted)

        #expect(deque.count == 3)
        #expect(deque.capacity == 3)
        #expect(deque.elements == [2, 3, 4])
    }

    @Test func shrinkingPreservesNewestElements() {
        var deque = BoundedDeque<Int>(capacity: 5)
        deque.append(contentsOf: 1...5)

        let discarded = deque.resize(to: 2)

        #expect(discarded == 3)
        #expect(deque.capacity == 2)
        #expect(deque.elements == [4, 5])
    }

    @Test func growingPreservesOrderAndAvailableCapacity() {
        var deque = BoundedDeque<Int>(capacity: 2)
        deque.append(contentsOf: [10, 20])

        let discarded = deque.resize(to: 4)
        deque.append(contentsOf: [30, 40])

        #expect(discarded == 0)
        #expect(deque.elements == [10, 20, 30, 40])
    }

    @Test func removeAllReleasesContentsAndAllowsReuse() {
        var deque = BoundedDeque<String>(capacity: 2)
        deque.append(contentsOf: ["old-1", "old-2"])

        deque.removeAll()
        deque.append("new")

        #expect(deque.count == 1)
        #expect(deque.elements == ["new"])
    }
}
