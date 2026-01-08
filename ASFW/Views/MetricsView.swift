//
//  MetricsView.swift
//  ASFW
//
//  Isochronous metrics dashboard with live updates
//

import SwiftUI
import Charts
import Combine

// MARK: - ViewModel

@MainActor
class MetricsViewModel: ObservableObject {
    @Published var metrics: IsochRxMetrics?
    @Published var isReceiving = false
    @Published var packetsPerSecond: Double = 0
    @Published var history: [Double] = []  // Last 30 seconds of pkts/sec
    
    private var connector: ASFWDriverConnector
    private var timer: Timer?
    private var lastPacketCount: UInt64 = 0
    private var lastTimestamp = Date()
    
    init(connector: ASFWDriverConnector) {
        self.connector = connector
    }
    
    func startPolling() {
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.fetchMetrics()
            }
        }
    }
    
    func stopPolling() {
        timer?.invalidate()
        timer = nil
    }
    
    func fetchMetrics() {
        guard let m = connector.getIsochRxMetrics() else { return }
        
        // Calculate packets per second
        let now = Date()
        let elapsed = now.timeIntervalSince(lastTimestamp)
        if elapsed > 0 && m.totalPackets > lastPacketCount {
            packetsPerSecond = Double(m.totalPackets - lastPacketCount) / elapsed
        }
        lastPacketCount = m.totalPackets
        lastTimestamp = now
        
        // Update history (keep last 30 values)
        history.append(packetsPerSecond)
        if history.count > 30 {
            history.removeFirst()
        }
        
        metrics = m
        isReceiving = m.totalPackets > 0
    }
    
    func startReceive(channel: UInt8 = 0) {
        if connector.startIsochReceive(channel: channel) {
            isReceiving = true
        }
    }
    
    func stopReceive() {
        if connector.stopIsochReceive() {
            isReceiving = false
        }
    }
    
    func resetMetrics() -> Bool {
        let success = connector.resetIsochRxMetrics()
        if success {
            fetchMetrics()
            // Clear history and local state
            history.removeAll()
            packetsPerSecond = 0
            lastPacketCount = 0
        }
        return success
    }
}

// MARK: - Main View

struct MetricsView: View {
    @StateObject private var viewModel: MetricsViewModel
    
    init(connector: ASFWDriverConnector) {
        _viewModel = StateObject(wrappedValue: MetricsViewModel(connector: connector))
    }
    
    var body: some View {
        VStack(spacing: 16) {
            // Tab Bar (for future expansion)
            HStack {
                TabButton(title: "Isoch Receive", isSelected: true)
                TabButton(title: "Isoch Transmit", isSelected: false)
                TabButton(title: "Async", isSelected: false)
                TabButton(title: "Bus", isSelected: false)
                Spacer()
                
                // Start/Stop Button
                Button(action: {
                    if viewModel.isReceiving {
                        viewModel.stopReceive()
                    } else {
                        viewModel.startReceive()
                    }
                }) {
                    HStack {
                        Image(systemName: viewModel.isReceiving ? "stop.fill" : "play.fill")
                        Text(viewModel.isReceiving ? "Stop" : "Start")
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(viewModel.isReceiving ? Color.red : Color.green)
                    .foregroundColor(.white)
                    .cornerRadius(8)
                }
                
                // Reset Stats Button
                Button(action: {
                    _ = viewModel.resetMetrics()
                }) {
                    Image(systemName: "trash")
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 6)
                .background(Color.gray.opacity(0.3))
                .foregroundColor(.white)
                .cornerRadius(8)
                .help("Reset Metrics")
            }
            .padding(.horizontal)
            
            // Stats Cards Row
            HStack(spacing: 16) {
                StatCard(title: "Throughput",
                        value: String(format: "%.0f", viewModel.packetsPerSecond),
                        unit: "pkts/sec",
                        color: .blue)
                
                StatCard(title: "Total Packets",
                        value: formatNumber(viewModel.metrics?.totalPackets ?? 0),
                        unit: "",
                        color: .primary)
                
                StatCard(title: "Drops",
                        value: String(viewModel.metrics?.drops ?? 0),
                        unit: "",
                        color: (viewModel.metrics?.drops ?? 0) == 0 ? .green : .red)
                
                StatCard(title: "Errors",
                        value: String(viewModel.metrics?.errors ?? 0),
                        unit: "",
                        color: (viewModel.metrics?.errors ?? 0) == 0 ? .green : .orange)
            }
            .padding(.horizontal)
            
            // Histograms Row
            HStack(spacing: 16) {
                // Latency Histogram
                VStack(alignment: .leading) {
                    Text("Poll Latency Distribution")
                        .font(.headline)
                    
                    if let m = viewModel.metrics {
                        LatencyHistogram(buckets: [
                            m.latencyHist.0,
                            m.latencyHist.1,
                            m.latencyHist.2,
                            m.latencyHist.3
                        ])
                        .frame(height: 150)
                    } else {
                        Text("No data")
                            .foregroundColor(.secondary)
                            .frame(height: 150)
                    }
                }
                .padding()
                .background(Color(.windowBackgroundColor).opacity(0.5))
                .cornerRadius(12)
                
                // Packet Types Pie Chart
                VStack(alignment: .leading) {
                    Text("Packet Types")
                        .font(.headline)
                    
                    if let m = viewModel.metrics, m.totalPackets > 0 {
                        PacketTypePie(dataPackets: m.dataPackets, emptyPackets: m.emptyPackets)
                            .frame(width: 150, height: 150)
                    } else {
                        Text("No data")
                            .foregroundColor(.secondary)
                            .frame(width: 150, height: 150)
                    }
                }
                .padding()
                .background(Color(.windowBackgroundColor).opacity(0.5))
                .cornerRadius(12)
            }
            .padding(.horizontal)
            
            // Throughput Sparkline
            VStack(alignment: .leading) {
                Text("Throughput (last 30s)")
                    .font(.headline)
                
                ThroughputSparkline(data: viewModel.history)
                    .frame(height: 80)
            }
            .padding()
            .background(Color(.windowBackgroundColor).opacity(0.5))
            .cornerRadius(12)
            .padding(.horizontal)
            
            // CIP Status Bar
            if let m = viewModel.metrics {
                CIPStatusBar(metrics: m)
                    .padding(.horizontal)
            }
            
            Spacer()
        }
        .padding(.vertical)
        .onAppear {
            viewModel.startPolling()
        }
        .onDisappear {
            viewModel.stopPolling()
        }
    }
    
    private func formatNumber(_ n: UInt64) -> String {
        if n >= 1_000_000 {
            return String(format: "%.1fM", Double(n) / 1_000_000)
        } else if n >= 1_000 {
            return String(format: "%.1fK", Double(n) / 1_000)
        }
        return String(n)
    }
}

// MARK: - Subviews

struct TabButton: View {
    let title: String
    let isSelected: Bool
    
    var body: some View {
        Text(title)
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(isSelected ? Color.accentColor : Color.clear)
            .foregroundColor(isSelected ? .white : .secondary)
            .cornerRadius(8)
    }
}

struct StatCard: View {
    let title: String
    let value: String
    let unit: String
    let color: Color
    
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(value)
                .font(.system(size: 28, weight: .bold, design: .rounded))
                .foregroundColor(color)
            Text(unit.isEmpty ? title : "\(unit)")
                .font(.caption)
                .foregroundColor(.secondary)
            if !unit.isEmpty {
                Text(title)
                    .font(.caption2)
                    .foregroundColor(.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding()
        .background(Color(.windowBackgroundColor).opacity(0.5))
        .cornerRadius(12)
    }
}

struct LatencyHistogram: View {
    let buckets: [UInt64]
    let labels = ["<100µs", "100-500µs", "500-1ms", ">1ms"]
    
    var body: some View {
        Chart(Array(zip(labels.indices, labels)), id: \.0) { index, label in
            BarMark(
                x: .value("Bucket", label),
                y: .value("Count", buckets.indices.contains(index) ? buckets[index] : 0)
            )
            .foregroundStyle(
                buckets.indices.contains(index) && index == 0 ? Color.green :
                index == 1 ? Color.yellow :
                index == 2 ? Color.orange : Color.red
            )
        }
        .chartYAxis {
            AxisMarks(position: .leading)
        }
    }
}

struct PacketTypePie: View {
    let dataPackets: UInt64
    let emptyPackets: UInt64
    
    var body: some View {
        let total = max(1, dataPackets + emptyPackets)
        let dataRatio = Double(dataPackets) / Double(total)
        let emptyRatio = Double(emptyPackets) / Double(total)
        
        VStack {
            ZStack {
                Circle()
                    .trim(from: 0, to: CGFloat(dataRatio))
                    .stroke(Color.blue, lineWidth: 30)
                    .rotationEffect(.degrees(-90))
                
                Circle()
                    .trim(from: CGFloat(dataRatio), to: 1)
                    .stroke(Color.gray.opacity(0.5), lineWidth: 30)
                    .rotationEffect(.degrees(-90))
                
                VStack {
                    Text("\(Int(dataRatio * 100))%")
                        .font(.title2.bold())
                    Text("Data")
                        .font(.caption)
                }
            }
            
            HStack(spacing: 16) {
                Label("Data", systemImage: "circle.fill")
                    .font(.caption)
                    .foregroundColor(.blue)
                Label("Empty", systemImage: "circle.fill")
                    .font(.caption)
                    .foregroundColor(.gray)
            }
        }
    }
}

struct ThroughputSparkline: View {
    let data: [Double]
    
    var body: some View {
        if data.isEmpty {
            Text("Collecting data...")
                .foregroundColor(.secondary)
        } else {
            Chart(Array(data.enumerated()), id: \.offset) { index, value in
                LineMark(
                    x: .value("Time", index),
                    y: .value("Pkts/sec", value)
                )
                .foregroundStyle(Color.blue)
                
                AreaMark(
                    x: .value("Time", index),
                    y: .value("Pkts/sec", value)
                )
                .foregroundStyle(
                    LinearGradient(
                        colors: [Color.blue.opacity(0.3), Color.blue.opacity(0.0)],
                        startPoint: .top,
                        endPoint: .bottom
                    )
                )
            }
            .chartYAxis {
                AxisMarks(position: .leading)
            }
            .chartXAxis(.hidden)
        }
    }
}

struct CIPStatusBar: View {
    let metrics: IsochRxMetrics
    
    var body: some View {
        HStack {
            Text("CIP:")
                .font(.caption.bold())
            
            Group {
                Text("SID=\(metrics.cipSID)")
                Text("DBS=\(metrics.cipDBS)")
                Text(String(format: "FDF=0x%02X", metrics.cipFDF))
                Text(String(format: "SYT=0x%04X", metrics.cipSYT))
                Text(String(format: "DBC=0x%02X", metrics.cipDBC))
            }
            .font(.system(.caption, design: .monospaced))
            
            Spacer()
            
            Circle()
                .fill(metrics.totalPackets > 0 ? Color.green : Color.gray)
                .frame(width: 8, height: 8)
            Text("Live")
                .font(.caption)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Color.black.opacity(0.3))
        .cornerRadius(8)
    }
}

#Preview {
    MetricsView(connector: ASFWDriverConnector())
        .frame(width: 800, height: 600)
}
