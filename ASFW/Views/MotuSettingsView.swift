import SwiftUI

struct MotuSettingsView: View {
    @ObservedObject var connector: ASFWDriverConnector
    @AppStorage(ASFWDriverConnector.motuCoreAudioVolumeControlsKey)
    private var coreAudioVolumeControls: Bool = true

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text("MOTU")
                .font(.title2)
                .fontWeight(.bold)

            if !connector.isConnected {
                Text("⚠️ Not connected to driver — the setting is saved and sent on the next connection")
                    .foregroundColor(.orange)
                    .padding()
            }

            VStack(alignment: .leading, spacing: 8) {
                Toggle("Enable Core Audio volume controls", isOn: $coreAudioVolumeControls)
                    .font(.headline)

                Text("On: the macOS volume keys, mute and menu-bar slider scale the output in the driver; the device's MONITOR level does not move. Off: macOS shows no volume for the device, and the level is set on the device (MONITOR) or in CueMix.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .padding(.leading, 4)

                Text("One setting for all MOTU devices. Today it applies to the 828mk3.")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .padding(.leading, 4)
            }
            .padding()
            .background(Color(nsColor: .controlBackgroundColor))
            .cornerRadius(8)

            Spacer()
        }
        .padding()
        .onChange(of: coreAudioVolumeControls) { _, enabled in
            guard connector.isConnected else { return }
            DispatchQueue.global(qos: .userInitiated).async {
                _ = connector.setMotuCoreAudioVolumeControls(enabled: enabled)
            }
        }
    }
}
