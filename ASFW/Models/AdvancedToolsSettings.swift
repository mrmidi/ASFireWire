enum AdvancedToolsSettings {
    // Development preferences must not accidentally enable advanced tools in Release.
    #if DEBUG
    static let storageKey = "asfw.ui.advanced-tools.debug"
    static let defaultEnabled = true
    #else
    static let storageKey = "asfw.ui.advanced-tools.release"
    static let defaultEnabled = false
    #endif
}
