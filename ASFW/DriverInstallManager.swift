import Foundation
import SystemExtensions

final class DriverInstallManager: NSObject, OSSystemExtensionRequestDelegate {
    static let shared = DriverInstallManager()
    private override init() {}

    private let extensionIdentifier = "net.mrmidi.ASFW.ASFWDriver" // matches driver bundle id

    enum ActivationError: Error { case unknown, rejected, requiresUserApproval }
    enum OperationKind { case activation, deactivation }
    private var currentOp: OperationKind = .activation
    private var completion: ((Result<String, Error>) -> Void)?

    func activate(completion: @escaping (Result<String, Error>) -> Void) {
        submit(kind: .activation, request: OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: extensionIdentifier, queue: .main), completion: completion)
    }

    func deactivate(completion: @escaping (Result<String, Error>) -> Void) {
        submit(kind: .deactivation, request: OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: extensionIdentifier, queue: .main), completion: completion)
    }

    private func submit(kind: OperationKind, request: OSSystemExtensionRequest, completion: @escaping (Result<String, Error>) -> Void) {
        currentOp = kind
        request.delegate = self
        self.completion = completion
        logBundleScan()
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    // MARK: OSSystemExtensionRequestDelegate
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        let op = (currentOp == .activation ? "Activation" : "Deactivation")
        completion?(.success("\(op) finished with result: \(result.rawValue)"))
        completion = nil
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        completion?(.failure(describe(error: error)))
        completion = nil
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        completion?(.failure(ActivationError.requiresUserApproval))
    }

    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties, withExtension replacement: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        return .replace
    }

    private func describe(error: Error) -> NSError {
        let nsError = error as NSError
        guard nsError.domain == OSSystemExtensionErrorDomain,
              let code = OSSystemExtensionError.Code(rawValue: nsError.code) else {
            return nsError
        }

        let message: String
        switch code {
        case .unknown:
            message = "Unknown system extension error"
        case .missingEntitlement:
            message = "Missing required system extension entitlement"
        case .unsupportedParentBundleLocation:
            message = "App must be launched from a supported bundle location"
        case .extensionNotFound:
            message = "System extension not found inside the running app bundle"
        case .extensionMissingIdentifier:
            message = "System extension bundle identifier is missing"
        case .duplicateExtensionIdentifer:
            message = "Duplicate system extension identifier found in app bundle"
        case .unknownExtensionCategory:
            message = "Unknown system extension category"
        case .codeSignatureInvalid:
            message = "System extension code signature is invalid"
        case .validationFailed:
            message = "System extension validation failed"
        case .forbiddenBySystemPolicy:
            message = "System policy blocked the system extension"
        case .requestCanceled:
            message = "System extension request was canceled"
        case .requestSuperseded:
            message = "System extension request was superseded by a newer request"
        case .authorizationRequired:
            message = "System extension authorization is required"
        @unknown default:
            message = nsError.localizedDescription
        }

        return NSError(domain: nsError.domain,
                       code: nsError.code,
                       userInfo: [NSLocalizedDescriptionKey: "\(message) (\(nsError.domain) error \(nsError.code))"])
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
