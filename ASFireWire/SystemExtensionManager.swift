import Foundation
import SystemExtensions
import os.log

// MARK: - Delegate / State
protocol SystemExtensionManagerDelegate: AnyObject {
    func systemExtensionManager(_ manager: SystemExtensionManager, didUpdateState state: SystemExtensionManager.State)
    func systemExtensionManager(_ manager: SystemExtensionManager, didEmitMessage message: String)
}

final class SystemExtensionManager: NSObject {
    enum State: Equatable {
        case unknown
        case notInstalled
        case installed(enabled: Bool, awaitingApproval: Bool, uninstalling: Bool)
        case error(String)
    }

    enum Action { case activate, deactivate }

    private static let log = Logger(subsystem: "net.mrmidi.ASFireWire", category: "SystemExtensions")
    private let dextIdentifier = "net.mrmidi.ASFireWire.ASOHCI"

    weak var delegate: SystemExtensionManagerDelegate?

    // Concurrency / tracking
    private let internalQueue = DispatchQueue(label: "SystemExtensionManager.serial")
    private var currentRequest: OSSystemExtensionRequest?
    private var pendingAction: Action?

    // MARK: Public API
    func activate() { submit(.activate) }
    func deactivate() { submit(.deactivate) }
    func refreshStatus() { requestProperties() }

    // Backward compatibility wrappers for existing call sites
    func activateDriver(completion: @escaping (String) -> Void) {
        oneShotMessageTap(completion) { self.activate() }
    }
    func deactivateDriver(completion: @escaping (String) -> Void) {
        oneShotMessageTap(completion) { self.deactivate() }
    }

    // MARK: Internal core
    private func submit(_ action: Action) {
        internalQueue.async { [weak self] in
            guard let self else { return }
            guard self.currentRequest == nil else {
                self.emit("A request is already in progress")
                return
            }
            // Pre-flight status check to avoid redundant activation
            self.requestProperties { [weak self] props in
                guard let self else { return }
                if action == .activate, let p = props.first, p.isEnabled {
                    Self.log.debug("Activation skipped: already enabled")
                    self.emit("Extension already active")
                    self.publishState(from: p)
                    return
                }
                self.createAndSubmit(action: action)
            }
        }
    }

    private func createAndSubmit(action: Action) {
        let request: OSSystemExtensionRequest
        switch action {
        case .activate:
            emit("Submitting activation request…")
            request = OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
        case .deactivate:
            emit("Submitting deactivation request…")
            request = OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
        }
        request.delegate = self
        pendingAction = action
        currentRequest = request
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    // MARK: Properties Request / Status
    private func requestProperties(completion: (([OSSystemExtensionProperties]) -> Void)? = nil) {
        // propertiesRequest available since macOS 12 (Monterey)
        guard #available(macOS 12.0, *) else {
            completion?([])
            return
        }
        let req = OSSystemExtensionRequest.propertiesRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
        let delegate = PropertyRequestDelegate { [weak self] props in
            completion?(props)
            self?.publishState(from: props.first)
        }
        req.delegate = delegate
        // Keep the delegate alive until completion by associating via objc_setAssociatedObject
        objc_setAssociatedObject(req, &AssociatedKeys.propDelegateKey, delegate, .OBJC_ASSOCIATION_RETAIN_NONATOMIC)
        OSSystemExtensionManager.shared.submitRequest(req)
    }

    private func publishState(from props: OSSystemExtensionProperties?) {
        let state: State
        if let p = props {
            state = .installed(enabled: p.isEnabled, awaitingApproval: p.isAwaitingUserApproval, uninstalling: p.isUninstalling)
        } else {
            state = .notInstalled
        }
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.systemExtensionManager(self, didUpdateState: state)
        }
    }

    private func finishRequest() {
        internalQueue.async { [weak self] in
            self?.currentRequest = nil
            self?.pendingAction = nil
        }
    }

    private func emit(_ message: String) {
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.systemExtensionManager(self, didEmitMessage: message)
        }
    }

    // Utility to bridge old closure-based API to new delegate
    private func oneShotMessageTap(_ completion: @escaping (String) -> Void, action: () -> Void) {
        // Temporary delegate proxy
        let proxy = ClosureDelegateProxy { completion($0) }
        let previousDelegate = delegate
        delegate = proxy
        action()
        // Restore delegate after short delay (async operations may still send messages)
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            if self?.delegate === proxy { self?.delegate = previousDelegate }
        }
    }

    private struct AssociatedKeys { static var propDelegateKey: UInt8 = 0 }
}

// MARK: - OSSystemExtensionRequestDelegate
extension SystemExtensionManager: OSSystemExtensionRequestDelegate {
    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties, withExtension replacement: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        // Replace only if versions differ
        if existing.bundleVersion != replacement.bundleVersion {
            Self.log.info("Replacing extension version \(existing.bundleVersion) -> \(replacement.bundleVersion)")
            emit("Updating extension (\(existing.bundleShortVersion) → \(replacement.bundleShortVersion))")
            return .replace
        }
        emit("Extension already up to date")
        return .cancel
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        emit("Needs user approval in System Settings → Privacy & Security")
    }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch result {
        case .completed:
            emit("Completed")
        case .willCompleteAfterReboot:
            emit("Will complete after reboot")
        @unknown default:
            emit("Completed with unknown result")
        }
        finishRequest()
        refreshStatus()
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        emit("Failed: \(friendly(error))")
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.delegate?.systemExtensionManager(self, didUpdateState: .error(friendly(error)))
        }
        finishRequest()
    }
}

// MARK: - Error Mapping
private func friendly(_ error: Error) -> String {
    let ns = error as NSError
    guard ns.domain == OSSystemExtensionErrorDomain else { return error.localizedDescription }
    // Attempt to map known codes; raw values used to avoid symbol issues across SDKs
    // Enumerated values (mirroring SDK symbols) guarded by numeric codes for forward compatibility
    // Source: SystemExtensions.framework (values stable across recent macOS versions)
    enum Code: Int {
        case missingEntitlement = 1
        case codeSignatureInvalid = 2
        case notFound = 3
        case forbiddenBySystemPolicy = 4
    }
    switch Code(rawValue: ns.code) {
    case .missingEntitlement:
        return "Missing entitlement (check system-extension & driverkit entitlements)"
    case .codeSignatureInvalid:
        return "Code signature invalid – rebuild with proper signing"
    case .notFound:
        return "Extension bundle not found in host app"
    case .forbiddenBySystemPolicy:
        return "Blocked by system policy – user/MDM approval required"
    case .none:
        return error.localizedDescription
    }
}

// MARK: - Property Request Delegate Wrapper
private final class PropertyRequestDelegate: NSObject, OSSystemExtensionRequestDelegate {
    private let handler: ([OSSystemExtensionProperties]) -> Void
    init(handler: @escaping ([OSSystemExtensionProperties]) -> Void) { self.handler = handler }
    func request(_ request: OSSystemExtensionRequest, foundProperties properties: [OSSystemExtensionProperties]) {
        handler(properties)
    }
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {}
    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) { handler([]) }
    // Provide stubs for other optional callbacks to satisfy protocol if it changes.
    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {}
    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties, withExtension replacement: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction { .cancel }
}

// MARK: - Closure Delegate Proxy (for backwards compatibility wrappers)
private final class ClosureDelegateProxy: NSObject, SystemExtensionManagerDelegate {
    private let sink: (String) -> Void
    init(_ sink: @escaping (String) -> Void) { self.sink = sink }
    func systemExtensionManager(_ manager: SystemExtensionManager, didUpdateState state: SystemExtensionManager.State) { /* no-op for old API */ }
    func systemExtensionManager(_ manager: SystemExtensionManager, didEmitMessage message: String) { sink(message) }
}

// MARK: - Workspace Observation (macOS 15.1+)
@available(macOS 15.1, *)
extension SystemExtensionManager: OSSystemExtensionsWorkspaceObserver {
    func startObservingWorkspace() {
        // API: addObserver returns void (throws via error pointer if provided in older betas). We attempt and ignore errors if the signature mismatches.
        let workspace = OSSystemExtensionsWorkspace.shared
        if (workspace.responds(to: NSSelectorFromString("addObserver:"))) {
            _ = try? workspace.addObserver(self)
        }
    }
    func systemExtensionWillBecomeEnabled(_ info: OSSystemExtensionInfo) { emit("Extension enabled (") ; refreshStatus() }
    func systemExtensionWillBecomeDisabled(_ info: OSSystemExtensionInfo) { emit("Extension disabled") ; refreshStatus() }
    func systemExtensionWillBecomeInactive(_ info: OSSystemExtensionInfo) { emit("Extension inactive") ; refreshStatus() }
}

