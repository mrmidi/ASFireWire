import Testing
@testable import ASFW

struct DriverExtensionHealthPolicyTests {
    private func snapshot(controller: Bool = true,
                          driver: Bool = false,
                          server: Bool = false,
                          associated: Int = 0)
        -> DriverExtensionRegistrySnapshot {
        DriverExtensionRegistrySnapshot(
            controllerPresent: controller,
            driverAttached: driver,
            serverPresent: server,
            associatedServiceCount: associated
        )
    }

    @Test func activationSucceedsOnlyAfterDriverAttachment() {
        let disposition = DriverExtensionHealthPolicy.activationDisposition(
            for: snapshot(driver: true, server: true, associated: 2),
            timedOut: false
        )
        #expect(disposition == .attached)
    }

    @Test func activationWithoutControllerWaitsForHardware() {
        let disposition = DriverExtensionHealthPolicy.activationDisposition(
            for: snapshot(controller: false),
            timedOut: false
        )
        #expect(disposition == .enabledWaitingForController)
    }

    @Test func emptyServerIsOnlyOrphanedAfterTimeout() {
        let state = snapshot(server: true)
        #expect(DriverExtensionHealthPolicy.activationDisposition(
            for: state,
            timedOut: false
        ) == .pending)
        #expect(DriverExtensionHealthPolicy.activationDisposition(
            for: state,
            timedOut: true
        ) == .orphanedServer)
    }

    @Test func missingServerAfterActivationIsAttachFailure() {
        let disposition = DriverExtensionHealthPolicy.activationDisposition(
            for: snapshot(),
            timedOut: true
        )
        #expect(disposition == .didNotAttach)
    }

    @Test func deactivationWaitsForDriverKitServerExit() {
        let state = snapshot(controller: false, server: true)
        #expect(DriverExtensionHealthPolicy.deactivationDisposition(
            for: state,
            timedOut: false
        ) == .pending)
        #expect(DriverExtensionHealthPolicy.deactivationDisposition(
            for: state,
            timedOut: true
        ) == .orphanedServer)
    }

    @Test func deactivationCompletesWhenServiceAndServerAreGone() {
        let disposition = DriverExtensionHealthPolicy.deactivationDisposition(
            for: snapshot(controller: false),
            timedOut: false
        )
        #expect(disposition == .detached)
    }

    @Test func replacementRequiresStrictlyNewerNumericBuild() {
        #expect(DriverExtensionVersionPolicy.replacementIsNewer(
            existing: "1",
            replacement: "2"
        ))
        #expect(DriverExtensionVersionPolicy.replacementIsNewer(
            existing: "9",
            replacement: "10"
        ))
        #expect(!DriverExtensionVersionPolicy.replacementIsNewer(
            existing: "2",
            replacement: "2"
        ))
        #expect(!DriverExtensionVersionPolicy.replacementIsNewer(
            existing: "3",
            replacement: "2"
        ))
    }

    @MainActor
    @Test func liveConnectionOverridesStaleActivationError() {
        let viewModel = DriverViewModel()
        viewModel.activationStatus = "Error: orphaned DriverKit server"

        viewModel.updateDriverConnection(true)

        #expect(viewModel.isDriverConnected)
        #expect(viewModel.displayedStatus ==
                "Driver loaded — UserClient connected")

        viewModel.updateDriverConnection(false)

        #expect(!viewModel.isDriverConnected)
        #expect(viewModel.displayedStatus == "Driver disconnected")
    }
}
