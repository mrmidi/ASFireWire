//
//  CommandsView.swift
//  ASFW
//
//  Created by ASFireWire Project on 11.12.2025.
//

import SwiftUI

struct CommandsView: View {
    var viewModel: DebugViewModel
    @StateObject private var connectorObservable = ASFWDriverConnector.Observable()

    var body: some View {
        TabView {
            // Read/Write Tab
            ReadWriteView(viewModel: viewModel)
                .tabItem {
                    Label("Read/Write", systemImage: "arrow.left.arrow.right")
                }

            // Compare & Swap Tab
            CompareSwapView(connectorObservable: connectorObservable)
                .tabItem {
                    Label("Compare & Swap", systemImage: "lock.rectangle")
                }
        }
        .navigationTitle("Async Commands")
        .onAppear {
            // Connect to driver when view appears
            if !connectorObservable.isConnected {
                _ = connectorObservable.connector.connect()
            }
        }
    }
}

#if false
#Preview {
    CommandsView(viewModel: DebugViewModel())
}
#endif
