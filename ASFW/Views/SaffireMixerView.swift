//
//  SaffireMixerView.swift
//  ASFW
//
//  Created by ASFireWire Project on 2026-02-08.
//

import SwiftUI

struct SaffireMixerView: View {
    @StateObject private var viewModel: SaffireMixerViewModel
    
    init(connector: ASFWDriverConnector) {
        _viewModel = StateObject(wrappedValue: SaffireMixerViewModel(connector: connector))
    }
    
    var body: some View {
        VStack(spacing: 24) {
            Spacer()
            
            Toggle(isOn: Binding(
                get: { viewModel.outputState.muteEnabled },
                set: { viewModel.setMasterMute($0) }
            )) {
                Label("Master Mute", systemImage: "speaker.slash.fill")
                    .font(.title2)
            }
            .toggleStyle(.button)
            .modifier(ProminentGlassButtonStyle())
            .tint(.red)
            .controlSize(.large)
            
            Spacer()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .navigationTitle("Saffire")
        .onAppear {
            viewModel.startAutoRefresh(interval: 0.5)
        }
        .onDisappear {
            viewModel.stopAutoRefresh()
        }
    }
}

/// `.glassProminent` only exists on macOS 26; macOS 15 gets `.borderedProminent`.
private struct ProminentGlassButtonStyle: ViewModifier {
    func body(content: Content) -> some View {
        if #available(macOS 26.0, *) {
            content.buttonStyle(.glassProminent)
        } else {
            content.buttonStyle(.borderedProminent)
        }
    }
}
