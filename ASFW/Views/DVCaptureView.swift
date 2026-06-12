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
    @Published var stats = DVCaptureStats()
    @Published var systemLabel = "—"
    @Published var lastError: String?

    private var ring: DVCaptureRing?
    private var fileHandle: FileHandle?
    private var captureTask: Task<Void, Never>?

    // Frame accumulator: only exact-size frames are written, so a duplicated
    // or lost packet corrupts one frame instead of misaligning the whole file
    // (DV demuxers read fixed 120000/144000-byte records with no resync).
    private var currentFrame = Data()
    private var inFrame = false
    private var expectedFrameBytes = 120_000

    func start(connector: ASFWDriverConnector, channel: UInt8, url: URL) {
        guard !isCapturing else { return }
        lastError = nil

        guard FileManager.default.createFile(atPath: url.path, contents: nil),
              let handle = try? FileHandle(forWritingTo: url) else {
            lastError = "Could not create \(url.lastPathComponent)"
            return
        }

        guard connector.startDVCapture(channel: channel) else {
            lastError = connector.lastError ?? "startDVCapture failed (is audio receive running?)"
            try? handle.close()
            return
        }

        guard let mappedRing = connector.mapDVCaptureRing() else {
            lastError = "Failed to map DV ring"
            _ = connector.stopDVCapture()
            try? handle.close()
            return
        }

        fileHandle = handle
        ring = mappedRing
        bytesWritten = 0
        framesSeen = 0
        droppedFrames = 0
        currentFrame.removeAll()
        inFrame = false
        systemLabel = "—"
        isCapturing = true

        captureTask = Task { [weak self] in
            while !Task.isCancelled {
                self?.tick()
                try? await Task.sleep(nanoseconds: 33_000_000) // ~30 Hz
            }
        }
    }

    func stop(connector: ASFWDriverConnector) {
        guard isCapturing else { return }

        captureTask?.cancel()
        captureTask = nil

        // Final drain so the tail of the stream lands in the file.
        tick()
        flushCurrentFrame()

        _ = connector.stopDVCapture()
        ring?.unmap()
        ring = nil
        try? fileHandle?.close()
        fileHandle = nil
        isCapturing = false
    }

    private func tick() {
        guard let ring else { return }

        ring.drain { chunk in
            // Frame start: first DIF block is a header-section block (SCT=0)
            // with sequence number 0. Masked comparison per Apple's
            // AVCVideoServices DVReceiver ((u16 & 0xE0FC) == 0x0004) - the
            // unmasked bits are reserved/arbitrary and vary between devices.
            let isFrameStart = (chunk[0] & 0xE0) == 0x00 && (chunk[1] & 0xFC) == 0x04
            if isFrameStart {
                flushCurrentFrame()
                inFrame = true
                let isPAL = (chunk[3] & 0x80) != 0
                expectedFrameBytes = isPAL ? 144_000 : 120_000
                if systemLabel == "—" {
                    systemLabel = isPAL ? "PAL (625/50)" : "NTSC (525/60)"
                }
                currentFrame.removeAll(keepingCapacity: true)
            }
            guard inFrame else { return }
            currentFrame.append(contentsOf: chunk)
        }

        stats = ring.stats
    }

    private func flushCurrentFrame() {
        guard inFrame, !currentFrame.isEmpty else { return }
        if currentFrame.count == expectedFrameBytes, let fileHandle {
            fileHandle.write(currentFrame)
            bytesWritten += UInt64(currentFrame.count)
            framesSeen += 1
        } else {
            droppedFrames += 1
        }
    }
}

// MARK: - View

struct DVCaptureView: View {
    @ObservedObject var viewModel: DebugViewModel
    @StateObject private var controller = DVCaptureController()

    @State private var avcUnits: [ASFWDriverConnector.AVCUnitInfo] = []
    @State private var selectedUnitGUID: UInt64?
    @State private var channelText = "63"
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
                Picker("Unit", selection: $selectedUnitGUID) {
                    ForEach(avcUnits) { unit in
                        Text("GUID: \(unit.guidHex) (Node: \(unit.nodeIDHex))")
                            .tag(unit.guid as UInt64?)
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
            .disabled(transportBusy || selectedUnitGUID == nil || !viewModel.isConnected)

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
                TextField("Channel", text: $channelText)
                    .frame(width: 60)
                Text("Isoch channel (camcorders broadcast on 63)")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            .disabled(controller.isCapturing)

            HStack {
                Button("Choose Output File…") { chooseOutputFile() }
                    .disabled(controller.isCapturing)
                Text(outputURL?.lastPathComponent ?? "No file selected")
                    .foregroundColor(.secondary)
            }

            if controller.isCapturing {
                Button {
                    controller.stop(connector: viewModel.connector)
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
                .disabled(!viewModel.isConnected || outputURL == nil)
            }

            captureStats
        }
    }

    private var captureStats: some View {
        Group {
            LabeledContent("System", value: controller.systemLabel)
            LabeledContent("Frames", value: "\(controller.framesSeen)")
            LabeledContent("Dropped frames", value: "\(controller.droppedFrames)")
                .foregroundColor(controller.droppedFrames > 0 ? .orange : .primary)
            LabeledContent("Written", value: formatBytes(controller.bytesWritten))
            LabeledContent("Isoch packets seen", value: "\(controller.stats.packetsSeen)")
            LabeledContent("DV source packets", value: "\(controller.stats.dvSourcePackets)")
            LabeledContent("Non-DV packets", value: "\(controller.stats.nonDvPackets)")
            LabeledContent("Ring overruns", value: "\(controller.stats.overruns)")
                .foregroundColor(controller.stats.overruns > 0 ? .orange : .primary)
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
            let units = viewModel.connector.getAVCUnits() ?? []
            DispatchQueue.main.async {
                self.avcUnits = units
                if self.selectedUnitGUID == nil, let first = units.first {
                    self.selectedUnitGUID = first.guid
                }
            }
        }
    }

    private func sendTransport(opcode: UInt8, operand: UInt8, label: String) {
        guard let guid = selectedUnitGUID else { return }

        // CONTROL, tape subunit (type 0x04, id 0), opcode, operand.
        // Exactly 4 bytes - transport opcodes take a single operand and some
        // camcorders reject frames with extra padding operands.
        let frame = Data([0x00, 0x20, opcode, operand])

        transportBusy = true
        transportStatus = "Sending \(label)…"

        DispatchQueue.global(qos: .userInitiated).async {
            let response = viewModel.connector.sendRawFCPCommand(guid: guid, frame: frame)
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
        guard let url = outputURL else { return }
        let channel = UInt8(channelText) ?? 63
        controller.start(connector: viewModel.connector,
                         channel: min(channel, 63),
                         url: url)
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        if bytes < 1_000_000 {
            return String(format: "%.0f KB", Double(bytes) / 1000)
        }
        return String(format: "%.1f MB", Double(bytes) / 1_000_000)
    }
}
