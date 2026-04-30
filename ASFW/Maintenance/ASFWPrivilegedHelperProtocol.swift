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

