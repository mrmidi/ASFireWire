//
//  DVCaptureView.swift
//  ASFW
//
//  Capture DV (MiniDV camcorder) video to a raw .dv file.
//
//  Flow: AV/C tape transport (PLAY) via raw FCP → driver receives the isoch
//  stream and fills the shared DIF ring → this view drains the ring and
//  appends chunks to a .dv file. Raw DV files open directly in QuickTime,
//  iMovie, FFmpeg, DaVinci Resolve, etc.
//

import SwiftUI
import AppKit
import Combine

// MARK: - Capture Controller

@MainActor
final class DVCaptureController: ObservableObject {
    @Published var isCapturing = false
    @Published var bytesWritten: UInt64 = 0
    @Published var framesSeen: Int = 0
    @Published var droppedFrames: Int = 0
    @Published var duplicateBlocks: Int = 0
    @Published var invalidBlocks: Int = 0
    @Published var stats = DVCaptureStats()
    @Published var systemLabel = "—"
    @Published var lastError: String?

    private var ring: DVCaptureRing?
    private var fileWriter: DVFileWriter?
    private var captureTask: Task<Void, Never>?
    private var assembler = DVFrameAssembler()

    func start(connector: ASFWDriverConnector,
               deviceID: DeviceInstanceID,
               url: URL) {
        guard !isCapturing else { return }
        lastError = nil

        guard connector.startDVCapture(deviceID: deviceID) else {
            lastError = connector.lastError ?? "startDVCapture failed (is audio receive running?)"
            return
        }

        guard let mappedRing = connector.mapDVCaptureRing() else {
            lastError = "Failed to map DV ring"
            _ = connector.stopDVCapture()
            return
        }

        let writer: DVFileWriter
        do {
            writer = try DVFileWriter(url: url)
        } catch {
            lastError = "Could not create \(url.lastPathComponent): \(error.localizedDescription)"
            _ = connector.stopDVCapture()
            mappedRing.unmap()
            return
        }

        fileWriter = writer
        ring = mappedRing
        bytesWritten = 0
        framesSeen = 0
        droppedFrames = 0
        duplicateBlocks = 0
        invalidBlocks = 0
        assembler.reset()
        systemLabel = "—"
        isCapturing = true

        captureTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                await self.tick(connector: connector)
                try? await Task.sleep(nanoseconds: 33_000_000) // ~30 Hz
            }
        }
    }

    func stop(connector: ASFWDriverConnector) async {
        guard isCapturing else { return }

        captureTask?.cancel()
        captureTask = nil

        // Final drain so the tail of the stream lands in the file.
        await tick(connector: connector)
        await apply(assembler.finish(), connector: connector)
        await shutdown(connector: connector)
    }

    func reportError(_ message: String) {
        lastError = message
    }

    private func tick(connector: ASFWDriverConnector) async {
        guard let ring else { return }

        var completedFrames: [Data] = []
        var dropped = 0
        var duplicates = 0
        var invalid = 0
        ring.drain { chunk in
            let result = assembler.ingest(chunk)
            if let frame = result.completedFrame { completedFrames.append(frame) }
            if result.droppedFrame { dropped += 1 }
            duplicates += result.duplicateBlocks
            invalid += result.invalidBlocks
        }

        droppedFrames += dropped
        duplicateBlocks += duplicates
        invalidBlocks += invalid
        if systemLabel == "—", let isPAL = assembler.isPAL {
            systemLabel = isPAL ? "PAL (625/50)" : "NTSC (525/60)"
        }
        if !completedFrames.isEmpty {
            await write(completedFrames, connector: connector)
        }
        stats = ring.stats
        if DVCaptureSessionState(rawValue: stats.sessionState) == .busReset {
            lastError = "Capture stopped because the FireWire bus reset. Select the camcorder again and restart capture."
            await shutdown(connector: connector)
        }
    }

    private func apply(_ result: DVFrameAssemblyResult,
                       connector: ASFWDriverConnector) async {
        if result.droppedFrame { droppedFrames += 1 }
        duplicateBlocks += result.duplicateBlocks
        invalidBlocks += result.invalidBlocks
        if let frame = result.completedFrame {
            await write([frame], connector: connector)
        }
    }

    private func write(_ frames: [Data],
                       connector: ASFWDriverConnector) async {
        guard let fileWriter else { return }
        do {
            bytesWritten += try await fileWriter.write(frames)
            framesSeen += frames.count
        } catch {
            lastError = "DV file write failed: \(error.localizedDescription)"
            await shutdown(connector: connector)
        }
    }

    private func shutdown(connector: ASFWDriverConnector) async {
        captureTask?.cancel()
        captureTask = nil
        _ = connector.stopDVCapture()
        ring?.unmap()
        ring = nil
        if let fileWriter {
            do {
                try await fileWriter.close()
            } catch where lastError == nil {
                lastError = "Could not finalize DV file: \(error.localizedDescription)"
            } catch { }
        }
        self.fileWriter = nil
        isCapturing = false
    }
}

// MARK: - View

struct DVCaptureView: View {
    @ObservedObject var viewModel: DebugViewModel
    @StateObject private var controller = DVCaptureController()

    @State private var avcUnits: [ASFWDriverConnector.AVCUnitInfo] = []
    @State private var selectedUnitID: UnitInstanceID?
    @State private var outputURL: URL?
    @State private var transportBusy = false
    @State private var transportStatus: String?

    var body: some View {
        Form {
            statusSection
            camcorderSection
            captureSection
        }
        .formStyle(.grouped)
        .navigationTitle("DV Capture")
        .onAppear { refreshUnits() }
        .onDisappear {
            Task { await controller.stop(connector: viewModel.connector) }
        }
    }

    // MARK: Sections

    private var statusSection: some View {
        Section("Status") {
            if viewModel.isConnected {
                Label("Connected to driver", systemImage: "checkmark.circle.fill")
                    .foregroundColor(.green)
            } else {
                Label("Driver not connected", systemImage: "xmark.circle.fill")
                    .foregroundColor(.red)
            }
            if let error = controller.lastError {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .foregroundColor(.red)
            }
        }
    }

    private var camcorderSection: some View {
        Section("Camcorder Transport (AV/C Tape Subunit)") {
            if avcUnits.isEmpty {
                HStack {
                    Text("No AV/C units found")
                        .foregroundColor(.secondary)
                    Button("Refresh") { refreshUnits() }
                }
            } else {
                Picker("Unit", selection: $selectedUnitID) {
                    ForEach(avcUnits) { unit in
                        Text("Unit \(unit.id.description) (Node: \(unit.nodeIDHex), observed \(unit.observedGuidHex))")
                            .tag(unit.id as UnitInstanceID?)
                    }
                }
            }

            HStack(spacing: 12) {
                Button {
                    sendTransport(opcode: 0xC4, operand: 0x65, label: "Rewind")
                } label: {
                    Label("Rewind", systemImage: "backward.fill")
                }

                Button {
                    sendTransport(opcode: 0xC3, operand: 0x75, label: "Play")
                } label: {
                    Label("Play", systemImage: "play.fill")
                }

                Button {
                    sendTransport(opcode: 0xC4, operand: 0x60, label: "Stop")
                } label: {
                    Label("Stop", systemImage: "stop.fill")
                }
            }
            .disabled(transportBusy || selectedUnitID == nil || !viewModel.isConnected)

            if let status = transportStatus {
                Text(status)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
    }

    private var captureSection: some View {
        Section("Capture") {
            HStack {
                Button("Choose Output File…") { chooseOutputFile() }
                    .disabled(controller.isCapturing)
                Text(outputURL?.lastPathComponent ?? "No file selected")
                    .foregroundColor(.secondary)
            }

            if controller.isCapturing {
                Button {
                    Task { await controller.stop(connector: viewModel.connector) }
                } label: {
                    Label("Stop Capture", systemImage: "stop.circle.fill")
                }
                .tint(.red)
            } else {
                Button {
                    startCapture()
                } label: {
                    Label("Start Capture", systemImage: "record.circle")
                }
                .disabled(!viewModel.isConnected || outputURL == nil || selectedUnitID == nil)
            }

            captureStats
        }
    }

    private var captureStats: some View {
        Group {
            LabeledContent("System", value: controller.systemLabel)
            LabeledContent("Isoch channel",
                           value: controller.stats.channel <= 63
                               ? "\(controller.stats.channel)"
                               : "—")
            LabeledContent("Connection",
                           value: connectionModeLabel(controller.stats.connectionMode))
            LabeledContent("Frames", value: "\(controller.framesSeen)")
            LabeledContent("Dropped frames", value: "\(controller.droppedFrames)")
                .foregroundColor(controller.droppedFrames > 0 ? .orange : .primary)
            LabeledContent("Written", value: formatBytes(controller.bytesWritten))
            LabeledContent("Isoch packets seen", value: "\(controller.stats.packetsSeen)")
            LabeledContent("DV source packets", value: "\(controller.stats.dvSourcePackets)")
            LabeledContent("Non-DV packets", value: "\(controller.stats.nonDvPackets)")
            LabeledContent("Ring overruns", value: "\(controller.stats.overruns)")
                .foregroundColor(controller.stats.overruns > 0 ? .orange : .primary)
            LabeledContent("DBC discontinuities", value: "\(controller.stats.dbcDiscontinuities)")
            LabeledContent("Invalid DIF chunks", value: "\(controller.stats.invalidDifBlocks)")
            LabeledContent("Duplicate DIF blocks", value: "\(controller.duplicateBlocks)")
            if controller.stats.nonDvPackets > 0 {
                LabeledContent("Last rejected",
                               value: String(format: "len=%u q0=%08X q1=%08X",
                                             controller.stats.lastRejectLen,
                                             controller.stats.lastRejectQ0,
                                             controller.stats.lastRejectQ1))
                    .foregroundColor(.orange)
                LabeledContent("Last xferStatus",
                               value: String(format: "%04X (evt=0x%02X)",
                                             controller.stats.lastXferStatus,
                                             controller.stats.lastXferStatus & 0x1F))
                    .foregroundColor(.orange)
            }
        }
        .font(.callout)
    }

    // MARK: Actions

    private func refreshUnits() {
        guard viewModel.isConnected else { return }

        DispatchQueue.global(qos: .userInitiated).async {
            let units = (viewModel.connector.getAVCUnits() ?? []).sorted {
                let lhsTape = $0.subunits.contains { $0.type == 0x04 }
                let rhsTape = $1.subunits.contains { $0.type == 0x04 }
                return lhsTape && !rhsTape
            }
            DispatchQueue.main.async {
                self.avcUnits = units
                if !units.contains(where: { $0.id == self.selectedUnitID }) {
                    self.selectedUnitID = units.first?.id
                }
            }
        }
    }

    private func sendTransport(opcode: UInt8, operand: UInt8, label: String) {
        guard let unitID = selectedUnitID else { return }

        // CONTROL, tape subunit (type 0x04, id 0), opcode, operand.
        // Exactly 4 bytes - transport opcodes take a single operand and some
        // camcorders reject frames with extra padding operands.
        let frame = Data([0x00, 0x20, opcode, operand])

        transportBusy = true
        transportStatus = "Sending \(label)…"

        DispatchQueue.global(qos: .userInitiated).async {
            let response = viewModel.connector.sendRawFCPCommand(unitID: unitID, frame: frame)
            DispatchQueue.main.async {
                self.transportBusy = false
                if let response, response.count >= 1 {
                    let code = response[0]
                    // AV/C response codes: 0x09 ACCEPTED, 0x08 NOT IMPLEMENTED, 0x0A REJECTED
                    let codeLabel: String
                    switch code {
                    case 0x09: codeLabel = "ACCEPTED"
                    case 0x0A: codeLabel = "REJECTED"
                    case 0x08: codeLabel = "NOT IMPLEMENTED"
                    case 0x0C: codeLabel = "IMPLEMENTED/STABLE"
                    default: codeLabel = String(format: "0x%02X", code)
                    }
                    self.transportStatus = "\(label): \(codeLabel)"
                } else {
                    // Timeout is common (some camcorders act on the command but
                    // the response path misses it) - check if the tape moved.
                    self.transportStatus = "\(label): no FCP response (command may still have worked)"
                }
            }
        }
    }

    private func chooseOutputFile() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "capture.dv"
        panel.canCreateDirectories = true
        if panel.runModal() == .OK {
            outputURL = panel.url
        }
    }

    private func startCapture() {
        guard let url = outputURL, let unitID = selectedUnitID else { return }
        controller.start(connector: viewModel.connector,
                         deviceID: unitID.device,
                         url: url)
    }

    private func connectionModeLabel(_ rawValue: UInt8) -> String {
        switch rawValue {
        case 1: return "Broadcast"
        case 2: return "CMP overlay"
        case 3: return "CMP point-to-point"
        case 4: return "Broadcast fallback"
        default: return "—"
        }
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        if bytes < 1_000_000 {
            return String(format: "%.0f KB", Double(bytes) / 1000)
        }
        return String(format: "%.1f MB", Double(bytes) / 1_000_000)
    }
}
