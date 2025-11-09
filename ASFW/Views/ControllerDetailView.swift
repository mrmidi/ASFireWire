//
//  ControllerDetailView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI
import Foundation

struct ControllerDetailView: View {
    @ObservedObject var viewModel: DebugViewModel
    
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                // Connection Status
                connectionCard
                
                // Controller Status
                if viewModel.isConnected {
                    if let status = viewModel.controllerStatus {
                        controllerStatusCard(status)
                        asyncTransmitCard(status.async)
                    } else if let snapshot = viewModel.sharedStatus {
                        controllerSnapshotCard(snapshot)
                    } else {
                        ProgressView("Waiting for controller snapshot...")
                            .padding()
                    }
                }
            }
            .padding()
        }
        .navigationTitle("Controller Status")
    }
    
    @ViewBuilder
    private var connectionCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Image(systemName: viewModel.isConnected ? "cable.connector" : "cable.connector.slash")
                    .foregroundStyle(viewModel.isConnected ? .green : .red)
                    .imageScale(.large)
                
                Text(viewModel.isConnected ? "UserClient Connected" : "UserClient Disconnected")
                    .font(.headline)
                
                Spacer()
                
                Button(viewModel.isConnected ? "Reconnect" : "Connect") {
                    viewModel.connect()
                }
                .buttonStyle(.bordered)

                Button("Debug Snapshot") {
                    viewModel.dumpDebugSnapshot()
                }
                .buttonStyle(.borderless)
            }

            if let status = viewModel.sharedStatus {
                Divider()
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Label("Last Update", systemImage: "clock")
                            .font(.subheadline)
                        Spacer()
                        Text(status.reason.displayName)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    HStack(spacing: 8) {
                        Text("Sequence \(status.sequence)")
                            .font(.caption.monospaced())
                            .foregroundStyle(.secondary)
                        Text("Mach \(status.timestampMach)")
                            .font(.caption.monospaced())
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
    }
    
    @ViewBuilder
    private func controllerStatusCard(_ status: ControllerStatus) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Label("Hardware State", systemImage: "memorychip")
                .font(.headline)

            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 12) {
                MetricCard(label: "State", value: status.stateName, icon: "circle.hexagonpath")
                MetricCard(label: "Generation", value: "\(status.generation)", icon: "number.circle")
                MetricCard(label: "Bus Resets", value: "\(status.busResetCount)", icon: "bolt.horizontal")
                MetricCard(label: "Nodes", value: "\(status.nodeCount)", icon: "network")
                MetricCard(label: "Local Node", value: status.formattedLocalNodeID, icon: "location.circle")
                MetricCard(label: "Root Node", value: status.formattedRootNodeID, icon: "crown")
                MetricCard(label: "IRM Node", value: status.formattedIRMNodeID, icon: "star")
            }

            if status.isIRM || status.isCycleMaster {
                HStack(spacing: 12) {
                    if status.isIRM {
                        Label("IRM", systemImage: "checkmark.seal.fill")
                            .foregroundStyle(.green)
                            .font(.caption)
                    }
                    if status.isCycleMaster {
                        Label("Cycle Master", systemImage: "clock.arrow.circlepath")
                            .foregroundStyle(.blue)
                            .font(.caption)
                    }
                }
                .padding(.top, 4)
            }
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
    }

    @ViewBuilder
    private func controllerSnapshotCard(_ status: DriverStatus) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Label("Controller Snapshot", systemImage: "memorychip")
                .font(.headline)

            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 12) {
                MetricCard(label: "State", value: status.controllerStateName, icon: "circle.hexagonpath")
                MetricCard(label: "Generation", value: "\(status.busGeneration)", icon: "number.circle")
                MetricCard(label: "Bus Resets", value: "\(status.busResetCount)", icon: "bolt.horizontal")
                MetricCard(label: "Nodes", value: "\(status.nodeCount)", icon: "network")
                MetricCard(label: "Local Node", value: formatNodeID(status.localNodeID), icon: "location.circle")
                MetricCard(label: "Root Node", value: formatNodeID(status.rootNodeID), icon: "crown")
                MetricCard(label: "IRM Node", value: formatNodeID(status.irmNodeID), icon: "star")
            }

            HStack(spacing: 12) {
                if status.isIRM {
                    Label("IRM", systemImage: "checkmark.seal.fill")
                        .foregroundStyle(.green)
                        .font(.caption)
                }
                if status.isCycleMaster {
                    Label("Cycle Master", systemImage: "clock.arrow.circlepath")
                        .foregroundStyle(.blue)
                        .font(.caption)
                }
                if status.linkActive {
                    Label("Link Active", systemImage: "link")
                        .foregroundStyle(.teal)
                        .font(.caption)
                }
            }
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
    }

    @ViewBuilder
    private func asyncTransmitCard(_ asyncStatus: ControllerStatus.AsyncStatus) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 12) {
                Text("AT Request")
                    .font(.subheadline.bold())
                asyncContextDetails(asyncStatus.atRequest)

                Divider()

                Text("AT Response")
                    .font(.subheadline.bold())
                asyncContextDetails(asyncStatus.atResponse)

                Divider()

                Text("AR Request")
                    .font(.subheadline.bold())
                asyncContextDetails(asyncStatus.arRequest)

                Divider()

                Text("AR Response")
                    .font(.subheadline.bold())
                asyncContextDetails(asyncStatus.arResponse)

                Divider()

                Text("AR Request Buffers")
                    .font(.subheadline.bold())
                asyncBufferDetails(asyncStatus.arRequestBuffers)

                Divider()

                Text("AR Response Buffers")
                    .font(.subheadline.bold())
                asyncBufferDetails(asyncStatus.arResponseBuffers)

                Divider()

                Text("DMA Slab")
                    .font(.subheadline.bold())
                InfoRow(label: "Virtual Base", value: hex64(asyncStatus.dmaSlabVirt))
                InfoRow(label: "IOVA Base", value: hex64(asyncStatus.dmaSlabIOVA))
                InfoRow(label: "Size", value: String(format: "%u bytes", asyncStatus.dmaSlabSize))
            }
        } label: {
            Label("Async DMA", systemImage: "bolt.horizontal.circle")
                .font(.headline)
        }
    }

    @ViewBuilder
    private func asyncContextDetails(_ context: ControllerStatus.AsyncContextStatus) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            InfoRow(label: "Descriptor Virt", value: hex64(context.descriptorVirt))
            InfoRow(label: "Descriptor IOVA", value: hex64(context.descriptorIOVA))
            InfoRow(label: "CommandPtr", value: hex32(context.commandPtr))
            InfoRow(label: "Descriptors", value: "\(context.descriptorCount) × \(context.descriptorStride) bytes")
        }
    }

    @ViewBuilder
    private func asyncBufferDetails(_ buffers: ControllerStatus.AsyncBufferStatus) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            InfoRow(label: "Buffer Virt", value: hex64(buffers.bufferVirt))
            InfoRow(label: "Buffer IOVA", value: hex64(buffers.bufferIOVA))
            InfoRow(label: "Count", value: "\(buffers.bufferCount)")
            InfoRow(label: "Size", value: "\(buffers.bufferSize) bytes")
        }
    }

    private func hex64(_ value: UInt64) -> String {
        String(format: "0x%016llX", value)
    }

    private func hex32(_ value: UInt32) -> String {
        String(format: "0x%08X", value)
    }

    private func formatNodeID(_ value: UInt32?) -> String {
        guard let node = value else { return "--" }
        return String(format: "0x%02X", node)
    }
}
