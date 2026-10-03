import SwiftUI

struct AvcHardwareControlCard: View {
    let control: AvcHardwareControl
    let guid: UInt64
    let bridge: AvcHardwareControls
    @State private var draft: Double = 0
    @State private var editing = false
    @State private var busy = false
    @State private var failure: String?
    @State private var confirmed: AvcHardwareControl?

    private var value: AvcHardwareControl { confirmed ?? control }
    private var scopeLabel: String {
        switch control.scope {
        case 0x696e7074: "Input"
        case 0x6f757470: "Output"
        default: "Internal mixer"
        }
    }
    var body: some View {
        AvcCard {
            VStack(alignment: .leading, spacing: 12) {
                Text(control.name).font(.headline)
                Text(scopeLabel).font(.caption).foregroundStyle(.secondary)
                if let low = value.minimum, let high = value.maximum, let db = value.decibels, db.isFinite {
                    HStack {
                        Slider(value: $draft, in: low...high) { active in
                            editing = active
                            if !active { apply(decibels: Float(draft)) }
                        }
                        .disabled(busy || !value.volumeWritable)
                        .accessibilityLabel("\(control.name) volume")
                        Text(String(format: "%+.1f dB", editing ? draft : Double(db)))
                            .font(.system(.caption, design: .monospaced)).frame(width: 68, alignment: .trailing)
                    }
                    Text(String(format: "%.1f to %.1f dB", low, high)).font(.caption).foregroundStyle(.secondary)
                }
                if let muted = value.muted {
                    Button { apply(muted: !muted) } label: {
                        Label(muted ? "Unmute" : "Mute", systemImage: muted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                    }
                    .disabled(busy || !value.muteWritable)
                    .accessibilityLabel("\(control.name): \(muted ? "unmute" : "mute")")
                }
                if busy { ProgressView().controlSize(.small) }
                if let failure { Text(failure).font(.caption).foregroundStyle(.red).accessibilityLabel(failure) }
            }
        }
        .onAppear { draft = Double(value.decibels ?? 0) }
        .onChange(of: control.decibels) {
            if !editing && !busy { confirmed = nil; draft = Double(control.decibels ?? 0) }
        }
        .onChange(of: control.muted) { if !busy { confirmed = nil } }
    }
    private func apply(decibels: Float? = nil, muted: Bool? = nil) {
        guard !busy else { return }
        busy = true
        failure = nil
        Task {
            do { try await bridge.set(guid: guid, key: control.id, decibels: decibels, muted: muted) }
            catch { failure = error.localizedDescription }
            confirmed = await bridge.load(guid: guid).first { $0.id == control.id }
            draft = Double(value.decibels ?? 0)
            busy = false
        }
    }
}
