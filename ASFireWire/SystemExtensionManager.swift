import Foundation
import SystemExtensions
import os.log

final class SystemExtensionManager: NSObject, OSSystemExtensionRequestDelegate {
    private static let logger = Logger(subsystem: "net.mrmidi.ASFireWire", category: "SystemExtensions")

    // Use the actual DriverKit extension bundle identifier from the project
    // Adjust here if you change the dext target's bundle id in Xcode
    private let dextIdentifier = "net.mrmidi.ASFireWire.ASOHCI"

    private var completion: ((String) -> Void)?

    func activateDriver(completion: @escaping (String) -> Void) {
        self.completion = completion

        completion("Creating activation request…")
        Self.logger.info("Creating activation request for \(self.dextIdentifier)")

        let request = OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: self.dextIdentifier, queue: .main)
        request.delegate = self

        completion("Submitting request to system…")
        Self.logger.info("Submitting activation request")
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func deactivateDriver(completion: @escaping (String) -> Void) {
        self.completion = completion

        completion("Creating deactivation request…")
        Self.logger.info("Creating deactivation request for \(self.dextIdentifier)")

        let request = OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: self.dextIdentifier, queue: .main)
        request.delegate = self

        completion("Submitting request to system…")
        Self.logger.info("Submitting deactivation request")
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    // MARK: OSSystemExtensionRequestDelegate

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        let msg = (result == .completed) ? "Driver activated successfully" : "Driver will complete after reboot"
        Self.logger.info("Request finished: \(result.rawValue)")
        completion?("✅ Success: \(msg)")
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        Self.logger.error("Request failed: \(error.localizedDescription)")
        completion?("❌ Failed: \(error.localizedDescription)")
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        Self.logger.info("Request needs user approval")
        completion?("⚠️ Needs user approval in System Settings → Privacy & Security")
    }

    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension replacement: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        Self.logger.info("Replacing existing extension")
        completion?("🔄 Replacing existing extension…")
        return .replace
    }
}

