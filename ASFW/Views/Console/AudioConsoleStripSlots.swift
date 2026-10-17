import SwiftUI

/// Keeps every compact console strip on the same control/fader grid.  The
/// shell, header, button treatment, and these two slots originate in the
/// M-Audio console; device families only fill them with semantic controls.
struct AudioConsoleStripSlots<Controls: View, FaderBlock: View>: View {
    let controlHeight: CGFloat
    let faderHeight: CGFloat
    @ViewBuilder let controls: () -> Controls
    @ViewBuilder let faderBlock: () -> FaderBlock

    var body: some View {
        VStack(spacing: 6) {
            controls()
                .frame(maxWidth: .infinity, minHeight: controlHeight,
                       maxHeight: controlHeight, alignment: .top)
            faderBlock()
                .frame(maxWidth: .infinity, minHeight: faderHeight,
                       maxHeight: faderHeight, alignment: .bottom)
        }
    }
}
