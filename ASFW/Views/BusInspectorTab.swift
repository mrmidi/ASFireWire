enum BusInspectorTab: String, CaseIterable, Identifiable {
    case device = "Device"
    case rom = "Config ROM"
    case topology = "Topology & Self-ID"

    var id: Self { self }
}
