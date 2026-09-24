import Testing
@testable import ASFW

struct BeBoBShellCommandPolicyTests {
    @Test func onlyReadOnlyDiagnosticCommandsAreAccepted() {
        #expect(ASFWMCPBeBoBShellClient.supports("sys stat"))
        #expect(ASFWMCPBeBoBShellClient.supports("sys avstat all"))
        #expect(ASFWMCPBeBoBShellClient.supports("fw show"))
        #expect(ASFWMCPBeBoBShellClient.supports("fw mix show"))
        #expect(ASFWMCPBeBoBShellClient.supports("fw vol peak"))
        #expect(!ASFWMCPBeBoBShellClient.supports("fw mix connect i14s1 lineout1"))
        #expect(!ASFWMCPBeBoBShellClient.supports("sys avstat clr all"))
        #expect(!ASFWMCPBeBoBShellClient.supports("fw flash"))
    }
}
