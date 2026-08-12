import Foundation
import IOKit

extension ASFWDriverConnector {
    /// Invoke the full AudioCoordinator lifecycle for one published endpoint.
    /// The driver owns IRM reservations, PCR connections, AM824 setup, and
    /// rollback; MCP does not assemble those wire actions itself.
    func setAudioStreaming(endpointID: AudioEndpointID, enabled: Bool) -> kern_return_t {
        guard isConnected, connection != 0, endpointID.rawValue != 0 else { return kIOReturnNotReady }
        var input = endpointID.rawValue
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
