import SwiftUI

struct AudioAnalyzerView: View {
    let devices: [AudioWrapperDevice]
    let refresh: () -> Void
    @State private var selectedDeviceID: AudioWrapperDevice.ID?

    private var selectedDevice: AudioWrapperDevice? {
        devices.first { $0.id == selectedDeviceID } ?? devices.first
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Text("Audio Analyzer").font(.title2.bold())
                Spacer()
                Picker("Device", selection: Binding(
                    get: { selectedDevice?.id },
                    set: { selectedDeviceID = $0 }
                )) {
                    ForEach(devices, id: \.id) { device in
                        Text(device.name).tag(Optional(device.id))
                    }
                }
                .frame(maxWidth: 400)
                Button("Refresh", systemImage: "arrow.clockwise") {
                    refresh()
                }
            }

            if let device = selectedDevice,
               let guid = ASFWAudioObserverClient.guid(fromDeviceUID: device.uid) {
                AudioObserverPanel(guid: guid, deviceName: device.name)
                    .id(guid)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ContentUnavailableView(
                    "No ASFW Audio Device",
                    systemImage: "waveform",
                    description: Text("Connect an audio device and wait for it to become available in Core Audio."))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .padding()
        .onAppear { refresh() }
    }

}
