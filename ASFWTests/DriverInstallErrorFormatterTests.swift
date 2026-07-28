import Foundation
import SystemExtensions
import Testing
@testable import ASFW

struct DriverInstallErrorFormatterTests {
    @Test func describesInvalidSignatureWithActionableContext() {
        let error = NSError(
            domain: OSSystemExtensionErrorDomain,
            code: OSSystemExtensionError.Code.codeSignatureInvalid.rawValue
        )

        let described = DriverInstallErrorFormatter.describe(error: error)

        #expect(described.domain == OSSystemExtensionErrorDomain)
        #expect(described.code == OSSystemExtensionError.Code.codeSignatureInvalid.rawValue)
        #expect(described.localizedDescription.contains("code signature is invalid"))
        #expect(described.localizedDescription.contains("OSSystemExtensionErrorDomain"))
    }

    @Test func preservesErrorsOutsideSystemExtensionDomain() {
        let error = NSError(
            domain: NSCocoaErrorDomain,
            code: NSFileReadNoSuchFileError,
            userInfo: [NSLocalizedDescriptionKey: "Missing bundle"]
        )

        let described = DriverInstallErrorFormatter.describe(error: error)

        #expect(described === error)
        #expect(described.localizedDescription == "Missing bundle")
    }
}

struct DriverInstallResultFormatterTests {
    @Test func describesCompletedActivation() {
        let description = DriverInstallResultFormatter.describe(
            operation: "Activation",
            result: .completed
        )

        #expect(description == "Activation completed")
    }

    @Test func tellsTheUserToRestartForDeferredActivation() {
        let description = DriverInstallResultFormatter.describe(
            operation: "Activation",
            result: .willCompleteAfterReboot
        )

        #expect(description.contains("Activation accepted"))
        #expect(description.contains("Restart macOS"))
    }
}
