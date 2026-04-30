import Foundation
import ServiceManagement

protocol MaintenanceHelperManaging {
    var helperStatus: MaintenanceHelperApprovalState { get }

    func registerHelper() throws -> MaintenanceHelperApprovalState
    func openApprovalSettings()
    func stagedDriverCDHash(driverBundleID: String) -> String?
    func probeMaintenanceState(completion: @escaping (MaintenanceOperationOutcome) -> Void)
    func captureHygieneSnapshot(completion: @escaping (MaintenanceOperationOutcome) -> Void)
    func refreshDriver(expectedCDHash: String?,
                       driverBundleID: String,
                       completion: @escaping (MaintenanceOperationOutcome) -> Void)
    func cleanupAfterUninstall(driverBundleID: String,
                               completion: @escaping (MaintenanceOperationOutcome) -> Void)
}

final class MaintenanceHelperManager: MaintenanceHelperManaging {
    static let shared = MaintenanceHelperManager()

    private var connection: NSXPCConnection?
    private let commandRunner = MaintenanceCommandRunner()

    private init() {}

    var appBundleIdentifier: String {
        Bundle.main.bundleIdentifier ?? "net.mrmidi.ASFW"
    }

    var helperBundleIdentifier: String {
        "\(appBundleIdentifier).PrivilegedHelper"
    }

    var helperMachServiceName: String {
        "\(appBundleIdentifier).PrivilegedHelper"
    }

    var helperPlistName: String {
        "\(appBundleIdentifier).PrivilegedHelper.plist"
    }

    var teamIdentifier: String {
        (Bundle.main.object(forInfoDictionaryKey: "ASFWTeamIdentifier") as? String)
        ?? "8CZML8FK2D"
    }

    var helperStatus: MaintenanceHelperApprovalState {
        switch service.status {
        case .notRegistered:
            return .notRegistered
        case .enabled:
            return .enabled
        case .requiresApproval:
            return .requiresApproval
        case .notFound:
            return .notFound
        @unknown default:
            return .unknown
        }
    }

    private var service: SMAppService {
        SMAppService.daemon(plistName: helperPlistName)
    }

    func registerHelper() throws -> MaintenanceHelperApprovalState {
        do {
            try service.register()
        } catch {
            let current = helperStatus
            if current == .enabled || current == .requiresApproval {
                return current
            }
            throw error
        }
        return helperStatus
    }

    func unregisterHelper(completion: @escaping (Error?) -> Void) {
        service.unregister(completionHandler: completion)
    }

    func openApprovalSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }

    func invalidateConnection() {
        connection?.invalidate()
        connection = nil
    }

    func stagedDriverCDHash(driverBundleID: String) -> String? {
        let dext = Bundle.main.bundleURL
            .appendingPathComponent("Contents/Library/SystemExtensions", isDirectory: true)
            .appendingPathComponent("\(driverBundleID).dext", isDirectory: true)
        guard FileManager.default.fileExists(atPath: dext.path) else { return nil }

        let result = commandRunner.run("/usr/bin/codesign", arguments: ["-dv", "--verbose=4", dext.path])
        let combined = result.stdout + "\n" + result.stderr
        let pattern = #"CDHash=([0-9a-fA-F]+)"#
        guard let regex = try? NSRegularExpression(pattern: pattern),
              let match = regex.firstMatch(in: combined, range: NSRange(combined.startIndex..<combined.endIndex, in: combined)),
              match.numberOfRanges > 1,
              let range = Range(match.range(at: 1), in: combined) else {
            return nil
        }
        return String(combined[range])
    }

    func helperVersion(completion: @escaping (MaintenanceOperationOutcome) -> Void) {
        callHelper(timeout: 15, completion: completion) { proxy, reply in
            proxy.helperVersion(reply)
        }
    }

    func probeMaintenanceState(completion: @escaping (MaintenanceOperationOutcome) -> Void) {
        callHelper(timeout: 25, completion: completion) { proxy, reply in
            proxy.probeMaintenanceState(reply)
        }
    }

    func captureHygieneSnapshot(completion: @escaping (MaintenanceOperationOutcome) -> Void) {
        callHelper(timeout: 30, completion: completion) { proxy, reply in
            proxy.captureHygieneSnapshot(reply)
        }
    }

    func refreshDriver(expectedCDHash: String?,
                       driverBundleID: String,
                       completion: @escaping (MaintenanceOperationOutcome) -> Void) {
        callHelper(timeout: 75, completion: completion) { proxy, reply in
            proxy.refreshDriver(expectedCDHash: expectedCDHash ?? "", driverBundleID: driverBundleID, reply: reply)
        }
    }

    func cleanupAfterUninstall(driverBundleID: String,
                               completion: @escaping (MaintenanceOperationOutcome) -> Void) {
        callHelper(timeout: 45, completion: completion) { proxy, reply in
            proxy.cleanupAfterUninstall(driverBundleID: driverBundleID, reply: reply)
        }
    }

    private func callHelper(timeout: TimeInterval,
                            completion: @escaping (MaintenanceOperationOutcome) -> Void,
                            body: @escaping (ASFWPrivilegedHelperProtocol, @escaping (NSDictionary) -> Void) -> Void) {
        let oneShot = OneShotMaintenanceCompletion(completion)
        let timeoutItem = DispatchWorkItem { [weak self] in
            self?.invalidateConnection()
            oneShot.finish(.failed(message: "Maintenance helper did not respond within \(Int(timeout)) seconds.", snapshotPath: nil))
        }
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + timeout, execute: timeoutItem)

        callHelper { proxy in
            body(proxy) { dict in
                timeoutItem.cancel()
                oneShot.finish(Self.outcome(from: dict))
            }
        } failure: { error in
            timeoutItem.cancel()
            oneShot.finish(.failed(message: error.localizedDescription, snapshotPath: nil))
        }
    }

    private func callHelper(_ body: @escaping (ASFWPrivilegedHelperProtocol) -> Void,
                            failure: @escaping (Error) -> Void) {
        guard helperStatus == .enabled else {
            failure(MaintenanceHelperError.helperNotEnabled(helperStatus.displayName))
            return
        }

        let connection = activeConnection(failure: failure)
        let proxy = connection.remoteObjectProxyWithErrorHandler { error in
            failure(error)
        } as? ASFWPrivilegedHelperProtocol

        guard let proxy else {
            failure(MaintenanceHelperError.invalidProxy)
            return
        }

        body(proxy)
    }

    private func activeConnection(failure: @escaping (Error) -> Void) -> NSXPCConnection {
        if let connection {
            return connection
        }

        let connection = NSXPCConnection(machServiceName: helperMachServiceName, options: .privileged)
        connection.remoteObjectInterface = NSXPCInterface(with: ASFWPrivilegedHelperProtocol.self)
        connection.setCodeSigningRequirement(helperSigningRequirement)
        connection.invalidationHandler = { [weak self] in
            self?.connection = nil
        }
        connection.interruptionHandler = { [weak self] in
            self?.connection = nil
        }
        connection.activate()
        self.connection = connection
        return connection
    }

    private var helperSigningRequirement: String {
        if teamIdentifier.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return #"identifier "\#(helperBundleIdentifier)""#
        }
        return #"identifier "\#(helperBundleIdentifier)" and anchor apple generic and certificate leaf[subject.OU] = "\#(teamIdentifier)""#
    }

    static func outcome(from dict: NSDictionary) -> MaintenanceOperationOutcome {
        let ok = (dict["ok"] as? NSNumber)?.boolValue ?? false
        let rebootRequired = (dict["rebootRequired"] as? NSNumber)?.boolValue ?? false
        let repairNeeded = (dict["repairNeeded"] as? NSNumber)?.boolValue ?? false
        let message = dict["message"] as? String ?? "Maintenance helper returned no message."
        let snapshotPath = dict["snapshotPath"] as? String

        if ok {
            return .succeeded(message: message, snapshotPath: snapshotPath)
        }
        if rebootRequired {
            return .rebootRequired(message: message, snapshotPath: snapshotPath)
        }
        if repairNeeded {
            return .repairNeeded(message: message, snapshotPath: snapshotPath)
        }
        return .failed(message: message, snapshotPath: snapshotPath)
    }
}

enum MaintenanceHelperError: LocalizedError {
    case helperNotEnabled(String)
    case invalidProxy

    var errorDescription: String? {
        switch self {
        case .helperNotEnabled(let status):
            return "Maintenance helper is not enabled (\(status))."
        case .invalidProxy:
            return "Could not create the maintenance helper XPC proxy."
        }
    }
}

private struct MaintenanceCommandResult {
    var stdout: String
    var stderr: String
    var status: Int32
}

private final class MaintenanceCommandRunner {
    func run(_ executablePath: String, arguments: [String], timeout: TimeInterval = 10) -> MaintenanceCommandResult {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executablePath)
        process.arguments = arguments

        let temporaryDirectory = FileManager.default.temporaryDirectory
        let stdoutURL = temporaryDirectory.appendingPathComponent("asfw-command-\(UUID().uuidString).out")
        let stderrURL = temporaryDirectory.appendingPathComponent("asfw-command-\(UUID().uuidString).err")
        FileManager.default.createFile(atPath: stdoutURL.path, contents: nil)
        FileManager.default.createFile(atPath: stderrURL.path, contents: nil)

        guard let stdout = try? FileHandle(forWritingTo: stdoutURL),
              let stderr = try? FileHandle(forWritingTo: stderrURL) else {
            return MaintenanceCommandResult(stdout: "", stderr: "Could not create command output files.", status: 126)
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
            return MaintenanceCommandResult(stdout: "", stderr: error.localizedDescription, status: 127)
        }

        let finished = DispatchSemaphore(value: 0)
        process.terminationHandler = { _ in finished.signal() }
        if finished.wait(timeout: .now() + timeout) == .timedOut {
            process.terminate()
            _ = finished.wait(timeout: .now() + 2)
            return MaintenanceCommandResult(stdout: "", stderr: "Command timed out: \(executablePath)", status: 124)
        }

        try? stdout.close()
        try? stderr.close()
        let stdoutData = (try? Data(contentsOf: stdoutURL)) ?? Data()
        let stderrData = (try? Data(contentsOf: stderrURL)) ?? Data()
        return MaintenanceCommandResult(
            stdout: String(data: stdoutData, encoding: .utf8) ?? "",
            stderr: String(data: stderrData, encoding: .utf8) ?? "",
            status: process.terminationStatus
        )
    }
}

private final class OneShotMaintenanceCompletion {
    private let lock = NSLock()
    private var didComplete = false
    private let completion: (MaintenanceOperationOutcome) -> Void

    init(_ completion: @escaping (MaintenanceOperationOutcome) -> Void) {
        self.completion = completion
    }

    func finish(_ outcome: MaintenanceOperationOutcome) {
        lock.lock()
        defer { lock.unlock() }
        guard !didComplete else { return }
        didComplete = true
        completion(outcome)
    }
}
