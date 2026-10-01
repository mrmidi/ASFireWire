import SwiftUI

/// Every manual AV/C rescan asks first. Discovery sends dozens of commands to
/// each device; an unproven one can make a device glitch, go silent or reset
/// the bus (a Phase 88 did, on a bare UNIT INFO), and a reset mid-playback can
/// send a loud burst to whatever is connected.
enum AvcProbeConfirmation {
    static let title = "Turn down speakers and headphones first"
    static let message = """
        Refresh sends AV/C discovery commands to every device. A device may glitch, \
        stop answering, or reset the FireWire bus, which can produce a loud noise.

        Before you continue: stop playback, turn monitors and headphones down or \
        disconnect them. If a device stops responding, power-cycle it.
        """
    static let confirmLabel = "Refresh Anyway"
}

extension View {
    func avcProbeConfirmation(isPresented: Binding<Bool>, onConfirm: @escaping () -> Void) -> some View {
        confirmationDialog(AvcProbeConfirmation.title, isPresented: isPresented, titleVisibility: .visible) {
            Button(AvcProbeConfirmation.confirmLabel, role: .destructive, action: onConfirm)
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(AvcProbeConfirmation.message)
        }
    }
}
