import Foundation

// Stage 6 (dev-only): continuous (not bounded-to-100ms) silent isoch TX
// cadence, for device protocols that implement IDeviceProtocol's continuous-
// playback hooks (currently only RME Fireface 800). Reachable only through
// this developer-gated MCP path -- never through real CoreAudio StartIO,
// which must keep returning kIOReturnUnsupported.

extension ASFWMCPToolCatalog {
    static let continuousTxTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(
            name: "asfw_continuous_tx_start_dev",
            group: "isoch_dev",
            visibility: .developerWrite,
            readOnly: false,
            idempotent: false,
            summary: "Start a continuous (unbounded) silent isoch TX cadence on the held playback route (targetGuid required). Never enables real Core Audio streaming."
        ),
        ASFWMCPToolDefinition(
            name: "asfw_continuous_tx_stop_dev",
            group: "isoch_dev",
            visibility: .developerWrite,
            readOnly: false,
            idempotent: true,
            summary: "Stop an active continuous silent isoch TX cadence and release its held IRM route (targetGuid required)."
        ),
        ASFWMCPToolDefinition(
            name: "asfw_continuous_tx_health_dev",
            group: "isoch_dev",
            visibility: .readOnly,
            readOnly: true,
            idempotent: false,
            summary: "Non-blocking health snapshot of an active continuous silent isoch TX cadence (targetGuid required)."
        )
    ]
}

struct ASFWMCPContinuousTxReceipt: Equatable {
    let targetGuid: UInt64
    let started: Bool
    let status: Int32

    var ok: Bool { status == 0 }

    var mcpValue: ASFWMCPValue {
        .object([
            "targetGuid": .string(String(format: "0x%016llX", targetGuid)),
            "action": .string(started ? "start" : "stop"),
            "status": .int(Int(status)),
            "ok": .bool(ok),
        ])
    }
}

struct ASFWMCPContinuousTxHealthReceipt: Equatable {
    let targetGuid: UInt64
    let health: ASFWDriverConnector.ContinuousIsochTxHealth?
    let status: Int32

    var ok: Bool { status == 0 }

    var mcpValue: ASFWMCPValue {
        guard let health else {
            return .object([
                "targetGuid": .string(String(format: "0x%016llX", targetGuid)),
                "status": .int(Int(status)),
                "ok": .bool(false),
            ])
        }
        return .object([
            "targetGuid": .string(String(format: "0x%016llX", targetGuid)),
            "status": .int(Int(status)),
            "ok": .bool(ok),
            "running": .bool(health.running),
            "dead": .bool(health.dead),
            "eventError": .bool(health.eventError),
            "anchorExecutions": .int(Int(health.anchorExecutions)),
            "lastAnchorTimestamp": .int(Int(health.lastAnchorTimestamp)),
        ])
    }
}
