#pragma once

#include <cstdint>
#include <cstring>

namespace ASFW::Shared {

/**
 * Driver version information structure for UserClient queries.
 * 
 * This structure is shared between kernel driver and userspace tools.
 * It provides version metadata for debugging and verification purposes.
 * 
 * Layout is designed for ABI stability with reserved fields for future expansion.
 */
struct DriverVersionInfo {
    char semanticVersion[32];    ///< Semantic version string (e.g., "0.1.0-alpha")
    char gitCommitShort[8];      ///< Short git commit hash (7 chars + null)
    char gitCommitFull[41];      ///< Full git commit SHA-1 (40 chars + null)
    char gitBranch[64];          ///< Git branch name
    char buildTimestamp[32];     ///< ISO 8601 timestamp (e.g., "2025-11-18T21:30:00Z")
    char buildHost[64];          ///< Build machine hostname
    bool gitDirty;               ///< True if working tree had uncommitted changes
    uint8_t padding[3];          ///< Padding for alignment
    uint32_t reserved[8];        ///< Reserved for future expansion
    
    /// Default constructor - zero-initialize
    DriverVersionInfo() {
        std::memset(this, 0, sizeof(*this));
    }
    
    /// Helper to populate from compile-time constants
    template <size_t N1, size_t N2, size_t N3, size_t N4, size_t N5, size_t N6>
    static DriverVersionInfo Create(
        const char (&semVer)[N1],
        const char (&commitShort)[N2],
        const char (&commitFull)[N3],
        const char (&branch)[N4],
        const char (&timestamp)[N5],
        const char (&host)[N6],
        bool dirty)
    {
        DriverVersionInfo info{};
        
        // Safe string copy with bounds checking
        std::strncpy(info.semanticVersion, semVer, sizeof(info.semanticVersion) - 1);
        std::strncpy(info.gitCommitShort, commitShort, sizeof(info.gitCommitShort) - 1);
        std::strncpy(info.gitCommitFull, commitFull, sizeof(info.gitCommitFull) - 1);
        std::strncpy(info.gitBranch, branch, sizeof(info.gitBranch) - 1);
        std::strncpy(info.buildTimestamp, timestamp, sizeof(info.buildTimestamp) - 1);
        std::strncpy(info.buildHost, host, sizeof(info.buildHost) - 1);
        info.gitDirty = dirty;
        
        return info;
    }
};


static_assert(sizeof(DriverVersionInfo) == 280, "DriverVersionInfo size must be stable for ABI");

} // namespace ASFW::Shared
