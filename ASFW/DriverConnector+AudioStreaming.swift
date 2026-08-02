import Foundation
import IOKit

extension ASFWDriverConnector {
    /// Invoke the full AudioCoordinator lifecycle for one published audio GUID.
    /// The driver owns IRM reservations, PCR connections, AM824 setup, and
    /// rollback; MCP does not assemble those wire actions itself.
    func setAudioStreaming(guid: UInt64, enabled: Bool) -> kern_return_t {
        guard isConnected, connection != 0, guid != 0 else { return kIOReturnNotReady }
        var input = guid
        return IOConnectCallScalarMethod(
            connection,
            (enabled ? Method.startAudioStreaming : Method.stopAudioStreaming).rawValue,
            &input,
            1,
            nil,
            nil
        )
    }
}
