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

    private let internalQueue = DispatchQueue(label: "SystemExtensionManager.serial")
    private var currentRequest: OSSystemExtensionRequest?
    private var pendingAction: Action?

    // MARK: Public API
    func activate() { submit(.activate) }
    func deactivate() { submit(.deactivate) }
    func refreshStatus() { requestProperties() }

    func activateDriver(completion: @escaping (String) -> Void) { oneShotMessageTap(completion) { self.activate() } }
    func deactivateDriver(completion: @escaping (String) -> Void) { oneShotMessageTap(completion) { self.deactivate() } }

    private func submit(_ action: Action) {
        internalQueue.async { [weak self] in
            guard let self else { return }
            guard self.currentRequest == nil else { self.emit("A request is already in progress"); return }
            self.requestProperties { [weak self] props in
                guard let self else { return }
                if action == .activate, let p = props.first, p.isEnabled {
                    Self.log.debug("Activation skipped: already enabled")
                    self.emit("Extension already active")
                    self.publishState(from: p)
                    return
                }
                if action == .deactivate, let p = props.first, p.isUninstalling {
                    self.emit("Already uninstalling; will finish" + (p.isEnabled ? " after reboot" : ""))
                    self.publishState(from: p)
                    return
                }
                self.createAndSubmit(action: action)
            }
        }
    }

    private func createAndSubmit(action: Action) {
        let req: OSSystemExtensionRequest
        switch action {
        case .activate:
            emit("Submitting activation request…")
            req = OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
        case .deactivate:
            emit("Submitting deactivation request…")
            req = OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
        }
        req.delegate = self
        pendingAction = action
        currentRequest = req
        OSSystemExtensionManager.shared.submitRequest(req)
        if #available(macOS 15.1, *) { try? OSSystemExtensionsWorkspace.shared.addObserver(self) }
    }

    private func requestProperties(completion: (([OSSystemExtensionProperties]) -> Void)? = nil) {
        guard #available(macOS 12.0, *) else { completion?([]); return }
        let req = OSSystemExtensionRequest.propertiesRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
        let delegate = PropertyRequestDelegate { [weak self] props in
            let sorted = props.sorted { lhs, rhs in
                if lhs.isEnabled != rhs.isEnabled { return lhs.isEnabled && !rhs.isEnabled }
                if lhs.isAwaitingUserApproval != rhs.isAwaitingUserApproval { return lhs.isAwaitingUserApproval && !rhs.isAwaitingUserApproval }
                return (lhs.bundleVersion as NSString).compare(rhs.bundleVersion, options: .numeric) == .orderedDescending
            }
            completion?(sorted)
            self?.publishState(from: sorted.first)
        }
        req.delegate = delegate
        objc_setAssociatedObject(req, &AssociatedKeys.propDelegateKey, delegate, .OBJC_ASSOCIATION_RETAIN_NONATOMIC)
        OSSystemExtensionManager.shared.submitRequest(req)
    }

    private func publishState(from props: OSSystemExtensionProperties?) {
        let state: State = {
            guard let p = props else { return .notInstalled }
            if #available(macOS 12.0, *) { return .installed(enabled: p.isEnabled, awaitingApproval: p.isAwaitingUserApproval, uninstalling: p.isUninstalling) }
            return .installed(enabled: true, awaitingApproval: false, uninstalling: false)
        }()
        DispatchQueue.main.async { [weak self] in self?.delegate?.systemExtensionManager(self!, didUpdateState: state) }
    }

    private func finishRequest() { internalQueue.async { [weak self] in self?.currentRequest = nil; self?.pendingAction = nil } }

    private func emit(_ message: String) { DispatchQueue.main.async { [weak self] in self?.delegate?.systemExtensionManager(self!, didEmitMessage: message) } }

    private func oneShotMessageTap(_ completion: @escaping (String) -> Void, action: () -> Void) {
        let proxy = ClosureDelegateProxy { completion($0) }
        let prev = delegate
        delegate = proxy
        action()
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in if self?.delegate === proxy { self?.delegate = prev } }
    }

    private struct AssociatedKeys { static var propDelegateKey: UInt8 = 0 }
}

extension SystemExtensionManager: OSSystemExtensionRequestDelegate {
    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties, withExtension replacement: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        if existing.bundleVersion != replacement.bundleVersion {
            Self.log.info("Replacing extension \(existing.bundleShortVersion) → \(replacement.bundleShortVersion)")
            emit("Updating extension (\(existing.bundleShortVersion) → \(replacement.bundleShortVersion))")
            return .replace
        }
        emit("Extension already up to date")
        return .cancel
    }
    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) { emit("Needs user approval in System Settings → Privacy & Security") }
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch result { case .completed: emit("Completed"); case .willCompleteAfterReboot: emit("Will complete after reboot"); @unknown default: emit("Completed with unknown result") }
        finishRequest(); refreshStatus()
    }
    func request(_ request: OSSystemExtensionRequest, didFailWithError error: any Error) {
        let msg = friendly(error); emit("Failed: \(msg)"); DispatchQueue.main.async { [weak self] in self?.delegate?.systemExtensionManager(self!, didUpdateState: .error(msg)) }; finishRequest()
    }
}

private func friendly(_ error: Error) -> String {
    let ns = error as NSError
    guard ns.domain == OSSystemExtensionErrorDomain, let code = OSSystemExtensionError.Code(rawValue: ns.code) else { return error.localizedDescription }
    switch code {
    case .missingEntitlement: return "Missing entitlement (system-extension / DriverKit)"
    case .unsupportedParentBundleLocation: return "Unsupported parent bundle location"
    case .extensionNotFound: return "Extension bundle not found in host app"
    case .extensionMissingIdentifier: return "Extension missing bundle identifier"
    case .duplicateExtensionIdentifer: return "Duplicate extension identifier"
    case .unknownExtensionCategory: return "Unknown extension category"
    case .codeSignatureInvalid: return "Code signature invalid – rebuild with proper signing"
    case .validationFailed: return "Validation failed"
    case .forbiddenBySystemPolicy: return "Blocked by system policy – user/MDM approval required"
    case .requestCanceled: return "Request canceled"
    case .requestSuperseded: return "A newer request superseded this one"
    case .authorizationRequired: return "Authorization required"
    case .unknown: fallthrough
    @unknown default: return "Unknown system extension error (\(ns.code))"
    }
}

private final class PropertyRequestDelegate: NSObject, OSSystemExtensionRequestDelegate {
    private let handler: ([OSSystemExtensionProperties]) -> Void
    init(handler: @escaping ([OSSystemExtensionProperties]) -> Void) { self.handler = handler }
    func request(_ request: OSSystemExtensionRequest, foundProperties properties: [OSSystemExtensionProperties]) { handler(properties) }
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {}
    func request(_ request: OSSystemExtensionRequest, didFailWithError error: any Error) { handler([]) }
    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {}
    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties, withExtension replacement: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction { .cancel }
}

private final class ClosureDelegateProxy: NSObject, SystemExtensionManagerDelegate {
    private let sink: (String) -> Void
    init(_ sink: @escaping (String) -> Void) { self.sink = sink }
    func systemExtensionManager(_ manager: SystemExtensionManager, didUpdateState state: SystemExtensionManager.State) {}
    func systemExtensionManager(_ manager: SystemExtensionManager, didEmitMessage message: String) { sink(message) }
}

@available(macOS 15.1, *)
extension SystemExtensionManager: OSSystemExtensionsWorkspaceObserver {
    func systemExtensionWillBecomeEnabled(_ info: OSSystemExtensionInfo) { emit("Extension enabled"); refreshStatus() }
    func systemExtensionWillBecomeDisabled(_ info: OSSystemExtensionInfo) { emit("Extension disabled"); refreshStatus() }
    func systemExtensionWillBecomeInactive(_ info: OSSystemExtensionInfo) { emit("Extension inactive (uninstalling)"); refreshStatus() }
}

