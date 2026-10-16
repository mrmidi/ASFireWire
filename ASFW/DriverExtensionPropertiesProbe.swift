import Foundation
import SystemExtensions

/// Asks the SystemExtensions framework which copies of the driver are
/// registered, and compares the active one with the driver bundled in this app.
///
/// It is its own request delegate, separate from DriverInstallManager, so a
/// properties request never reaches the install manager's pending
/// activation/deactivation state. The SDK header does not say whether
/// `didFinishWithResult` follows `foundProperties`, so only `foundProperties`
/// and `didFailWithError` count as answers.
final class DriverExtensionPropertiesProbe: NSObject, OSSystemExtensionRequestDelegate {
    typealias Completion = (DriverExtensionUpdateState) -> Void

    /// The request holds its delegate weakly; this keeps each probe alive
    /// until it answers.
    private static var inFlight: Set<DriverExtensionPropertiesProbe> = []

    private let bundled: BundledDriverExtension?
    private let completion: Completion
    private var answered = false

    private init(bundled: BundledDriverExtension?, completion: @escaping Completion) {
        self.bundled = bundled
        self.completion = completion
    }

    static func check(completion: @escaping Completion) {
        let identifier = DriverInstallManager.extensionIdentifier
        let probe = DriverExtensionPropertiesProbe(
            bundled: bundledDriver(identifier: identifier),
            completion: completion
        )
        inFlight.insert(probe)
        let request = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: identifier,
            queue: .main
        )
        request.delegate = probe
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    /// Reads the dext in Contents/Library/SystemExtensions whose
    /// CFBundleIdentifier matches, as activation would pick it.
    private static func bundledDriver(identifier: String) -> BundledDriverExtension? {
        let directory = Bundle.main.bundleURL.appendingPathComponent(
            "Contents/Library/SystemExtensions",
            isDirectory: true
        )
        guard let entries = try? FileManager.default.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles]
        ) else { return nil }

        for url in entries where url.pathExtension == "dext" {
            guard let data = try? Data(contentsOf: url.appendingPathComponent("Info.plist")),
                  let plist = try? PropertyListSerialization.propertyList(
                    from: data,
                    options: [],
                    format: nil
                  ) as? [String: Any],
                  plist["CFBundleIdentifier"] as? String == identifier,
                  let version = plist["CFBundleVersion"] as? String else { continue }
            return BundledDriverExtension(
                bundleVersion: version,
                bundleShortVersion: plist["CFBundleShortVersionString"] as? String ?? ""
            )
        }
        return nil
    }

    private func answer(_ state: DriverExtensionUpdateState) {
        guard !answered else { return }
        answered = true
        Self.inFlight.remove(self)
        completion(state)
    }

    // MARK: OSSystemExtensionRequestDelegate

    func request(_ request: OSSystemExtensionRequest,
                 foundProperties properties: [OSSystemExtensionProperties]) {
        let installed = properties.map {
            InstalledDriverExtension(
                bundleVersion: $0.bundleVersion,
                bundleShortVersion: $0.bundleShortVersion,
                isEnabled: $0.isEnabled,
                isAwaitingUserApproval: $0.isAwaitingUserApproval,
                isUninstalling: $0.isUninstalling
            )
        }
        answer(DriverExtensionUpdatePolicy.state(installed: installed, bundled: bundled))
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFailWithError error: Error) {
        print("[DriverInstall] Properties request failed: \(error.localizedDescription)")
        answer(.unknown)
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {}

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {}

    func request(
        _ request: OSSystemExtensionRequest,
        actionForReplacingExtension existing: OSSystemExtensionProperties,
        withExtension replacement: OSSystemExtensionProperties
    ) -> OSSystemExtensionRequest.ReplacementAction {
        .cancel
    }
}
