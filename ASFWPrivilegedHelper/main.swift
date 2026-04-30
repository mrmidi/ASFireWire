import Foundation

@objc(ASFWPrivilegedHelperProtocol)
protocol ASFWPrivilegedHelperProtocol {
    func helperVersion(_ reply: @escaping (NSDictionary) -> Void)
    func probeMaintenanceState(_ reply: @escaping (NSDictionary) -> Void)
    func captureHygieneSnapshot(_ reply: @escaping (NSDictionary) -> Void)
    func refreshDriver(expectedCDHash: String,
                       driverBundleID: String,
                       reply: @escaping (NSDictionary) -> Void)
    func cleanupAfterUninstall(driverBundleID: String,
                               reply: @escaping (NSDictionary) -> Void)
}

final class HelperDelegate: NSObject, NSXPCListenerDelegate {
    private let service = ASFWPrivilegedHelperService()

    func listener(_ listener: NSXPCListener, shouldAcceptNewConnection newConnection: NSXPCConnection) -> Bool {
        newConnection.exportedInterface = NSXPCInterface(with: ASFWPrivilegedHelperProtocol.self)
        newConnection.exportedObject = service
        newConnection.activate()
        return true
    }
}

let config = ASFWPrivilegedHelperConfig.current
let listener = NSXPCListener(machServiceName: config.machServiceName)
listener.setConnectionCodeSigningRequirement(config.appSigningRequirement)
let delegate = HelperDelegate()
listener.delegate = delegate
listener.activate()
RunLoop.current.run()

