import SwiftUI
import AppKit

struct ContentView: View {
    private let dextIdentifier = "net.mrmidi.ASFireWire.ASOHCI"
    @State private var status: String = "Idle"
    @State private var logs: String = "ASFireWire Manager started\n"
    private let systemExtensionManager = SystemExtensionManager()

    var body: some View {
        VStack(spacing: 16) {
            Text("ASFireWire Manager").font(.title)
            Text(status).font(.caption).foregroundColor(.secondary)

            VStack {
                HStack {
                    Button("Install Driver") {
                        addLog("Install button clicked")
                        systemExtensionManager.activateDriver { message in
                            self.status = message
                            self.addLog("Install result: \(message)")
                        }
                    }
                    Button("Uninstall Driver") {
                        addLog("Uninstall button clicked")
                        systemExtensionManager.deactivateDriver { message in
                            self.status = message
                            self.addLog("Uninstall result: \(message)")
                        }
                    }
                    Button("Check Status") {
                        addLog("Check status button clicked")
                        checkDriverStatus()
                    }
                }

                HStack {
                    Button("Enable Dev Mode") {
                        addLog("Enable developer mode button clicked")
                        enableDeveloperMode()
                    }
                    .font(.caption)

                    Button("Show Logs") {
                        addLog("Show logs button clicked")
                        showSystemLogs()
                    }
                    .font(.caption)
                }
            }

            Text("You may be prompted to approve the driver in System Settings → Privacy & Security.")
                .font(.footnote).multilineTextAlignment(.center).padding(.top, 8)

            VStack(alignment: .leading) {
                Text("Log:").font(.headline)
                ScrollView {
                    ScrollViewReader { proxy in
                        Text(logs)
                            .font(.system(.caption, design: .monospaced))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(8)
                            .background(Color(.textBackgroundColor))
                            .cornerRadius(4)
                            .id("logBottom")
                            .onChange(of: logs) {
                                withAnimation { proxy.scrollTo("logBottom", anchor: .bottom) }
                            }
                    }
                }
                .frame(height: 200)

                HStack {
                    Button("Copy Log") {
                        let pasteboard = NSPasteboard.general
                        pasteboard.clearContents()
                        pasteboard.setString(logs, forType: .string)
                        addLog("📋 Log copied to clipboard")
                    }
                    .font(.caption)

                    Button("Clear Log") {
                        logs = "Log cleared\n"
                        addLog("ASFireWire Manager ready")
                    }
                    .font(.caption)
                }
            }
        }
        .padding(24)
        .frame(minWidth: 520, minHeight: 460)
    }

    private func addLog(_ message: String) {
        let df = DateFormatter()
        df.dateFormat = "HH:mm:ss"
        let ts = df.string(from: Date())
        logs += "[\(ts)] \(message)\n"
    }

    private func checkDriverStatus() {
        addLog("Checking system extension status…")

        let bundlePath = Bundle.main.bundleURL.appendingPathComponent("Contents/Library/SystemExtensions/\(dextIdentifier).dext")
        if FileManager.default.fileExists(atPath: bundlePath.path) {
            addLog("✅ Driver bundle found at: \(bundlePath.path)")

            var infoPlistPath = bundlePath.appendingPathComponent("Info.plist")
            if !FileManager.default.fileExists(atPath: infoPlistPath.path) {
                infoPlistPath = bundlePath.appendingPathComponent("Contents/Info.plist")
            }
            if let data = try? Data(contentsOf: infoPlistPath),
               let obj = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil),
               let plist = obj as? [String: Any],
               let actualBundleID = plist["CFBundleIdentifier"] as? String {
                addLog("🔍 Actual dext bundle ID: \(actualBundleID)")
                if actualBundleID != dextIdentifier {
                    addLog("⚠️ BUNDLE ID MISMATCH! This will cause installation to fail.")
                }
            }
        } else {
            addLog("❌ Driver bundle NOT found at: \(bundlePath.path)")
        }

        // systemextensionsctl list (may require non-sandbox context)
        let task = Process()
        task.launchPath = "/usr/bin/systemextensionsctl"
        task.arguments = ["list"]
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = pipe
        do {
            try task.run()
            task.waitUntilExit()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            let output = String(data: data, encoding: .utf8) ?? ""
            addLog("systemextensionsctl list output:")
            for line in output.split(separator: "\n") {
                let s = String(line)
                if s.contains("ASOHCI") || s.contains("net.mrmidi.ASFireWire") {
                    addLog("🔍 FOUND: \(s)")
                } else if s.contains("---") || s.contains("enabled") {
                    addLog("📋 \(s)")
                }
            }
            if !output.contains("ASOHCI") && !output.contains("net.mrmidi.ASFireWire") {
                addLog("ℹ️ No ASFireWire dext found in system extensions")
            }
        } catch {
            addLog("❌ Failed to run systemextensionsctl: \(error.localizedDescription)")
        }
    }

    private func enableDeveloperMode() {
        addLog("Attempting to enable system extensions developer mode… (admin required)")
        let task = Process()
        task.launchPath = "/usr/bin/sudo"
        task.arguments = ["/usr/bin/systemextensionsctl", "developer", "on"]
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = pipe
        do {
            try task.run()
            task.waitUntilExit()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            let output = String(data: data, encoding: .utf8) ?? ""
            if task.terminationStatus == 0 { addLog("✅ Developer mode enabled successfully") }
            else { addLog("❌ Failed to enable developer mode\n\(output)") }
        } catch {
            addLog("❌ Failed to run systemextensionsctl developer: \(error.localizedDescription)")
        }
    }

    private func showSystemLogs() {
        addLog("Opening Console.app… Look for subsystem: com.apple.systemextensions, net.mrmidi.ASFireWire; process: kernelmanagerd")
        if let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.Console") {
            NSWorkspace.shared.open(url)
        }
    }
}
