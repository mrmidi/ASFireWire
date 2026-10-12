import Foundation
import IOKit

extension ASFWDriverConnector {
    // MARK: - MOTU Settings

    /// UserDefaults key for "Enable Core Audio volume controls" (one setting for the MOTU family).
    static let motuCoreAudioVolumeControlsKey = "motu.coreAudioVolumeControls"

    /// The stored choice; on when never set, matching the driver's default.
    static var storedMotuCoreAudioVolumeControls: Bool {
        UserDefaults.standard.object(forKey: motuCoreAudioVolumeControlsKey) as? Bool ?? true
    }

    func setMotuCoreAudioVolumeControls(enabled: Bool) -> Bool {
        guard isConnected else {
            log("setMotuCoreAudioVolumeControls: Not connected", level: .warning)
            return false
        }

        var input: UInt64 = enabled ? 1 : 0
        let kr = IOConnectCallScalarMethod(
            connection,
            ASFWDriverConnector.Method.setCoreAudioVolumeControls.rawValue,
            &input,
            1,
            nil,
            nil
        )

        guard kr == KERN_SUCCESS else {
            log("setMotuCoreAudioVolumeControls failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }

        log("MOTU Core Audio volume controls \(enabled ? "enabled" : "disabled")", level: .success)
        return true
    }

    /// The driver cannot read the app's preferences and starts with its default, so the
    /// stored choice is sent on every connection.
    func applyStoredMotuSettings() {
        let enabled = Self.storedMotuCoreAudioVolumeControls
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            _ = self?.setMotuCoreAudioVolumeControls(enabled: enabled)
        }
    }
}
