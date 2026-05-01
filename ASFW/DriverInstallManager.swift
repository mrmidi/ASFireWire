import Foundation
import SystemExtensions

final class DriverInstallManager: NSObject, OSSystemExtensionRequestDelegate {
    static let shared = DriverInstallManager()
    private override init() {}

    private let extensionIdentifier: String = {
        if let configured = Bundle.main.object(forInfoDictionaryKey: "ASFWDriverBundleIdentifier") as? String,
           !configured.isEmpty {
            return configured
        }
        if let appIdentifier = Bundle.main.bundleIdentifier,
           !appIdentifier.isEmpty {
            return "\(appIdentifier).ASFWDriver"
        }
        return "net.mrmidi.ASFW.ASFWDriver"
    }() // matches driver bundle id

    var extensionBundleIdentifier: String { extensionIdentifier }
    var appBundlePath: String { Bundle.main.bundleURL.standardizedFileURL.path }
    var isRunningFromApplications: Bool {
        let path = appBundlePath
        return path == "/Applications" || path.hasPrefix("/Applications/")
    }

    enum ActivationError: LocalizedError {
        case unknown
        case rejected
        case requiresUserApproval
        case hostAppNotInApplications(currentPath: String)

        var errorDescription: String? {
            switch self {
            case .unknown:
                return "The system extension request failed for an unknown reason."
            case .rejected:
                return "The system extension request was rejected."
            case .requiresUserApproval:
                return "System extension approval is required in System Settings."
            case .hostAppNotInApplications(let currentPath):
                return "Move ASFW to /Applications before installing the driver. Current location: \(currentPath)"
            }
        }

        var recoverySuggestion: String? {
            switch self {
            case .requiresUserApproval:
                return "Open System Settings and approve the ASFW system extension, then return to ASFW."
            case .hostAppNotInApplications:
                return "Copy the built app to /Applications, launch that copy, then install again."
            case .unknown, .rejected:
                return nil
            }
        }
    }

    enum OperationKind { case activation, deactivation }
    private var currentOp: OperationKind = .activation
    private var completion: ((Result<String, Error>) -> Void)?

    static func isUserApprovalRequired(_ error: Error) -> Bool {
        if case ActivationError.requiresUserApproval = error {
            return true
        }
        return false
    }

    func activate(completion: @escaping (Result<String, Error>) -> Void) {
        guard isRunningFromApplications else {
            logBundleScan()
            completion(.failure(ActivationError.hostAppNotInApplications(currentPath: appBundlePath)))
            return
        }
        submit(kind: .activation, request: OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: extensionIdentifier, queue: .main), completion: completion)
        logBundleScan()
    }

    func deactivate(completion: @escaping (Result<String, Error>) -> Void) {
        submit(kind: .deactivation, request: OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: extensionIdentifier, queue: .main), completion: completion)
    }

    private func submit(kind: OperationKind, request: OSSystemExtensionRequest, completion: @escaping (Result<String, Error>) -> Void) {
        currentOp = kind
        request.delegate = self
        self.completion = completion
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    // MARK: OSSystemExtensionRequestDelegate
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        let message: String
        switch currentOp {
        case .activation:
            message = "macOS accepted the install/update request. Approval, reconnect, or reboot may still be required."
        case .deactivation:
            message = "macOS accepted the uninstall request. Cleanup or reboot may still be required."
        }
        completion?(.success(message))
        completion = nil
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        completion?(.failure(error))
        completion = nil
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        completion?(.failure(ActivationError.requiresUserApproval))
        completion = nil
    }

    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties, withExtension replacement: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        return .replace
    }

    private func logBundleScan() {
        let fm = FileManager.default
        let appBundleURL = Bundle.main.bundleURL
        let sysExtDir = appBundleURL.appendingPathComponent("Contents/Library/SystemExtensions", isDirectory: true)
        print("[DriverInstall] Main app bundle: \(appBundleURL.path)")
        print("[DriverInstall] Expected SystemExtensions dir: \(sysExtDir.path)")
        if let entries = try? fm.contentsOfDirectory(at: sysExtDir, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles]) {
            if entries.isEmpty { print("[DriverInstall] SystemExtensions directory is empty") }
            for url in entries where url.pathExtension == "dext" {
                print("[DriverInstall] Found candidate dext: \(url.lastPathComponent)")
                let plist = url.appendingPathComponent("Info.plist")
                if let data = try? Data(contentsOf: plist),
                   let plistObj = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil) as? [String: Any] {
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
