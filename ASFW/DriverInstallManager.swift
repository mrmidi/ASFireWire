import Foundation
import IOKit
import SystemExtensions

final class DriverInstallManager: NSObject, OSSystemExtensionRequestDelegate {
    static let shared = DriverInstallManager()

    typealias ProgressHandler = (String) -> Void
    typealias CompletionHandler = (Result<String, Error>) -> Void

    enum OperationKind: String, Equatable, Sendable {
        case activation
        case deactivation
    }

    enum OperationError: LocalizedError, Equatable {
        case operationInProgress(OperationKind)
        case replacementVersionNotNewer(existing: String, replacement: String)
        case orphanedDriverServer
        case driverDidNotAttach
        case driverDidNotDetach

        var errorDescription: String? {
            switch self {
            case .operationInProgress(let kind):
                return "A driver \(kind.rawValue) request is already in progress."
            case .replacementVersionNotNewer(let existing, let replacement):
                return "Cannot replace ASFW build \(existing) with build \(replacement): the replacement is not newer."
            case .orphanedDriverServer:
                return "ASFW has an orphaned DriverKit server with no attached services."
            case .driverDidNotAttach:
                return "ASFW was enabled, but ASFWDriver did not attach to the FireWire controller."
            case .driverDidNotDetach:
                return "ASFW deactivation completed, but the driver or DriverKit server is still running."
            }
        }

        var recoverySuggestion: String? {
            switch self {
            case .replacementVersionNotNewer:
                return "Turn off “Require a newer build” for local development, or build without --no-bump to increment CFBundleVersion."
            case .orphanedDriverServer, .driverDidNotDetach:
                return "Quit ASFW and reboot before installing the new build. Do not delete files from /Library/SystemExtensions manually."
            case .driverDidNotAttach:
                return "Reconnect the FireWire adapter. If the controller is present, remove ASFW, wait for it to detach, and install the build again."
            case .operationInProgress:
                return "Wait for the current system-extension request to finish."
            }
        }
    }

    private final class PendingOperation {
        let kind: OperationKind
        let requireNewerBuild: Bool
        let progress: ProgressHandler?
        let completion: CompletionHandler
        var resultDescription = ""
        var replacementError: OperationError?

        init(kind: OperationKind,
             requireNewerBuild: Bool,
             progress: ProgressHandler?,
             completion: @escaping CompletionHandler) {
            self.kind = kind
            self.requireNewerBuild = requireNewerBuild
            self.progress = progress
            self.completion = completion
        }
    }

    private let extensionIdentifier = "net.mrmidi.ASFW.ASFWDriver"
    private let verificationTimeout: TimeInterval = 10
    private var pending: PendingOperation?
    private var verificationTimer: DispatchSourceTimer?
    private var verificationDeadline = Date.distantPast

    private override init() {}

    func activate(requireNewerBuild: Bool = DriverInstallSettings.defaultRequireNewerBuild,
                  progress: ProgressHandler? = nil,
                  completion: @escaping CompletionHandler) {
        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: extensionIdentifier,
            queue: .main
        )
        submit(kind: .activation,
               request: request,
               requireNewerBuild: requireNewerBuild,
               progress: progress,
               completion: completion)
        logBundleScan()
    }

    func deactivate(progress: ProgressHandler? = nil,
                    completion: @escaping CompletionHandler) {
        let request = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: extensionIdentifier,
            queue: .main
        )
        submit(kind: .deactivation,
               request: request,
               requireNewerBuild: false,
               progress: progress,
               completion: completion)
    }

    private func submit(kind: OperationKind,
                        request: OSSystemExtensionRequest,
                        requireNewerBuild: Bool,
                        progress: ProgressHandler?,
                        completion: @escaping CompletionHandler) {
        guard pending == nil else {
            completion(.failure(OperationError.operationInProgress(
                pending!.kind
            )))
            return
        }

        let operation = PendingOperation(kind: kind,
                                         requireNewerBuild: requireNewerBuild,
                                         progress: progress,
                                         completion: completion)
        pending = operation
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    // MARK: OSSystemExtensionRequestDelegate

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        guard let operation = pending else { return }
        operation.resultDescription =
            "\(operation.kind.rawValue.capitalized) finished with result \(result.rawValue)"
        beginRegistryVerification()
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFailWithError error: Error) {
        if let replacementError = pending?.replacementError {
            finish(.failure(replacementError))
        } else {
            finish(.failure(error))
        }
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        pending?.progress?(
            "Approve ASFW in System Settings > General > Login Items & Extensions > Driver Extensions."
        )
    }

    func request(
        _ request: OSSystemExtensionRequest,
        actionForReplacingExtension existing: OSSystemExtensionProperties,
        withExtension replacement: OSSystemExtensionProperties
    ) -> OSSystemExtensionRequest.ReplacementAction {
        let existingVersion = existing.bundleVersion
        let replacementVersion = replacement.bundleVersion
        guard DriverExtensionVersionPolicy.replacementIsAllowed(
            existing: existingVersion,
            replacement: replacementVersion,
            requireNewerBuild: pending?.requireNewerBuild ?? DriverInstallSettings.defaultRequireNewerBuild
        ) else {
            pending?.replacementError = .replacementVersionNotNewer(
                existing: existingVersion,
                replacement: replacementVersion
            )
            return .cancel
        }

        pending?.progress?(
            "Replacing ASFW build \(existingVersion) with build \(replacementVersion)…"
        )
        return .replace
    }

    // MARK: Registry verification

    private func beginRegistryVerification() {
        verificationTimer?.cancel()
        verificationDeadline = Date().addingTimeInterval(verificationTimeout)

        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now(), repeating: .milliseconds(250))
        timer.setEventHandler { [weak self] in
            self?.pollRegistryVerification()
        }
        verificationTimer = timer
        timer.resume()
    }

    private func pollRegistryVerification() {
        guard let operation = pending else {
            verificationTimer?.cancel()
            verificationTimer = nil
            return
        }

        let snapshot = DriverExtensionRegistryInspector.snapshot()
        let timedOut = Date() >= verificationDeadline

        switch operation.kind {
        case .activation:
            switch DriverExtensionHealthPolicy.activationDisposition(
                for: snapshot,
                timedOut: timedOut
            ) {
            case .attached:
                finish(.success(
                    "\(operation.resultDescription); ASFWDriver is attached."
                ))
            case .enabledWaitingForController:
                finish(.success(
                    "\(operation.resultDescription); ASFW is enabled and waiting for the FireWire controller."
                ))
            case .pending:
                break
            case .orphanedServer:
                finish(.failure(OperationError.orphanedDriverServer))
            case .didNotAttach:
                finish(.failure(OperationError.driverDidNotAttach))
            }

        case .deactivation:
            switch DriverExtensionHealthPolicy.deactivationDisposition(
                for: snapshot,
                timedOut: timedOut
            ) {
            case .detached:
                finish(.success(
                    "\(operation.resultDescription); ASFWDriver and its DriverKit server terminated."
                ))
            case .pending:
                break
            case .orphanedServer:
                finish(.failure(OperationError.orphanedDriverServer))
            case .didNotDetach:
                finish(.failure(OperationError.driverDidNotDetach))
            }
        }
    }

    private func finish(_ result: Result<String, Error>) {
        verificationTimer?.cancel()
        verificationTimer = nil

        guard let operation = pending else { return }
        pending = nil
        operation.completion(result)
    }

    private func logBundleScan() {
        let fm = FileManager.default
        let appBundleURL = Bundle.main.bundleURL
        let sysExtDir = appBundleURL.appendingPathComponent(
            "Contents/Library/SystemExtensions",
            isDirectory: true
        )
        print("[DriverInstall] Main app bundle: \(appBundleURL.path)")
        print("[DriverInstall] Expected SystemExtensions dir: \(sysExtDir.path)")
        if let entries = try? fm.contentsOfDirectory(
            at: sysExtDir,
            includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles]
        ) {
            if entries.isEmpty {
                print("[DriverInstall] SystemExtensions directory is empty")
            }
            for url in entries where url.pathExtension == "dext" {
                print("[DriverInstall] Found candidate dext: \(url.lastPathComponent)")
                let plist = url.appendingPathComponent("Info.plist")
                if let data = try? Data(contentsOf: plist),
                   let plistObj = try? PropertyListSerialization.propertyList(
                    from: data,
                    options: [],
                    format: nil
                   ) as? [String: Any] {
                    let bid = plistObj["CFBundleIdentifier"] as? String ?? "<nil>"
                    let ver = plistObj["CFBundleVersion"] as? String ?? "<nil>"
                    print("[DriverInstall]   CFBundleIdentifier=\(bid) CFBundleVersion=\(ver)")
                } else {
                    print("[DriverInstall]   (Could not parse Info.plist)")
                }
            }
        } else {
            print("[DriverInstall] No SystemExtensions directory present")
        }
        print("[DriverInstall] Activation target identifier: \(extensionIdentifier)")
    }
}
