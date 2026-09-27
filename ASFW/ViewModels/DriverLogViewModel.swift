import Combine
import Foundation

@MainActor
@Observable
final class DriverLogViewModel {
    static let capacityOptions = [512, 1_024, 4_096, 8_192, 16_384]

    private(set) var records: [ASFWLogRingRecord] = []
    private(set) var filteredRecords: [ASFWLogRingRecord] = []
    private(set) var categories: [ASFWLogCategoryDescriptor] = []
    private(set) var presets: [ASFWLogCategoryPreset] = []
    var searchText = "" {
        didSet {
            searchTextSubject.send(searchText)
        }
    }
    
    private(set) var maximumLevel: UInt8 = 4
    private(set) var selectedCategories: Set<UInt8> = []
    private(set) var capacity: Int
    private(set) var localEvictedCount: UInt64 = 0
    private(set) var cursorResetCount: UInt64 = 0
    private(set) var driverStats: ASFWLogRingStats?
    private(set) var latestSequence: UInt64 = 0
    private(set) var lastPollError: String?
    var isPaused = false

    private static let capacityDefaultsKey = "driverLogViewer.capacity"
    private static let defaultCapacity = 4_096
    private static let maximumBootstrapDepth = 1_024
    private static let recordsPerPoll = 256
    private static let pollIntervalNs: UInt64 = 500_000_000
    private static let statsPollDivisor = 4

    @ObservationIgnored private let connectorObservable: ASFWDriverConnector.Observable
    @ObservationIgnored private let defaults: UserDefaults
    @ObservationIgnored private var buffer: BoundedDeque<ASFWLogRingRecord>
    @ObservationIgnored private var cursor: UInt64 = 0
    @ObservationIgnored private var cursorInitialized = false
    @ObservationIgnored private var pollCount = 0
    @ObservationIgnored private var pollingLoopActive = false
    @ObservationIgnored private var catalogLoaded = false
    @ObservationIgnored private var categoryNamesByID: [UInt8: String] = [:]
    @ObservationIgnored private var allCategoryIDs: Set<UInt8> = []
    @ObservationIgnored private var searchCancellable: AnyCancellable?
    @ObservationIgnored private let searchTextSubject = PassthroughSubject<String, Never>()

    init(connectorObservable: ASFWDriverConnector.Observable, defaults: UserDefaults = .standard) {
        self.connectorObservable = connectorObservable
        self.defaults = defaults

        let persistedCapacity = defaults.integer(forKey: Self.capacityDefaultsKey)
        let initialCapacity = Self.normalizedCapacity(persistedCapacity)
        capacity = initialCapacity
        buffer = BoundedDeque(capacity: initialCapacity)

        searchCancellable = searchTextSubject.eraseToAnyPublisher()
            .removeDuplicates()
            .debounce(for: .milliseconds(180), scheduler: DispatchQueue.main)
            .sink { [weak self] _ in
                self?.rebuildFilteredRecords()
            }
    }

    func runPolling() async {
        guard pollingLoopActive == false else { return }
        pollingLoopActive = true
        defer { pollingLoopActive = false }

        while Task.isCancelled == false {
            if connectorObservable.isConnected, isPaused == false {
                await pollOnce()
            }

            do {
                try await Task.sleep(nanoseconds: Self.pollIntervalNs)
            } catch {
                return
            }
        }
    }

    func setMaximumLevel(_ level: UInt8) {
        maximumLevel = min(level, 4)
        rebuildFilteredRecords()
    }

    func toggleCategory(_ category: UInt8) {
        if selectedCategories.contains(category) {
            selectedCategories.remove(category)
        } else {
            selectedCategories.insert(category)
        }
        rebuildFilteredRecords()
    }

    func selectAllCategories() {
        selectedCategories = allCategoryIDs
        rebuildFilteredRecords()
    }

    func clearCategorySelection() {
        selectedCategories.removeAll()
        rebuildFilteredRecords()
    }

    func selectPreset(_ preset: ASFWLogCategoryPreset) {
        selectedCategories = Set(categories.lazy.filter {
            preset.contains(category: $0.id)
        }.map(\.id))
        rebuildFilteredRecords()
    }

    func isPresetSelected(_ preset: ASFWLogCategoryPreset) -> Bool {
        selectedCategories == Set(categories.lazy.filter {
            preset.contains(category: $0.id)
        }.map(\.id))
    }

    func categoryName(for category: UInt8) -> String {
        categoryNamesByID[category] ?? "Unknown(\(category))"
    }

    /// A single plain-text clipboard item keeps multi-row pastes ordered and
    /// useful in terminals, issue trackers, and analysis prompts.
    func clipboardPayload(for selectedSequences: Set<UInt64>) -> [String] {
        Self.clipboardPayload(
            records,
            selectedSequences: selectedSequences,
            categoryNames: categoryNamesByID
        )
    }

    func setCapacity(_ requestedCapacity: Int) {
        let newCapacity = Self.normalizedCapacity(requestedCapacity)
        guard newCapacity != capacity else { return }

        let discarded = buffer.resize(to: newCapacity)
        localEvictedCount &+= UInt64(discarded)
        capacity = newCapacity
        defaults.set(newCapacity, forKey: Self.capacityDefaultsKey)
        publishBuffer()
    }

    /// Clears only the GUI buffer. The cursor remains at the current frontier.
    func clear() {
        buffer.removeAll()
        records.removeAll(keepingCapacity: true)
        filteredRecords.removeAll(keepingCapacity: true)
        localEvictedCount = 0
    }

    /// Clears the GUI and reloads a bounded tail of retained driver history.
    func reloadRetainedHistory() {
        clear()
        cursor = 0
        cursorInitialized = false
        lastPollError = nil
    }

    var selectedCategorySummary: String {
        if categories.isEmpty {
            return "Categories"
        }
        if selectedCategories == allCategoryIDs {
            return "All Categories"
        }
        if let preset = presets.first(where: { isPresetSelected($0) }) {
            return preset.name
        }
        if selectedCategories.isEmpty {
            return "No Categories"
        }
        return "\(selectedCategories.count) Categories"
    }

    var maximumLevelName: String {
        ASFWLogRingCategories.levelName(for: maximumLevel).capitalized
    }

    static func matchingRecords(
        _ records: [ASFWLogRingRecord],
        searchText: String,
        maximumLevel: UInt8,
        selectedCategories: Set<UInt8>,
        categoryNames: [UInt8: String]
    ) -> [ASFWLogRingRecord] {
        let needle = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        let knownCategoryIDs = Set(categoryNames.keys)
        let catalogAvailable = knownCategoryIDs.isEmpty == false
        let allKnownCategoriesSelected = selectedCategories == knownCategoryIDs

        return records.filter { record in
            guard record.level <= maximumLevel else { return false }
            guard catalogAvailable == false || allKnownCategoriesSelected ||
                    selectedCategories.contains(record.category) else {
                return false
            }
            guard needle.isEmpty == false else { return true }

            return record.message.localizedStandardContains(needle)
                || (categoryNames[record.category] ?? "Unknown(\(record.category))")
                    .localizedStandardContains(needle)
                || record.levelName.localizedStandardContains(needle)
                || String(record.sequence).localizedStandardContains(needle)
        }
    }

    static func clipboardPayload(
        _ records: [ASFWLogRingRecord],
        selectedSequences: Set<UInt64>,
        categoryNames: [UInt8: String]
    ) -> [String] {
        guard selectedSequences.isEmpty == false else { return [] }

        let lines = records.lazy
            .filter { selectedSequences.contains($0.sequence) }
            .map { record in
                let timestamp = String(
                    format: "%.6f s",
                    Double(record.timestampNs) / 1_000_000_000
                )
                let category = categoryNames[record.category]
                    ?? "Unknown(\(record.category))"
                return "#\(record.sequence) \(timestamp) [\(category)] "
                    + "\(record.levelName.uppercased()) \(record.message)"
            }

        let text = lines.joined(separator: "\n")
        return text.isEmpty ? [] : [text]
    }

    private func pollOnce() async {
        if catalogLoaded == false {
            let catalog = await connectorObservable.connector.logCategoryCatalog()
            guard Task.isCancelled == false else { return }
            if let catalog {
                apply(catalog: catalog)
            }
        }

        if cursorInitialized == false {
            let stats = await connectorObservable.connector.logRingStats()
            guard Task.isCancelled == false else { return }

            driverStats = stats
            if let stats {
                let historyDepth = UInt64(min(capacity, Self.maximumBootstrapDepth))
                cursor = stats.latestSequence > historyDepth
                    ? stats.latestSequence - historyDepth
                    : 0
            }
            cursorInitialized = true
        }

        let response = await connectorObservable.connector.queryLogRecords(ASFWLogRingQuery(
            afterSequence: cursor,
            categoryMask: 0xFFFF_FFFF,
            maxLevel: 4,
            contains: "",
            maxRecords: Self.recordsPerPoll
        ))
        guard Task.isCancelled == false else { return }
        guard let response else {
            lastPollError = "Unable to read the driver log ring."
            return
        }

        if response.cursorReset {
            buffer.removeAll()
            cursorResetCount &+= 1
        }

        cursor = response.nextSequence
        latestSequence = response.latestSequence
        let evicted = buffer.append(contentsOf: response.records)
        localEvictedCount &+= UInt64(evicted)
        publishBuffer()
        lastPollError = catalogLoaded
            ? nil
            : "Unable to read driver log categories (selector 1014)."

        pollCount &+= 1
        if pollCount % Self.statsPollDivisor == 0 {
            driverStats = await connectorObservable.connector.logRingStats()
        }
    }

    private func publishBuffer() {
        records = buffer.elements
        rebuildFilteredRecords()
    }

    private func rebuildFilteredRecords() {
        filteredRecords = Self.matchingRecords(
            records,
            searchText: searchText,
            maximumLevel: maximumLevel,
            selectedCategories: selectedCategories,
            categoryNames: categoryNamesByID
        )
    }

    private func apply(catalog: ASFWLogCategoryCatalog) {
        categories = catalog.categories
        presets = catalog.presets
        categoryNamesByID = Dictionary(
            uniqueKeysWithValues: catalog.categories.map { ($0.id, $0.name) })
        allCategoryIDs = catalog.categoryIDs
        selectedCategories = allCategoryIDs
        catalogLoaded = true
        rebuildFilteredRecords()
    }

    private static func normalizedCapacity(_ requestedCapacity: Int) -> Int {
        guard requestedCapacity > 0 else { return defaultCapacity }
        return min(max(requestedCapacity, 256), 32_768)
    }
}
