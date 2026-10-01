import SwiftUI

struct AudioAnalyzerView: View {
    @StateObject private var devicesModel = AudioDebugViewModel()

    private var devices: [AudioWrapperDevice] {
        devicesModel.devices.filter {
            ASFWAudioObserverClient.guid(fromDeviceUID: $0.uid) != nil
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Text("Audio Analyzer").font(.title2.bold())
                Spacer()
                Picker("Device", selection: $devicesModel.selectedDevice) {
                    ForEach(devices, id: \.id) { device in
                        Text(device.name).tag(Optional(device))
                    }
                }
                .frame(maxWidth: 400)
                Button("Refresh", systemImage: "arrow.clockwise") {
                    refresh()
                }
            }

            if let device = devicesModel.selectedDevice,
               let guid = ASFWAudioObserverClient.guid(fromDeviceUID: device.uid) {
                AudioObserverPanel(guid: guid, deviceName: device.name)
                    .id(guid)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ContentUnavailableView(
                    "No ASFW Audio Device",
                    systemImage: "waveform",
                    description: Text("Connect a FireWire audio device and refresh the list."))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .padding()
        .onAppear { refresh() }
    }

    private func refresh() {
        let previousUID = devicesModel.selectedDevice?.uid
        devicesModel.refreshDevices()
        devicesModel.selectedDevice = devices.first { $0.uid == previousUID } ?? devices.first
    }
}
