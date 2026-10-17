import Testing
@testable import ASFW

struct DriverExtensionUpdatePolicyTests {
    private let bundled = BundledDriverExtension(bundleVersion: "120", bundleShortVersion: "0.4.1")

    private func installed(_ version: String,
                           enabled: Bool = true,
                           awaiting: Bool = false,
                           uninstalling: Bool = false) -> InstalledDriverExtension {
        InstalledDriverExtension(bundleVersion: version,
                                 bundleShortVersion: "0.4.0",
                                 isEnabled: enabled,
                                 isAwaitingUserApproval: awaiting,
                                 isUninstalling: uninstalling)
    }

    @Test func olderEnabledBuildIsReported() {
        let old = installed("99")
        #expect(DriverExtensionUpdatePolicy.state(installed: [old], bundled: bundled)
                == .installedOlder(installed: old, bundled: bundled))
    }

    @Test func equalOrNewerBuildIsCurrent() {
        #expect(DriverExtensionUpdatePolicy.state(installed: [installed("120")], bundled: bundled) == .current)
        #expect(DriverExtensionUpdatePolicy.state(installed: [installed("121")], bundled: bundled) == .current)
    }

    @Test func buildNumbersCompareNumerically() {
        let old = installed("99")
        #expect(DriverExtensionUpdatePolicy.state(installed: [old],
                                                 bundled: BundledDriverExtension(bundleVersion: "100", bundleShortVersion: ""))
                == .installedOlder(installed: old, bundled: BundledDriverExtension(bundleVersion: "100", bundleShortVersion: "")))
    }

    @Test func uninstallingCopiesAreIgnored() {
        let state = DriverExtensionUpdatePolicy.state(
            installed: [installed("90", enabled: false, uninstalling: true), installed("120")],
            bundled: bundled)
        #expect(state == .current)
        #expect(DriverExtensionUpdatePolicy.state(
            installed: [installed("90", uninstalling: true)], bundled: bundled) == .notInstalled)
    }

    @Test func copyAwaitingApprovalIsCompared() {
        let pending = installed("110", enabled: false, awaiting: true)
        #expect(DriverExtensionUpdatePolicy.state(installed: [pending], bundled: bundled)
                == .installedOlder(installed: pending, bundled: bundled))
    }

    @Test func missingDataIsUnknown() {
        #expect(DriverExtensionUpdatePolicy.state(installed: nil, bundled: bundled) == .unknown)
        #expect(DriverExtensionUpdatePolicy.state(installed: [installed("1")], bundled: nil) == .unknown)
        #expect(DriverExtensionUpdatePolicy.state(installed: [], bundled: bundled) == .notInstalled)
    }
}
