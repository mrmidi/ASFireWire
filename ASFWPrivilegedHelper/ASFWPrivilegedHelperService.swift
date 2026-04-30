import Foundation

struct ASFWPrivilegedHelperConfig {
    var appBundleIdentifier: String
    var helperBundleIdentifier: String
    var machServiceName: String
    var teamIdentifier: String
    var expectedCoreAudioDeviceName: String?

    static var current: ASFWPrivilegedHelperConfig {
        let bundle = Bundle.main
        let environment = ProcessInfo.processInfo.environment
        let appID = environment["ASFW_APP_BUNDLE_IDENTIFIER"]
            ?? bundle.object(forInfoDictionaryKey: "ASFWAppBundleIdentifier") as? String
            ?? "net.mrmidi.ASFW"
        let teamID = environment["ASFW_TEAM_IDENTIFIER"]
            ?? bundle.object(forInfoDictionaryKey: "ASFWTeamIdentifier") as? String
            ?? "8CZML8FK2D"
        let helperID = environment["ASFW_HELPER_BUNDLE_IDENTIFIER"]
            ?? bundle.bundleIdentifier
            ?? "\(appID).PrivilegedHelper"
        let serviceName = environment["ASFW_HELPER_MACH_SERVICE_NAME"]
            ?? bundle.object(forInfoDictionaryKey: "ASFWHelperMachServiceName") as? String
            ?? "\(appID).PrivilegedHelper"
        let expectedCoreAudioDeviceName = Self.expectedCoreAudioDeviceName(environment: environment,
                                                                           bundle: bundle)
        return ASFWPrivilegedHelperConfig(
            appBundleIdentifier: appID,
            helperBundleIdentifier: helperID,
            machServiceName: serviceName,
            teamIdentifier: teamID,
            expectedCoreAudioDeviceName: expectedCoreAudioDeviceName
        )
    }

    var appSigningRequirement: String {
        if teamIdentifier.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return #"identifier "\#(appBundleIdentifier)""#
        }
        return #"identifier "\#(appBundleIdentifier)" and anchor apple generic and certificate leaf[subject.OU] = "\#(teamIdentifier)""#
    }

    private static func expectedCoreAudioDeviceName(environment: [String: String],
                                                    bundle: Bundle) -> String? {
        if let required = environment["ASFW_REQUIRE_CORE_AUDIO_DEVICE"],
           ["0", "false", "no"].contains(required.lowercased()) {
            return nil
        }
        if let value = environment["ASFW_EXPECTED_CORE_AUDIO_DEVICE_NAME"] {
            let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty {
                return trimmed
            }
        }
        if let required = bundle.object(forInfoDictionaryKey: "ASFWRequireCoreAudioDevice") as? Bool,
           required == false {
            return nil
        }
        if let value = bundle.object(forInfoDictionaryKey: "ASFWExpectedCoreAudioDeviceName") as? String {
            let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty && !trimmed.hasPrefix("$(") {
                return trimmed
            }
        }
        return "Alesis MultiMix Firewire"
    }
}

final class ASFWPrivilegedHelperService: NSObject, ASFWPrivilegedHelperProtocol {
    private let runner = HelperCommandRunner()
    private let diagnosticsRoot = URL(fileURLWithPath: "/Users/Shared/ASFW/Diagnostics", isDirectory: true)

    func helperVersion(_ reply: @escaping (NSDictionary) -> Void) {
        reply(result(ok: true, message: "ASFW maintenance helper is available.", extra: [
            "version": "1",
            "effectiveUserID": NSNumber(value: geteuid())
        ]))
    }

    func probeMaintenanceState(_ reply: @escaping (NSDictionary) -> Void) {
        let probe = probe(driverBundleID: defaultDriverBundleID(), expectedCDHash: "")
        reply(result(ok: probe.ok,
                     rebootRequired: probe.rebootRequired,
                     repairNeeded: probe.repairNeeded,
                     message: probe.message,
                     extra: probe.dictionary))
    }

    func captureHygieneSnapshot(_ reply: @escaping (NSDictionary) -> Void) {
        let snapshot = captureSnapshot(label: "manual")
        reply(result(ok: snapshot != nil,
                     message: snapshot.map { "Captured maintenance snapshot at \($0)." } ?? "Could not capture maintenance snapshot.",
                     snapshotPath: snapshot))
    }

    func refreshDriver(expectedCDHash: String,
                       driverBundleID: String,
                       reply: @escaping (NSDictionary) -> Void) {
        guard isSafeBundleIdentifier(driverBundleID) else {
            reply(result(ok: false, message: "Refused unsafe driver bundle identifier."))
            return
        }

        let snapshot = captureSnapshot(label: "refresh-before")
        terminateDriverProcesses(driverBundleID: driverBundleID)
        _ = runner.run("/usr/bin/killall", arguments: ["coreaudiod"], timeout: 5)

        var finalProbe = probe(driverBundleID: driverBundleID, expectedCDHash: expectedCDHash)
        let deadline = Date().addingTimeInterval(45)
        while Date() < deadline {
            finalProbe = probe(driverBundleID: driverBundleID, expectedCDHash: expectedCDHash)
            if finalProbe.isHealthyEnoughForRefresh {
                break
            }
            Thread.sleep(forTimeInterval: 1)
        }

        let afterSnapshot = captureSnapshot(label: "refresh-after") ?? snapshot
        reply(result(ok: finalProbe.ok,
                     rebootRequired: finalProbe.rebootRequired,
                     repairNeeded: finalProbe.repairNeeded,
                     message: finalProbe.message,
                     snapshotPath: afterSnapshot,
                     extra: finalProbe.dictionary))
    }

    func cleanupAfterUninstall(driverBundleID: String,
                               reply: @escaping (NSDictionary) -> Void) {
        guard isSafeBundleIdentifier(driverBundleID) else {
            reply(result(ok: false, message: "Refused unsafe driver bundle identifier."))
            return
        }

        let snapshot = captureSnapshot(label: "uninstall-before")
        terminateDriverProcesses(driverBundleID: driverBundleID)
        _ = runner.run("/usr/bin/killall", arguments: ["coreaudiod"], timeout: 5)

        var finalProbe = probe(driverBundleID: driverBundleID, expectedCDHash: "")
        let deadline = Date().addingTimeInterval(20)
        while Date() < deadline {
            finalProbe = probe(driverBundleID: driverBundleID, expectedCDHash: "")
            if !finalProbe.activeDriver && !finalProbe.staleTerminatingDriver {
                break
            }
            Thread.sleep(forTimeInterval: 1)
        }

        let afterSnapshot = captureSnapshot(label: "uninstall-after") ?? snapshot
        if finalProbe.staleTerminatingDriver {
            reply(result(ok: false,
                         rebootRequired: true,
                         message: "ASFW driver is still terminating for uninstall. Reboot before another install attempt.",
                         snapshotPath: afterSnapshot,
                         extra: finalProbe.dictionary))
        } else {
            reply(result(ok: !finalProbe.activeDriver,
                         repairNeeded: finalProbe.activeDriver,
                         message: finalProbe.activeDriver ? "ASFW driver is still active after uninstall cleanup." : "ASFW driver cleanup is complete.",
                         snapshotPath: afterSnapshot,
                         extra: finalProbe.dictionary))
        }
    }

    private func terminateDriverProcesses(driverBundleID: String) {
        let escapedID = NSRegularExpression.escapedPattern(for: driverBundleID)
        let pattern = #"/Library/SystemExtensions/.*/\#(escapedID)\.dext/"#
        let pgrep = runner.run("/usr/bin/pgrep", arguments: ["-f", pattern], timeout: 5)
        let pids = pgrep.stdout
            .split(whereSeparator: \.isNewline)
            .compactMap { token -> String? in
                let pid = String(token)
                return pid.allSatisfy(\.isNumber) ? pid : nil
            }
        guard !pids.isEmpty else { return }
        _ = runner.run("/bin/kill", arguments: ["-TERM"] + pids, timeout: 5)
    }

    private func captureSnapshot(label: String) -> String? {
        let stamp = Self.timestamp()
        let dir = diagnosticsRoot.appendingPathComponent("asfw-\(label)-\(stamp)", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            try write("ASFW maintenance snapshot\nCreated: \(Date())\nLabel: \(label)\n", to: dir.appendingPathComponent("README.txt"))
            writeCommand("/bin/ps", ["-axo", "pid,ppid,stat,%cpu,%mem,etime,comm,args"], to: dir.appendingPathComponent("top_processes.txt"))
            writeCommand("/usr/bin/pgrep", ["-fl", "ASFW|ASFWDriver|coreaudiod|com.apple.audio.DriverHelper"], to: dir.appendingPathComponent("asfw_related_processes.txt"))
            writeCommand("/usr/bin/systemextensionsctl", ["list"], to: dir.appendingPathComponent("systemextensions.txt"))
            writeCommand("/usr/sbin/ioreg", ["-p", "IOService", "-l", "-w0", "-r", "-c", "ASFWDriver"], to: dir.appendingPathComponent("ioreg_asfw_driver.txt"))
            writeCommand("/usr/sbin/ioreg", ["-p", "IOService", "-l", "-w0", "-r", "-c", "ASFWAudioNub"], to: dir.appendingPathComponent("ioreg_asfw_audio_nub.txt"))
            writeCommand("/usr/sbin/system_profiler", ["SPAudioDataType"], to: dir.appendingPathComponent("coreaudio.txt"))
            return dir.path
        } catch {
            return nil
        }
    }

    private func writeCommand(_ executable: String, _ arguments: [String], to url: URL) {
        let commandLine = ([executable] + arguments).joined(separator: " ")
        let output = runner.run(executable, arguments: arguments, timeout: 20)
        let contents = """
        Command: \(commandLine)
        Exit: \(output.status)

        stdout:
        \(output.stdout)

        stderr:
        \(output.stderr)
        """
        try? write(contents, to: url)
    }

    private func write(_ string: String, to url: URL) throws {
        try string.data(using: .utf8)?.write(to: url, options: .atomic)
    }

    private func probe(driverBundleID: String, expectedCDHash: String) -> HelperProbe {
        let systemExtensions = runner.run("/usr/bin/systemextensionsctl", arguments: ["list"], timeout: 10).stdout
        let ioreg = runner.run("/usr/sbin/ioreg", arguments: ["-p", "IOService", "-l", "-w0", "-r", "-c", "ASFWDriver"], timeout: 10).stdout
        let coreAudio = runner.run("/usr/sbin/system_profiler", arguments: ["SPAudioDataType"], timeout: 20).stdout
        let expectedDevice = ASFWPrivilegedHelperConfig.current.expectedCoreAudioDeviceName
        return HelperProbe(systemExtensions: systemExtensions,
                           ioreg: ioreg,
                           coreAudio: coreAudio,
                           driverBundleID: driverBundleID,
                           expectedCDHash: expectedCDHash,
                           expectedCoreAudioDeviceName: expectedDevice)
    }

    private func defaultDriverBundleID() -> String {
        let appID = ASFWPrivilegedHelperConfig.current.appBundleIdentifier
        return "\(appID).ASFWDriver"
    }

    private func isSafeBundleIdentifier(_ value: String) -> Bool {
        !value.isEmpty && value.range(of: #"^[A-Za-z0-9.-]+$"#, options: .regularExpression) != nil
    }

    private func result(ok: Bool,
                        rebootRequired: Bool = false,
                        repairNeeded: Bool = false,
                        message: String,
                        snapshotPath: String? = nil,
                        extra: [String: Any] = [:]) -> NSDictionary {
        var dict = extra
        dict["ok"] = NSNumber(value: ok)
        dict["rebootRequired"] = NSNumber(value: rebootRequired)
        dict["repairNeeded"] = NSNumber(value: repairNeeded)
        dict["message"] = message
        if let snapshotPath {
            dict["snapshotPath"] = snapshotPath
        }
        return NSDictionary(dictionary: dict)
    }

    private static func timestamp() -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        return formatter.string(from: Date())
    }
}

private struct HelperProbe {
    var systemExtensions: String
    var ioreg: String
    var coreAudio: String
    var driverBundleID: String
    var expectedCDHash: String
    var expectedCoreAudioDeviceName: String?

    var activeDriver: Bool {
        systemExtensions
            .split(separator: "\n")
            .contains { line in
                line.contains(driverBundleID)
                && line.contains("[activated enabled]")
                && !line.localizedCaseInsensitiveContains("terminating for uninstall")
            }
    }

    var staleTerminatingDriver: Bool {
        systemExtensions
            .split(separator: "\n")
            .contains { line in
                line.contains(driverBundleID)
                && line.localizedCaseInsensitiveContains("terminating for uninstall")
            }
    }

    var activeCDHash: String {
        if !driverBundleID.isEmpty {
            let driverBlocks = ioreg.components(separatedBy: "+-o ASFWDriver").dropFirst()
            for rawBlock in driverBlocks {
                let block = "+-o ASFWDriver" + rawBlock
                let matchesBundle =
                    block.contains(#""CFBundleIdentifier" = "\#(driverBundleID)""#)
                    || block.contains(#""IOUserServerName" = "\#(driverBundleID)""#)
                    || block.contains(#""IOPersonalityPublisher" = "\#(driverBundleID)""#)
                if matchesBundle {
                    return Self.firstCDHash(in: block)
                }
            }

            if ioreg.contains(#""CFBundleIdentifier" = ""#)
                || ioreg.contains(#""IOUserServerName" = ""#) {
                return ""
            }
        }

        return Self.firstCDHash(in: ioreg)
    }

    private static func firstCDHash(in text: String) -> String {
        let pattern = #""IOUserServerCDHash"\s*=\s*"([^"]+)""#
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return "" }
        let range = NSRange(text.startIndex..<text.endIndex, in: text)
        guard let match = regex.firstMatch(in: text, range: range),
              match.numberOfRanges > 1,
              let hashRange = Range(match.range(at: 1), in: text) else {
            return ""
        }
        return String(text[hashRange])
    }

    var coreAudioVisible: Bool {
        guard let expectedCoreAudioDeviceName,
              !expectedCoreAudioDeviceName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            return true
        }
        return coreAudio.localizedCaseInsensitiveContains(expectedCoreAudioDeviceName)
    }

    var ok: Bool {
        activeDriver && !staleTerminatingDriver && !cdHashMismatch && coreAudioVisible
    }

    var rebootRequired: Bool {
        staleTerminatingDriver
    }

    var repairNeeded: Bool {
        !ok && !rebootRequired
    }

    var isHealthyEnoughForRefresh: Bool {
        !staleTerminatingDriver && activeDriver && !cdHashMismatch && coreAudioVisible
    }

    var cdHashMismatch: Bool {
        !expectedCDHash.isEmpty && activeCDHash != expectedCDHash
    }

    var message: String {
        if staleTerminatingDriver {
            return "ASFW driver is terminating for uninstall. Reboot before another install attempt."
        }
        if ok {
            if expectedCoreAudioDeviceName == nil {
                return "ASFW driver refresh completed. CoreAudio publication is not required for this tester build."
            }
            return "ASFW driver refresh completed and CoreAudio can see \(expectedCoreAudioDeviceName ?? "the expected device")."
        }
        if cdHashMismatch {
            return "ASFW driver CDHash does not match the staged driver."
        }
        if activeDriver && !coreAudioVisible {
            return "ASFW driver is active, but CoreAudio does not currently publish \(expectedCoreAudioDeviceName ?? "the expected device")."
        }
        if !activeDriver {
            return "ASFW driver is not active."
        }
        return "ASFW maintenance check did not reach a clean state."
    }

    var dictionary: [String: Any] {
        [
            "activeDriver": NSNumber(value: activeDriver),
            "staleTerminatingDriver": NSNumber(value: staleTerminatingDriver),
            "coreAudioVisible": NSNumber(value: coreAudioVisible),
            "activeCDHash": activeCDHash,
            "expectedCDHash": expectedCDHash,
            "expectedCoreAudioDeviceName": expectedCoreAudioDeviceName ?? ""
        ]
    }
}

private struct HelperCommandResult {
    var stdout: String
    var stderr: String
    var status: Int32
}

private final class HelperCommandRunner {
    func run(_ executablePath: String, arguments: [String], timeout: TimeInterval) -> HelperCommandResult {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executablePath)
        process.arguments = arguments

        let temporaryDirectory = FileManager.default.temporaryDirectory
        let stdoutURL = temporaryDirectory.appendingPathComponent("asfw-helper-command-\(UUID().uuidString).out")
        let stderrURL = temporaryDirectory.appendingPathComponent("asfw-helper-command-\(UUID().uuidString).err")
        FileManager.default.createFile(atPath: stdoutURL.path, contents: nil)
        FileManager.default.createFile(atPath: stderrURL.path, contents: nil)

        guard let stdout = try? FileHandle(forWritingTo: stdoutURL),
              let stderr = try? FileHandle(forWritingTo: stderrURL) else {
            return HelperCommandResult(stdout: "", stderr: "Could not create command output files.", status: 126)
        }

        defer {
            try? stdout.close()
            try? stderr.close()
            try? FileManager.default.removeItem(at: stdoutURL)
            try? FileManager.default.removeItem(at: stderrURL)
        }

        process.standardOutput = stdout
        process.standardError = stderr

        do {
            try process.run()
        } catch {
            return HelperCommandResult(stdout: "", stderr: error.localizedDescription, status: 127)
        }

        let finished = DispatchSemaphore(value: 0)
        process.terminationHandler = { _ in finished.signal() }
        if finished.wait(timeout: .now() + timeout) == .timedOut {
            process.terminate()
            _ = finished.wait(timeout: .now() + 2)
            return HelperCommandResult(stdout: "", stderr: "Command timed out: \(executablePath)", status: 124)
        }

        try? stdout.close()
        try? stderr.close()
        let stdoutData = (try? Data(contentsOf: stdoutURL)) ?? Data()
        let stderrData = (try? Data(contentsOf: stderrURL)) ?? Data()
        return HelperCommandResult(
            stdout: String(data: stdoutData, encoding: .utf8) ?? "",
            stderr: String(data: stderrData, encoding: .utf8) ?? "",
            status: process.terminationStatus
        )
    }
}
