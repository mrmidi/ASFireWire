import SwiftUI

struct ContentView: View {
    @EnvironmentObject var store: RomStore
    @State private var isImporterPresented = false

    var body: some View {
        NavigationSplitView {
            SidebarView()
        } detail: {
            DetailView()
        }
        .toolbar {
            ToolbarItem(placement: .automatic) {
                Button("Open ROM…") { isImporterPresented = true }
            }
            ToolbarItem(placement: .automatic) {
                Toggle(isOn: $store.showInterpreted) {
                    Text(store.showInterpreted ? "Interpreted" : "Raw")
                }
                .toggleStyle(.switch)
                .help("Toggle interpreted view")
            }
        }
        .fileImporter(isPresented: $isImporterPresented, allowedContentTypes: [.data]) { result in
            switch result {
            case .success(let url):
                store.open(url: url)
            case .failure(let err):
                store.error = err.localizedDescription
            }
        }
        .alert("Error", isPresented: Binding(get: { store.error != nil }, set: { _ in store.error = nil })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(store.error ?? "")
        }
    }
}
