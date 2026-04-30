import Foundation
import Testing
@testable import ASFW

struct MaintenanceStateTests {
    private let driverID = "com.chrisizatt.ASFWLocal.ASFWDriver"

    @Test func cleanActiveStateIsHealthy() {
        let systemExtensions = """
        *\t*\t8CZML8FK2D\tcom.chrisizatt.ASFWLocal.ASFWDriver (1.0/16)\tnet.mrmidi.ASFW.ASFWDriver\t[activated enabled]
        """
        let ioreg = #""IOUserServerCDHash" = "abc123""#
        let coreAudio = "Alesis MultiMix Firewire:\nInput Channels: 12\nOutput Channels: 2\nCurrent SampleRate: 48000"

        let summary = ASFWMaintenanceParser.summarize(systemExtensions: systemExtensions,
                                                      ioregOutput: ioreg,
                                                      coreAudioOutput: coreAudio,
                                                      expectedCDHash: "abc123",
                                                      driverBundleID: driverID)

        #expect(summary.health == .clean)
        #expect(summary.activeDriver)
        #expect(summary.coreAudioDeviceVisible)
        #expect(summary.activeCDHash == "abc123")
    }

    @Test func staleTerminatingStateRequiresReboot() {
        let systemExtensions = """
        *\t*\t8CZML8FK2D\tcom.chrisizatt.ASFWLocal.ASFWDriver (1.0/16)\tnet.mrmidi.ASFW.ASFWDriver\t[activated enabled]
        \t\t8CZML8FK2D\tcom.chrisizatt.ASFWLocal.ASFWDriver (1.0/16)\tnet.mrmidi.ASFW.ASFWDriver\t[terminating for uninstall but still running]
        """

        let summary = ASFWMaintenanceParser.summarize(systemExtensions: systemExtensions,
                                                      ioregOutput: "",
                                                      coreAudioOutput: "",
                                                      expectedCDHash: nil,
                                                      driverBundleID: driverID)

        #expect(summary.health == .rebootRequired)
        #expect(summary.staleTerminatingDriver)
    }

    @Test func cdHashMismatchNeedsRepair() {
        let systemExtensions = """
        *\t*\t8CZML8FK2D\tcom.chrisizatt.ASFWLocal.ASFWDriver (1.0/15)\tnet.mrmidi.ASFW.ASFWDriver\t[activated enabled]
        """
        let summary = ASFWMaintenanceParser.summarize(systemExtensions: systemExtensions,
                                                      ioregOutput: #""IOUserServerCDHash" = "oldhash""#,
                                                      coreAudioOutput: "Alesis MultiMix Firewire",
                                                      expectedCDHash: "newhash",
                                                      driverBundleID: driverID)

        #expect(summary.health == .repairNeeded)
        #expect(summary.activeCDHash == "oldhash")
        #expect(summary.expectedCDHash == "newhash")
    }

    @Test func missingDriverAndAudioLooksUninstalled() {
        let summary = ASFWMaintenanceParser.summarize(systemExtensions: "2 extension(s)",
                                                      ioregOutput: "",
                                                      coreAudioOutput: "Mac Studio Speakers",
                                                      expectedCDHash: nil,
                                                      driverBundleID: driverID)

        #expect(summary.health == .uninstalled)
        #expect(!summary.activeDriver)
        #expect(!summary.coreAudioDeviceVisible)
    }

    @Test func cleanAudioWithMissingUserClientIsStillHealthy() {
        let status = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "abc123""#,
            audioNubIoreg: "ASFWAudioNub",
            coreAudioOutput: "Alesis MultiMix Firewire",
            expectedCDHash: "abc123",
            userClientConnected: false
        ))

        #expect(status.health == .clean)
        #expect(status.recommendedAction == .none)
        #expect(status.activeDriver)
        #expect(status.audioNubVisible)
        #expect(status.coreAudioDeviceVisible)
        #expect(!status.userClientConnected)
    }

    @Test func lifecycleMapsCoreAudioMissingToRepairWhenAudioNubExists() {
        let status = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "abc123""#,
            audioNubIoreg: "ASFWAudioNub",
            coreAudioOutput: "Mac Studio Speakers",
            expectedCDHash: "abc123",
            helperStatus: .enabled
        ))

        #expect(status.health == .repairNeeded)
        #expect(status.recommendedAction == .repairOnce)
        #expect(status.audioNubVisible)
        #expect(!status.coreAudioDeviceVisible)
    }

    @Test func lifecycleMapsStaleUninstallToReboot() {
        let status = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: """
            \(activeSystemExtension)
            \t\t8CZML8FK2D\tcom.chrisizatt.ASFWLocal.ASFWDriver (1.0/16)\tnet.mrmidi.ASFW.ASFWDriver\t[terminating for uninstall but still running]
            """,
            driverIoreg: #""IOUserServerCDHash" = "abc123""#,
            audioNubIoreg: "ASFWAudioNub",
            coreAudioOutput: "Alesis MultiMix Firewire",
            expectedCDHash: "abc123"
        ))

        #expect(status.health == .rebootRequired)
        #expect(status.recommendedAction == .reboot)
        #expect(status.staleTerminatingDriver)
    }

    @Test func lifecycleRequiresApplicationsCopyForMaintenance() {
        let status = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            isRunningFromApplications: false,
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "abc123""#,
            audioNubIoreg: "ASFWAudioNub",
            coreAudioOutput: "Alesis MultiMix Firewire",
            expectedCDHash: "abc123"
        ))

        #expect(status.health == .repairNeeded)
        #expect(status.recommendedAction == .moveAppToApplications)
    }

    @Test func lifecycleUsesHelperApprovalActionWhenRepairNeedsHelper() {
        let status = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "oldhash""#,
            audioNubIoreg: "ASFWAudioNub",
            coreAudioOutput: "Alesis MultiMix Firewire",
            expectedCDHash: "newhash",
            helperStatus: .requiresApproval
        ))

        #expect(status.health == .repairNeeded)
        #expect(status.recommendedAction == .approveHelper)
        #expect(status.activeCDHash == "oldhash")
        #expect(status.expectedCDHash == "newhash")
    }

    @Test func lifecycleMapsMissingActiveDriverToInstall() {
        let status = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: "2 extension(s)",
            driverIoreg: "",
            audioNubIoreg: "",
            coreAudioOutput: "Mac Studio Speakers",
            expectedCDHash: "abc123",
            stagedDriverPresent: true
        ))

        #expect(status.health == .uninstalled)
        #expect(status.recommendedAction == .installOrUpdateDriver)
        #expect(status.stagedDriverPresent)
    }

    @Test func helperOutcomeMapsRebootBeforeRepair() {
        let dict: NSDictionary = [
            "ok": NSNumber(value: false),
            "rebootRequired": NSNumber(value: true),
            "repairNeeded": NSNumber(value: true),
            "message": "reboot now",
            "snapshotPath": "/Users/Shared/ASFW/Diagnostics/example"
        ]

        let outcome = MaintenanceHelperManager.outcome(from: dict)

        #expect(outcome == .rebootRequired(message: "reboot now", snapshotPath: "/Users/Shared/ASFW/Diagnostics/example"))
    }

    @Test @MainActor func helperRegistrationRequiringApprovalUpdatesViewModelState() {
        let helper = FakeMaintenanceHelper(status: .notRegistered)
        helper.registerResult = .requiresApproval
        let viewModel = DriverViewModel(maintenanceHelper: helper)

        viewModel.enableMaintenanceHelper()

        #expect(viewModel.helperStatus == .requiresApproval)
        #expect(viewModel.activationStatus == "Maintenance helper needs approval in System Settings.")
        #expect(viewModel.maintenanceStatus == "Maintenance helper is registered but needs approval in System Settings.")
        #expect(!viewModel.isBusy)
    }

    @Test @MainActor func helperEnabledStateAllowsRepair() async {
        let helper = FakeMaintenanceHelper(status: .enabled)
        helper.stagedHash = "expected-hash"
        helper.refreshOutcome = .succeeded(message: "clean", snapshotPath: "/Users/Shared/ASFW/Diagnostics/clean")
        let viewModel = DriverViewModel(maintenanceHelper: helper)
        var disconnected = false
        var reconnected = false
        viewModel.setMaintenanceConnectionHandlers {
            disconnected = true
        } reconnect: {
            reconnected = true
        }

        viewModel.repairDriver()
        await Task.yield()

        #expect(disconnected)
        #expect(reconnected)
        #expect(helper.lastExpectedCDHash == "expected-hash")
        #expect(viewModel.activationStatus == "Repair completed: clean")
        #expect(viewModel.lastMaintenanceSnapshotPath == "/Users/Shared/ASFW/Diagnostics/clean")
        #expect(!viewModel.isBusy)
    }

    @Test @MainActor func repairFailureReportsRebootRequired() async {
        let helper = FakeMaintenanceHelper(status: .enabled)
        helper.refreshOutcome = .rebootRequired(message: "stale uninstall", snapshotPath: "/Users/Shared/ASFW/Diagnostics/stale")
        let viewModel = DriverViewModel(maintenanceHelper: helper)

        viewModel.repairDriver()
        await Task.yield()

        #expect(viewModel.activationStatus == "Reboot required: stale uninstall")
        #expect(viewModel.maintenanceStatus == "stale uninstall")
        #expect(viewModel.lastMaintenanceSnapshotPath == "/Users/Shared/ASFW/Diagnostics/stale")
        #expect(!viewModel.isBusy)
    }

    @Test @MainActor func diagnosticsCaptureUsesHelperSnapshotPath() async {
        let helper = FakeMaintenanceHelper(status: .enabled)
        helper.snapshotOutcome = .succeeded(message: "snapshot captured", snapshotPath: "/Users/Shared/ASFW/Diagnostics/snap")
        let viewModel = DriverViewModel(maintenanceHelper: helper)

        viewModel.captureDiagnostics()
        await Task.yield()

        #expect(viewModel.activationStatus == "snapshot captured")
        #expect(viewModel.maintenanceStatus == "snapshot captured")
        #expect(viewModel.lastMaintenanceSnapshotPath == "/Users/Shared/ASFW/Diagnostics/snap")
        #expect(!viewModel.isBusy)
    }

    private var activeSystemExtension: String {
        """
        *\t*\t8CZML8FK2D\tcom.chrisizatt.ASFWLocal.ASFWDriver (1.0/16)\tnet.mrmidi.ASFW.ASFWDriver\t[activated enabled]
        """
    }

    private func inputs(isRunningFromApplications: Bool = true,
                        systemExtensions: String,
                        driverIoreg: String,
                        audioNubIoreg: String,
                        coreAudioOutput: String,
                        expectedCDHash: String?,
                        helperStatus: MaintenanceHelperApprovalState = .enabled,
                        userClientConnected: Bool = true,
                        stagedDriverPresent: Bool = true) -> MaintenanceLifecycleInputs {
        MaintenanceLifecycleInputs(
            isRunningFromApplications: isRunningFromApplications,
            helperStatus: helperStatus,
            userClientConnected: userClientConnected,
            systemExtensions: systemExtensions,
            driverIoreg: driverIoreg,
            audioNubIoreg: audioNubIoreg,
            coreAudioOutput: coreAudioOutput,
            expectedCDHash: expectedCDHash,
            stagedDriverPresent: stagedDriverPresent,
            driverBundleID: driverID
        )
    }
}

private final class FakeMaintenanceHelper: MaintenanceHelperManaging {
    var helperStatus: MaintenanceHelperApprovalState
    var registerResult: MaintenanceHelperApprovalState = .enabled
    var stagedHash: String?
    var refreshOutcome: MaintenanceOperationOutcome = .succeeded(message: "ok", snapshotPath: nil)
    var cleanupOutcome: MaintenanceOperationOutcome = .succeeded(message: "clean", snapshotPath: nil)
    var probeOutcome: MaintenanceOperationOutcome = .succeeded(message: "probe", snapshotPath: nil)
    var snapshotOutcome: MaintenanceOperationOutcome = .succeeded(message: "snapshot", snapshotPath: nil)
    var lastExpectedCDHash: String?
    var lastDriverBundleID: String?

    init(status: MaintenanceHelperApprovalState) {
        self.helperStatus = status
    }

    func registerHelper() throws -> MaintenanceHelperApprovalState {
        helperStatus = registerResult
        return registerResult
    }

    func openApprovalSettings() {}

    func stagedDriverCDHash(driverBundleID: String) -> String? {
        lastDriverBundleID = driverBundleID
        return stagedHash
    }

    func probeMaintenanceState(completion: @escaping (MaintenanceOperationOutcome) -> Void) {
        completion(probeOutcome)
    }

    func captureHygieneSnapshot(completion: @escaping (MaintenanceOperationOutcome) -> Void) {
        completion(snapshotOutcome)
    }

    func refreshDriver(expectedCDHash: String?,
                       driverBundleID: String,
                       completion: @escaping (MaintenanceOperationOutcome) -> Void) {
        lastExpectedCDHash = expectedCDHash
        lastDriverBundleID = driverBundleID
        completion(refreshOutcome)
    }

    func cleanupAfterUninstall(driverBundleID: String,
                               completion: @escaping (MaintenanceOperationOutcome) -> Void) {
        lastDriverBundleID = driverBundleID
        completion(cleanupOutcome)
    }
}
