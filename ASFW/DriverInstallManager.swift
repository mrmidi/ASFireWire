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
        let op = (currentOp == .activation ? "Activation" : "Deactivation")
        completion?(.success("\(op) finished with result: \(result.rawValue)"))
        completion = nil
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        completion?(.failure(error))
        completion = nil
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        completion?(.failure(ActivationError.requiresUserApproval))
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
