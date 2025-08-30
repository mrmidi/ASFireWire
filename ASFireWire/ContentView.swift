import SwiftUI
import AppKit
import Combine

class SystemExtensionViewModel: ObservableObject, SystemExtensionManagerDelegate {
    @Published var status: String = "Idle"
    @Published var logs: String = "ASFireWire Manager started\n"
    
    private let systemExtensionManager = SystemExtensionManager()
    private let dextIdentifier = "net.mrmidi.ASFireWire.ASOHCI"
    
    init() {
        systemExtensionManager.delegate = self
    }
    
    // MARK: - SystemExtensionManagerDelegate
    func systemExtensionManager(_ manager: SystemExtensionManager, didUpdateState state: SystemExtensionManager.State) {
        DispatchQueue.main.async {
            switch state {
            case .unknown:
                self.status = "Unknown state"
            case .notInstalled:
                self.status = "Driver not installed"
            case .installed(let enabled, let awaitingApproval, let uninstalling):
                if uninstalling {
                    self.status = "Uninstalling..."
                } else if awaitingApproval {
                    self.status = "Awaiting user approval in System Settings"
                } else if enabled {
                    self.status = "Driver installed and active"
                } else {
                    self.status = "Driver installed but disabled"
                }
            case .error(let message):
                self.status = "Error: \(message)"
            }
            self.addLog("Status updated: \(self.status)")
        }
    }
    
    func systemExtensionManager(_ manager: SystemExtensionManager, didEmitMessage message: String) {
        addLog("Manager: \(message)")
    }
    
    // MARK: - Public API
    func installDriver() {
        addLog("Installing driver...")
        systemExtensionManager.activate()
    }
    
    func uninstallDriver() {
        addLog("Uninstalling driver...")
        systemExtensionManager.deactivate()
    }
    
    func checkDriverStatus() {
        addLog("Checking driver status...")
        systemExtensionManager.refreshStatus()
    }
    
    func clearLog() {
        logs = "Log cleared\n"
        addLog("ASFireWire Manager ready")
    }
    
    func copyLog() {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(logs, forType: .string)
        addLog("📋 Log copied to clipboard")
    }
    
    private func addLog(_ message: String) {
        let timestamp = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
        logs += "[\(timestamp)] \(message)\n"
    }
}

struct ContentView: View {
    @StateObject private var viewModel = SystemExtensionViewModel()

    var body: some View {
        VStack(spacing: 16) {
            Text("ASFireWire Manager").font(.title)
            Text(viewModel.status).font(.caption).foregroundColor(.secondary)

            VStack {
                HStack {
                    Button("Install Driver") {
                        viewModel.installDriver()
                    }
                    .buttonStyle(.borderedProminent)
                    
                    Button("Uninstall Driver") {
                        viewModel.uninstallDriver()
                    }
                    .buttonStyle(.bordered)
                    
                    Button("Check Status") {
                        viewModel.checkDriverStatus()
                    }
                    .buttonStyle(.bordered)
                }
                
                HStack {
                    Button("Clear Log") {
                        viewModel.clearLog()
                    }
                    .buttonStyle(.bordered)
                    
                    Button("Copy Log") {
                        viewModel.copyLog()
                    }
                    .buttonStyle(.bordered)
                }
            }

            Text("You may be prompted to approve the driver in System Settings → Privacy & Security.")
                .font(.footnote).multilineTextAlignment(.center).padding(.top, 8)

            VStack(alignment: .leading) {
                Text("Log:").font(.headline)
                ScrollView {
                    ScrollViewReader { proxy in
                        Text(viewModel.logs)
                            .font(.system(.caption, design: .monospaced))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(8)
                            .background(Color(.textBackgroundColor))
                            .cornerRadius(4)
                            .id("logBottom")
                            .onChange(of: viewModel.logs) {
                                withAnimation { proxy.scrollTo("logBottom", anchor: .bottom) }
                            }
                    }
                }
                .frame(height: 200)
            }
        }
        .padding(24)
        .frame(minWidth: 520, minHeight: 460)
    }
}
