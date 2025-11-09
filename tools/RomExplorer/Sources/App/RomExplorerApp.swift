import SwiftUI

@main
struct RomExplorerApp: App {
    @StateObject private var store = RomStore()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(store)
        }
        .windowStyle(.automatic)
    }
}
