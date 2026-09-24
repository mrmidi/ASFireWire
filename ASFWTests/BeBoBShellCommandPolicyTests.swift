import Testing
@testable import ASFW

struct BeBoBShellCommandPolicyTests {
    @Test func acceptsSinglePrintableShellLines() {
        #expect(ASFWMCPBeBoBShellClient.supports("sys stat"))
        #expect(ASFWMCPBeBoBShellClient.supports("sys avstat all"))
        #expect(ASFWMCPBeBoBShellClient.supports("fw show"))
        #expect(ASFWMCPBeBoBShellClient.supports("fw mix show"))
        #expect(ASFWMCPBeBoBShellClient.supports("fw vol peak"))
        #expect(ASFWMCPBeBoBShellClient.supports("fw mix connect i14s1 lineout1"))
        #expect(ASFWMCPBeBoBShellClient.supports("sys avstat clr all"))
        #expect(!ASFWMCPBeBoBShellClient.supports(""))
        #expect(!ASFWMCPBeBoBShellClient.supports("fw show\nfw flash"))
        #expect(!ASFWMCPBeBoBShellClient.supports("fw show\u{00}"))
        #expect(!ASFWMCPBeBoBShellClient.supports(String(repeating: "x", count: 1025)))
    }
}
