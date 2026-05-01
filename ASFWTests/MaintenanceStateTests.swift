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

    @Test func activeSystemExtensionWithoutLoadedDriverServiceIsNotCdHashMismatch() {
        let summary = ASFWMaintenanceParser.summarize(systemExtensions: activeSystemExtension,
                                                      ioregOutput: "",
                                                      coreAudioOutput: "Built-in Output",
                                                      expectedCDHash: "newhash",
                                                      driverBundleID: driverID,
                                                      expectedCoreAudioDeviceName: nil)

        #expect(summary.health == .repairNeeded)
        #expect(summary.activeDriver)
        #expect(!summary.driverServiceLoaded)
        #expect(summary.activeCDHash == nil)
        #expect(summary.message == "ASFW system extension is installed, but the driver service is not loaded.")
    }

    @Test func cdHashParserFiltersByDriverBundleID() {
        let ioreg = """
        +-o ASFWDriver  <class IOUserService>
          | {
          |   "CFBundleIdentifier" = "com.old.ASFW.ASFWDriver"
          |   "IOUserServerCDHash" = "oldhash"
          | }
        +-o ASFWDriver  <class IOUserService>
          | {
          |   "CFBundleIdentifier" = "com.chrisizatt.ASFWLocal.ASFWDriver"
          |   "IOUserServerCDHash" = "newhash"
          | }
        """

        #expect(ASFWMaintenanceParser.activeCDHash(ioregOutput: ioreg, driverBundleID: driverID) == "newhash")
        #expect(ASFWMaintenanceParser.activeCDHash(ioregOutput: ioreg, driverBundleID: "com.missing.ASFWDriver") == nil)
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

    @Test func timedOutCoreAudioProbeDoesNotTriggerRepairWhenAudioNubExists() {
        let status = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "abc123""#,
            audioNubIoreg: "ASFWAudioNub",
            coreAudioOutput: ASFWMaintenanceParser.probeTimeoutMarker,
            expectedCDHash: "abc123",
            helperStatus: .enabled
        ))

        #expect(status.health == .clean)
        #expect(status.recommendedAction == .none)
        #expect(status.coreAudioProbeUnavailable)
        #expect(!status.coreAudioDeviceVisible)
        #expect(status.isCleanForAudio)
    }

    @Test func driverOnlyTesterHealthDoesNotRequireAlesisCoreAudio() {
        let status = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "abc123""#,
            audioNubIoreg: "",
            coreAudioOutput: "Built-in Output",
            expectedCDHash: "abc123",
            expectedCoreAudioDeviceName: nil
        ))

        #expect(status.health == .clean)
        #expect(status.recommendedAction == .none)
        #expect(status.coreAudioDeviceVisible)
        #expect(status.summary == "ASFW driver is working.")
    }

    @Test func lifecycleMapsInstalledButNotLoadedDriverToReconnectNotRepair() {
        let status = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: "",
            audioNubIoreg: "",
            coreAudioOutput: "Built-in Output",
            expectedCDHash: "abc123",
            expectedCoreAudioDeviceName: nil
        ))

        #expect(status.health == .repairNeeded)
        #expect(status.recommendedAction == .reconnectDevice)
        #expect(status.activeDriver)
        #expect(!status.driverServiceLoaded)
        #expect(status.activeCDHash == nil)
        #expect(status.summary == "ASFW system extension is installed, but the driver service is not loaded.")
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

    @Test func lifecycleDoesNotTreatStaleCoreAudioAsHealthyWhenAudioNubMissing() {
        let status = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "abc123""#,
            audioNubIoreg: "",
            coreAudioOutput: "Alesis MultiMix Firewire",
            expectedCDHash: "abc123",
            helperStatus: .enabled
        ))

        #expect(status.health == .repairNeeded)
        #expect(status.recommendedAction == .reconnectDevice)
        #expect(!status.audioNubVisible)
        #expect(status.coreAudioDeviceVisible)
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

    @Test func recommendedActionMetadataSeparatesAppActionsFromManualSteps() {
        #expect(MaintenanceRecommendedAction.repairOnce.buttonTitle == "Repair Once")
        #expect(MaintenanceRecommendedAction.captureDiagnostics.buttonTitle == "Capture Diagnostics")
        #expect(MaintenanceRecommendedAction.repairOnce.isDirectlyActionableInApp)
        #expect(MaintenanceRecommendedAction.approveHelper.isDirectlyActionableInApp)
        #expect(!MaintenanceRecommendedAction.reboot.isDirectlyActionableInApp)
        #expect(!MaintenanceRecommendedAction.reconnectDevice.isDirectlyActionableInApp)
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
        let repairStatus = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "expected-hash" ASFWAudioNub"#,
            audioNubIoreg: "ASFWAudioNub",
            coreAudioOutput: "Mac Studio Speakers",
            expectedCDHash: "expected-hash",
            helperStatus: .enabled
        ))
        let sequence = LifecycleProbeSequence([.init(status: repairStatus, delay: 0)])
        let viewModel = DriverViewModel(maintenanceHelper: helper,
                                        lifecycleProbe: sequence.probe,
                                        mismatchConfirmationDelay: 0.01)
        viewModel.lifecycleStatus = repairStatus
        var disconnected = false
        var reconnected = false
        viewModel.setMaintenanceConnectionHandlers {
            disconnected = true
        } reconnect: {
            reconnected = true
        }

        viewModel.repairDriver()
        try? await Task.sleep(nanoseconds: 80_000_000)

        #expect(disconnected)
        #expect(reconnected)
        #expect(helper.refreshCallCount == 1)
        #expect(helper.lastExpectedCDHash == "expected-hash")
        #expect(viewModel.activationStatus == "Repair completed: clean")
        #expect(viewModel.lastMaintenanceSnapshotPath == "/Users/Shared/ASFW/Diagnostics/clean")
        #expect(!viewModel.isBusy)
    }

    @Test @MainActor func repairFailureReportsRebootRequired() async {
        let helper = FakeMaintenanceHelper(status: .enabled)
        helper.refreshOutcome = .rebootRequired(message: "stale uninstall", snapshotPath: "/Users/Shared/ASFW/Diagnostics/stale")
        let repairStatus = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "abc123" ASFWAudioNub"#,
            audioNubIoreg: "ASFWAudioNub",
            coreAudioOutput: "Mac Studio Speakers",
            expectedCDHash: "abc123",
            helperStatus: .enabled
        ))
        let sequence = LifecycleProbeSequence([.init(status: repairStatus, delay: 0)])
        let viewModel = DriverViewModel(maintenanceHelper: helper,
                                        lifecycleProbe: sequence.probe,
                                        mismatchConfirmationDelay: 0.01)
        viewModel.lifecycleStatus = repairStatus

        viewModel.repairDriver()
        try? await Task.sleep(nanoseconds: 80_000_000)

        #expect(viewModel.activationStatus == "Reboot required: stale uninstall")
        #expect(viewModel.maintenanceStatus == "stale uninstall")
        #expect(viewModel.lastMaintenanceSnapshotPath == "/Users/Shared/ASFW/Diagnostics/stale")
        #expect(!viewModel.isBusy)
    }

    @Test @MainActor func repairIsDisabledForReconnectOnlyState() {
        let helper = FakeMaintenanceHelper(status: .enabled)
        let viewModel = DriverViewModel(maintenanceHelper: helper)
        viewModel.lifecycleStatus = ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: "",
            audioNubIoreg: "",
            coreAudioOutput: "Built-in Output",
            expectedCDHash: "abc123",
            helperStatus: .enabled,
            expectedCoreAudioDeviceName: nil
        ))

        #expect(viewModel.lifecycleStatus.recommendedAction == .reconnectDevice)
        #expect(!viewModel.canRepairDriver)
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

    @Test @MainActor func recommendedDiagnosticsActionUsesHelperSnapshot() async {
        let helper = FakeMaintenanceHelper(status: .enabled)
        helper.snapshotOutcome = .succeeded(message: "snapshot captured", snapshotPath: "/Users/Shared/ASFW/Diagnostics/recommended")
        let viewModel = DriverViewModel(maintenanceHelper: helper)

        viewModel.performRecommendedAction()
        await Task.yield()

        #expect(viewModel.activationStatus == "snapshot captured")
        #expect(viewModel.lastMaintenanceSnapshotPath == "/Users/Shared/ASFW/Diagnostics/recommended")
        #expect(!viewModel.isBusy)
    }

    @Test @MainActor func staleOlderLifecycleProbeCannotOverwriteCleanResult() async {
        let helper = FakeMaintenanceHelper(status: .enabled)
        let sequence = LifecycleProbeSequence([
            .init(status: mismatchStatus(), delay: 0.12),
            .init(status: cleanStatus(), delay: 0)
        ])
        let viewModel = DriverViewModel(maintenanceHelper: helper,
                                        lifecycleProbe: sequence.probe,
                                        mismatchConfirmationDelay: 0.01)

        viewModel.refreshLifecycleStatus()
        try? await Task.sleep(nanoseconds: 20_000_000)
        viewModel.refreshLifecycleStatus()
        try? await Task.sleep(nanoseconds: 180_000_000)

        #expect(viewModel.lifecycleStatus.health == .clean)
        #expect(viewModel.lifecycleStatus.summary == "ASFW audio is working.")
        #expect(!viewModel.canRepairDriver)
        #expect(sequence.callCount == 2)
    }

    @Test @MainActor func firstDriverMismatchConfirmsBeforeEnablingRepair() async {
        let helper = FakeMaintenanceHelper(status: .enabled)
        let sequence = LifecycleProbeSequence([
            .init(status: mismatchStatus(), delay: 0),
            .init(status: cleanStatus(), delay: 0)
        ])
        let viewModel = DriverViewModel(maintenanceHelper: helper,
                                        lifecycleProbe: sequence.probe,
                                        mismatchConfirmationDelay: 0.2)
        viewModel.lifecycleStatus = cleanStatus()

        viewModel.refreshLifecycleStatus()
        try? await Task.sleep(nanoseconds: 50_000_000)

        #expect(viewModel.lifecycleStatus.health == .clean)
        #expect(viewModel.isConfirmingDriverMismatch)
        #expect(!viewModel.canRepairDriver)
    }

    @Test @MainActor func confirmedDriverMismatchEnablesRepair() async {
        let helper = FakeMaintenanceHelper(status: .enabled)
        helper.stagedHash = "newhash"
        let sequence = LifecycleProbeSequence([
            .init(status: mismatchStatus(), delay: 0),
            .init(status: mismatchStatus(), delay: 0)
        ])
        let viewModel = DriverViewModel(maintenanceHelper: helper,
                                        lifecycleProbe: sequence.probe,
                                        mismatchConfirmationDelay: 0.01)

        viewModel.refreshLifecycleStatus()
        try? await Task.sleep(nanoseconds: 100_000_000)

        #expect(viewModel.lifecycleStatus.health == .repairNeeded)
        #expect(viewModel.lifecycleStatus.summary == "Driver update may not have settled yet.")
        #expect(viewModel.canRepairDriver)
    }

    @Test @MainActor func repairRechecksAndCancelsWhenStateBecameHealthy() async {
        let helper = FakeMaintenanceHelper(status: .enabled)
        helper.stagedHash = "abc123"
        let sequence = LifecycleProbeSequence([
            .init(status: cleanStatus(), delay: 0)
        ])
        let viewModel = DriverViewModel(maintenanceHelper: helper,
                                        lifecycleProbe: sequence.probe,
                                        mismatchConfirmationDelay: 0.01)
        viewModel.lifecycleStatus = coreAudioMissingRepairStatus()

        viewModel.repairDriver()
        try? await Task.sleep(nanoseconds: 80_000_000)

        #expect(viewModel.lifecycleStatus.health == .clean)
        #expect(viewModel.activationStatus == "Audio path is already healthy.")
        #expect(helper.refreshCallCount == 0)
        #expect(!viewModel.isBusy)
    }

    @Test @MainActor func recoveredHelperErrorDoesNotRemainTopLevelFailure() async {
        let helper = FakeMaintenanceHelper(status: .enabled)
        helper.stagedHash = "abc123"
        let sequence = LifecycleProbeSequence([
            .init(status: cleanStatus(), delay: 0)
        ])
        let viewModel = DriverViewModel(maintenanceHelper: helper,
                                        lifecycleProbe: sequence.probe,
                                        mismatchConfirmationDelay: 0.01)
        viewModel.activationStatus = "Error: Couldn't communicate with a helper application."

        viewModel.refreshLifecycleStatus()
        try? await Task.sleep(nanoseconds: 80_000_000)

        #expect(viewModel.lifecycleStatus.health == .clean)
        #expect(viewModel.activationStatus == "Driver is active.")
        #expect(viewModel.maintenanceStatus.contains("Helper-only maintenance actions"))
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
                        stagedDriverPresent: Bool = true,
                        expectedCoreAudioDeviceName: String? = "Alesis MultiMix Firewire") -> MaintenanceLifecycleInputs {
        MaintenanceLifecycleInputs(
            isRunningFromApplications: isRunningFromApplications,
            helperStatus: helperStatus,
            userClientConnected: userClientConnected,
            systemExtensions: systemExtensions,
            driverIoreg: driverIoreg,
            audioNubIoreg: audioNubIoreg,
            coreAudioOutput: coreAudioOutput,
            expectedCDHash: expectedCDHash,
            expectedCoreAudioDeviceName: expectedCoreAudioDeviceName,
            stagedDriverPresent: stagedDriverPresent,
            driverBundleID: driverID
        )
    }

    private func cleanStatus() -> MaintenanceLifecycleStatus {
        ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "abc123" ASFWAudioNub"#,
            audioNubIoreg: "ASFWAudioNub",
            coreAudioOutput: "Alesis MultiMix Firewire",
            expectedCDHash: "abc123",
            helperStatus: .enabled
        ))
    }

    private func mismatchStatus() -> MaintenanceLifecycleStatus {
        ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "oldhash" ASFWAudioNub"#,
            audioNubIoreg: "ASFWAudioNub",
            coreAudioOutput: "Alesis MultiMix Firewire",
            expectedCDHash: "newhash",
            helperStatus: .enabled
        ))
    }

    private func coreAudioMissingRepairStatus() -> MaintenanceLifecycleStatus {
        ASFWMaintenanceLifecycleEvaluator.evaluate(inputs(
            systemExtensions: activeSystemExtension,
            driverIoreg: #""IOUserServerCDHash" = "abc123" ASFWAudioNub"#,
            audioNubIoreg: "ASFWAudioNub",
            coreAudioOutput: "Mac Studio Speakers",
            expectedCDHash: "abc123",
            helperStatus: .enabled
        ))
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
    var refreshCallCount = 0

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
        refreshCallCount += 1
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

private final class LifecycleProbeSequence {
    struct Step {
        var status: MaintenanceLifecycleStatus
        var delay: TimeInterval
    }

    private let lock = NSLock()
    private var steps: [Step]
    private(set) var callCount = 0

    init(_ steps: [Step]) {
        self.steps = steps
    }

    func probe(_ isRunningFromApplications: Bool,
               _ helperStatus: MaintenanceHelperApprovalState,
               _ userClientConnected: Bool,
               _ driverBundleID: String,
               _ expectedCDHash: String?) -> MaintenanceLifecycleStatus {
        lock.lock()
        callCount += 1
        let step = steps.isEmpty
            ? Step(status: MaintenanceLifecycleStatus.unknown, delay: 0)
            : steps.removeFirst()
        lock.unlock()

        if step.delay > 0 {
            Thread.sleep(forTimeInterval: step.delay)
        }
        return step.status
    }
}
