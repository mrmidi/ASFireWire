//
//  SaffireMixerView.swift
//  ASFW
//
//  Created by ASFireWire Project on 2026-02-08.
//

import SwiftUI

struct SaffireMixerView: View {
    @State private var viewModel: SaffireMixerViewModel
    
    init(connectorObservable: ASFWDriverConnector.Observable) {
        _viewModel = State(initialValue: SaffireMixerViewModel(connectorObservable: connectorObservable))
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
            .buttonStyle(.glassProminent)
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
