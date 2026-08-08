import SwiftUI

struct SystemLogsView: View {
    @ObservedObject private var connector: ASFWDriverConnector
    @StateObject private var viewModel: DriverLogViewModel
    @State private var followsTail = true
    @State private var selectedRecordSequences: Set<UInt64> = []

    init(connector: ASFWDriverConnector) {
        _connector = ObservedObject(wrappedValue: connector)
        _viewModel = StateObject(wrappedValue: DriverLogViewModel(connector: connector))
    }

    var body: some View {
        VStack(spacing: 0) {
            statusBar
            Divider()
            logContent
        }
        .navigationTitle("Driver Log Ring")
        .searchable(text: $viewModel.searchText, prompt: "Search message, category, level, or sequence")
        .toolbar { logToolbar }
        .task {
            await viewModel.runPolling()
        }
    }

    private var statusBar: some View {
        HStack(spacing: 10) {
            Circle()
                .fill(statusColor)
                .frame(width: 8, height: 8)
                .accessibilityHidden(true)

            Text(statusText)
                .fontWeight(.medium)

            Text("\(viewModel.filteredRecords.count.formatted()) shown")
            Text("\(viewModel.records.count.formatted()) retained")
            Text("capacity \(viewModel.capacity.formatted())")

            if viewModel.localEvictedCount > 0 {
                Text("\(viewModel.localEvictedCount.formatted()) locally evicted")
                    .foregroundStyle(.orange)
            }

            if let stats = viewModel.driverStats, stats.droppedRecords > 0 {
                Label("\(stats.droppedRecords.formatted()) driver drops", systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.red)
            }

            if viewModel.cursorResetCount > 0 {
                Text("\(viewModel.cursorResetCount.formatted()) resyncs")
                    .foregroundStyle(.orange)
            }

            if let error = viewModel.lastPollError {
                Text(error)
                    .foregroundStyle(.red)
                    .lineLimit(1)
            }

            Spacer()

            if let stats = viewModel.driverStats {
                Text("driver ring \(stats.capacityRecords.formatted()) records")
                    .foregroundStyle(.secondary)
            }
        }
        .font(.caption)
        .foregroundStyle(.secondary)
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
    }

    @ViewBuilder
    private var logContent: some View {
        if viewModel.filteredRecords.isEmpty {
            emptyState
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            ScrollViewReader { proxy in
                List(viewModel.filteredRecords, selection: $selectedRecordSequences) { record in
                    DriverLogRow(
                        record: record,
                        categoryName: viewModel.categoryName(for: record.category)
                    )
                        .id(record.sequence)
                }
                .listStyle(.plain)
                .copyable(viewModel.clipboardPayload(
                    for: selectedRecordSequences
                ))
                .onChange(of: viewModel.filteredRecords.last?.sequence) { _, sequence in
                    guard followsTail, let sequence else { return }
                    proxy.scrollTo(sequence, anchor: .bottom)
                }
                .onChange(of: viewModel.cursorResetCount) {
                    selectedRecordSequences.removeAll()
                }
            }
        }
    }

    private var emptyState: some View {
        Group {
            if connector.isConnected == false {
                ContentUnavailableView(
                    "Driver Disconnected",
                    systemImage: "cable.connector.slash",
                    description: Text("Connect to the ASFW driver to poll its diagnostic ring.")
                )
            } else if viewModel.records.isEmpty {
                ContentUnavailableView(
                    "Waiting for Driver Logs",
                    systemImage: "waveform.path.ecg",
                    description: Text("The bounded viewer will populate as the driver emits records.")
                )
            } else {
                ContentUnavailableView(
                    "No Matching Driver Logs",
                    systemImage: "line.3.horizontal.decrease.circle",
                    description: Text("Adjust the search, severity, or category filters.")
                )
            }
        }
    }

    @ToolbarContentBuilder
    private var logToolbar: some ToolbarContent {
        ToolbarItemGroup(placement: .automatic) {
            Menu {
                ForEach(0...4, id: \.self) { level in
                    Button {
                        viewModel.setMaximumLevel(UInt8(level))
                    } label: {
                        if viewModel.maximumLevel == level {
                            Label(ASFWLogRingCategories.levelName(for: UInt8(level)).capitalized,
                                  systemImage: "checkmark")
                        } else {
                            Text(ASFWLogRingCategories.levelName(for: UInt8(level)).capitalized)
                        }
                    }
                }
            } label: {
                Label(viewModel.maximumLevelName, systemImage: "line.3.horizontal.decrease.circle")
            }
            .help("Show this severity and more important records")

            Menu {
                Button("All Categories") {
                    viewModel.selectAllCategories()
                }

                ForEach(viewModel.presets) { preset in
                    Button {
                        viewModel.selectPreset(preset)
                    } label: {
                        if viewModel.isPresetSelected(preset) {
                            Label(preset.name, systemImage: "checkmark")
                        } else {
                            Text(preset.name)
                        }
                    }
                }

                Button("None") {
                    viewModel.clearCategorySelection()
                }

                Divider()

                ForEach(viewModel.categories) { category in
                    Button {
                        viewModel.toggleCategory(category.id)
                    } label: {
                        if viewModel.selectedCategories.contains(category.id) {
                            Label(category.name, systemImage: "checkmark")
                        } else {
                            Text(category.name)
                        }
                    }
                }

                if viewModel.categories.isEmpty {
                    Text("Waiting for driver category catalog…")
                }
            } label: {
                Label(viewModel.selectedCategorySummary, systemImage: "tag")
            }
            .help("Filter driver log categories")

            Button {
                followsTail.toggle()
            } label: {
                Label(followsTail ? "Following Tail" : "Follow Tail",
                      systemImage: followsTail ? "arrow.down.to.line.compact" : "arrow.down.to.line")
            }
            .help(followsTail ? "Stop following new records" : "Follow new records")

            Button {
                viewModel.isPaused.toggle()
            } label: {
                Label(viewModel.isPaused ? "Resume Polling" : "Pause Polling",
                      systemImage: viewModel.isPaused ? "play.fill" : "pause.fill")
            }
            .help(viewModel.isPaused ? "Resume driver-ring polling" : "Pause driver-ring polling")

            Menu {
                ForEach(DriverLogViewModel.capacityOptions, id: \.self) { capacity in
                    Button {
                        viewModel.setCapacity(capacity)
                    } label: {
                        if viewModel.capacity == capacity {
                            Label(capacity.formatted(), systemImage: "checkmark")
                        } else {
                            Text(capacity.formatted())
                        }
                    }
                }
            } label: {
                Label("Buffer Capacity", systemImage: "memorychip")
            }
            .help("Set the maximum number of records retained by the GUI")

            Button {
                selectedRecordSequences.removeAll()
                viewModel.reloadRetainedHistory()
            } label: {
                Label("Reload Retained History", systemImage: "arrow.clockwise")
            }
            .help("Reload a bounded tail of retained driver history")

            Button(role: .destructive) {
                selectedRecordSequences.removeAll()
                viewModel.clear()
            } label: {
                Label("Clear Viewer", systemImage: "trash")
            }
            .help("Clear the GUI buffer without clearing the driver ring")
        }
    }

    private var statusText: String {
        if connector.isConnected == false { return "Disconnected" }
        if viewModel.isPaused { return "Paused" }
        if viewModel.lastPollError != nil { return "Read error" }
        return "Live"
    }

    private var statusColor: Color {
        if connector.isConnected == false || viewModel.lastPollError != nil { return .red }
        if viewModel.isPaused { return .orange }
        return .green
    }
}
