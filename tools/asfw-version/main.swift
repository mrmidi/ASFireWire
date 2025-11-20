import Foundation
import IOKit

/// Driver version verification CLI tool
/// Queries the loaded ASFWDriver and compares with current git state

struct DriverVersionInfo {
    var semanticVersion: String
    var gitCommitShort: String
    var gitCommitFull: String
    var gitBranch: String
    var buildTimestamp: String
    var buildHost: String
    var gitDirty: Bool
    
    init(from data: Data) {
        var offset = 0
        
        // Parse fixed-size C struct (matches DriverVersionInfo.hpp layout)
        semanticVersion = data.extractString(at: offset, maxLength: 32)
        offset += 32
        
        gitCommitShort = data.extractString(at: offset, maxLength: 8)
        offset += 8
        
        gitCommitFull = data.extractString(at: offset, maxLength: 41)
        offset += 41
        
        gitBranch = data.extractString(at: offset, maxLength: 64)
        offset += 64
        
        buildTimestamp = data.extractString(at: offset, maxLength: 32)
        offset += 32
        
        buildHost = data.extractString(at: offset, maxLength: 64)
        offset += 64
        
        gitDirty = data.extractBool(at: offset)
    }
}

extension Data {
    func extractString(at offset: Int, maxLength: Int) -> String {
        let range = offset..<(offset + maxLength)
        guard range.upperBound <= self.count else { return "" }
        
        let subdata = self.subdata(in: range)
        if let nullIndex = subdata.firstIndex(of: 0) {
            return String(data: subdata.prefix(upTo: nullIndex), encoding: .utf8) ?? ""
        }
        return String(data: subdata, encoding: .utf8) ?? ""
    }
    
    func extractBool(at offset: Int) -> Bool {
        guard offset < self.count else { return false }
        return self[offset] != 0
    }
}

func queryDriverVersion() -> DriverVersionInfo? {
    // Find the ASFWDriver service
    let matchingDict = IOServiceMatching("net_mrmidi_ASFW_ASFWDriver")
    var iterator: io_iterator_t = 0
    
    let kr = IOServiceGetMatchingServices(kIOMainPortDefault, matchingDict, &iterator)
    guard kr == KERN_SUCCESS else {
        print("❌ Failed to find ASFWDriver service")
        return nil
    }
    
    defer { IOObjectRelease(iterator) }
    
    let service = IOIteratorNext(iterator)
    guard service != 0 else {
        print("❌ ASFWDriver not loaded")
        return nil
    }
    
    defer { IOObjectRelease(service) }
    
    // Open UserClient connection
    var connect: io_connect_t = 0
    let openKr = IOServiceOpen(service, mach_task_self_, 0, &connect)
    guard openKr == KERN_SUCCESS else {
        print("❌ Failed to open UserClient connection")
        return nil
    }
    
    defer { IOServiceClose(connect) }
    
    // Call kMethodGetDriverVersion (selector 18)
    var outputStruct = Data(count: 280) // sizeof(DriverVersionInfo)
    var outputStructCnt = outputStruct.count
    
    let callKr = outputStruct.withUnsafeMutableBytes { outputPtr in
        IOConnectCallStructMethod(
            connect,
            18, // kMethodGetDriverVersion selector
            nil,
            0,
            outputPtr.baseAddress,
            &outputStructCnt
        )
    }
    
    guard callKr == KERN_SUCCESS else {
        print("❌ Failed to query driver version (error: \\(callKr))")
        print("   This may indicate the UserClient method is not implemented yet")
        return nil
    }
    
    return DriverVersionInfo(from: outputStruct)
}

func getCurrentGitInfo() -> (commit: String, branch: String, dirty: Bool)? {
    let commitTask = Process()
    commitTask.executableURL = URL(fileURLWithPath: "/usr/bin/git")
    commitTask.arguments = ["rev-parse", "--short", "HEAD"]
    
    let commitPipe = Pipe()
    commitTask.standardOutput = commitPipe
    commitTask.standardError = Pipe()
    
    do {
        try commitTask.run()
        commitTask.waitUntilExit()
        
        let commitData = commitPipe.fileHandleForReading.readDataToEndOfFile()
        let commit = String(data: commitData, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "unknown"
        
        // Get branch
        let branchTask = Process()
        branchTask.executableURL = URL(fileURLWithPath: "/usr/bin/git")
        branchTask.arguments = ["rev-parse", "--abbrev-ref", "HEAD"]
        
        let branchPipe = Pipe()
        branchTask.standardOutput = branchPipe
        branchTask.standardError = Pipe()
        
        try branchTask.run()
        branchTask.waitUntilExit()
        
        let branchData = branchPipe.fileHandleForReading.readDataToEndOfFile()
        let branch = String(data: branchData, encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "unknown"
        
        // Check dirty status
        let statusTask = Process()
        statusTask.executableURL = URL(fileURLWithPath: "/usr/bin/git")
        statusTask.arguments = ["diff-index", "--quiet", "HEAD", "--"]
        statusTask.standardOutput = Pipe()
        statusTask.standardError = Pipe()
        
        try statusTask.run()
        statusTask.waitUntilExit()
        
        let dirty = (statusTask.terminationStatus != 0)
        
        return (commit, branch, dirty)
    } catch {
        return nil
    }
}

// Main execution
print("ASFWDriver Version Checker")
print("==========================\\n")

guard let driverInfo = queryDriverVersion() else {
    print("\n⚠️  Could not query driver version.")
    print("   Make sure the driver is loaded and UserClient API is implemented.")
    exit(1)
}

print("Driver Version Information:")
print("  Semantic Version: \(driverInfo.semanticVersion)")
print("  Git Commit:       \(driverInfo.gitCommitShort) (\(driverInfo.gitBranch))")
print("  Full Commit:      \(driverInfo.gitCommitFull)")
print("  Build Timestamp:  \(driverInfo.buildTimestamp)")
print("  Build Host:       \(driverInfo.buildHost)")
print("  Dirty Build:      \(driverInfo.gitDirty ? "True ⚠️" : "False")")

print("\nSystem Extension Status:")
// TODO: Query system extension path and binary timestamp

if let gitInfo = getCurrentGitInfo() {
    print("\nCurrent Git State:")
    print("  Commit:  \(gitInfo.commit)")
    print("  Branch:  \(gitInfo.branch)")
    print("  Dirty:   \(gitInfo.dirty ? "True" : "False")")
    
    // Compare
    print("\nVersion Match Analysis:")
    if driverInfo.gitCommitShort == gitInfo.commit {
        print("  ✅ Loaded driver matches current git HEAD")
    } else {
        print("  ⚠️  WARNING: Version mismatch detected!")
        print("     Loaded:  \\(driverInfo.gitCommitShort)")
        print("     Current: \\(gitInfo.commit)")
        print("     → Driver binary is stale. Rebuild and reload required.")
    }
    
    if driverInfo.gitDirty {
        print("  ⚠️  Driver was built with uncommitted changes")
        print("     The loaded binary may not match any committed state")
    }
} else {
    print("\\n⚠️  Could not determine current git state")
    print("   (Not in a git repository or git not available)")
}

print("")
