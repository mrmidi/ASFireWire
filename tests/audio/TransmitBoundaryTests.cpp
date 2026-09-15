#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

[[nodiscard]] std::string ReadSource(const char* relativePath) {
    const auto repositoryRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    std::ifstream source(repositoryRoot / relativePath);
    std::ostringstream contents;
    contents << source.rdbuf();
    return contents.str();
}

[[nodiscard]] std::filesystem::path RepositoryRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

/// Blank out comments, keeping length and line structure.
///
/// The boundary these tests guard is a CODE dependency: transport must not
/// include audio headers, name audio types, or reason about content. Prose
/// about *why* a transport constant holds its value is the opposite of a
/// violation -- CLAUDE.md requires citing the reference a wire-observable
/// constant came from, and those citations name devices and reference stacks.
/// Matching raw file text put the two rules in conflict and the citations
/// lost: it flagged kPayloadFinalityLeadPackets's provenance (Saffire.kext,
/// reversed at two named addresses) and one sentence naming which wakeups can
/// still bind a payload. It would equally flag a commented-out memset below.
[[nodiscard]] std::string StripComments(const std::string& source) {
    std::string out = source;
    const size_t n = out.size();
    for (size_t i = 0; i < n;) {
        if (out[i] == '"' || out[i] == '\'') {
            const char quote = out[i];
            ++i;
            while (i < n && out[i] != quote) {
                i += (out[i] == '\\') ? 2 : 1;
            }
            ++i;
            continue;
        }
        if (out[i] == '/' && i + 1 < n && out[i + 1] == '/') {
            while (i < n && out[i] != '\n') out[i++] = ' ';
            continue;
        }
        if (out[i] == '/' && i + 1 < n && out[i + 1] == '*') {
            out[i] = ' ';
            out[i + 1] = ' ';
            i += 2;
            while (i + 1 < n && !(out[i] == '*' && out[i + 1] == '/')) {
                if (out[i] != '\n') out[i] = ' ';
                ++i;
            }
            if (i + 1 < n) {
                out[i] = ' ';
                out[i + 1] = ' ';
                i += 2;
            }
            continue;
        }
        ++i;
    }
    return out;
}

TEST(TransmitBoundaryTests, CoreSourcesDoNotDependOnAudioPacketSemantics) {
    // Comments stripped for the same reason as the tree scans below.
    const std::string headers =
        ReadSource("ASFWDriver/Isoch/Transmit/IsochTransmitContext.hpp") +
        ReadSource("ASFWDriver/Isoch/Transmit/IsochTxDmaRing.hpp") +
        ReadSource("ASFWDriver/Isoch/Transmit/IsochTxLayout.hpp");
    const std::string sources =
        ReadSource("ASFWDriver/Isoch/Transmit/IsochTransmitContext.cpp") +
        ReadSource("ASFWDriver/Isoch/Transmit/IsochTxDmaRing.cpp");
    const std::string headerCode = StripComments(headers);
    const std::string sourceCode = StripComments(sources);

    for (const char* forbidden : {
             "Audio/", "AudioTimingGeometry", "IsochAudioTransport",
             "AM824", "CipHeader", "SYT", "Replay", "ZTS", "sampleFrame",
         }) {
        EXPECT_EQ(headerCode.find(forbidden), std::string::npos) << forbidden;
        EXPECT_EQ(sourceCode.find(forbidden), std::string::npos) << forbidden;
    }
}

TEST(TransmitBoundaryTests, TransportTreesContainOnlyContentNeutralCode) {
    const auto repositoryRoot = RepositoryRoot();
    const std::vector<std::filesystem::path> transportRoots{
        repositoryRoot / "ASFWDriver" / "Isoch",
        repositoryRoot / "ASFWDriver" / "Shared" / "Isoch",
    };
    const std::regex contentSemanticToken(
        R"(\b(AM824|CIP|PCM|SYT|ZTS|DICE|ADK|CoreAudio|AudioDriverKit|Saffire|Focusrite|Duet)\b)");

    for (const auto& root : transportRoots) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto extension = entry.path().extension();
            if (extension != ".hpp" && extension != ".cpp") {
                continue;
            }

            std::ifstream source(entry.path());
            std::ostringstream buffer;
            buffer << source.rdbuf();
            const std::string contents = StripComments(buffer.str());
            EXPECT_EQ(contents.find("ASFW::Audio"), std::string::npos)
                << entry.path();
            EXPECT_EQ(contents.find("/Audio/"), std::string::npos)
                << entry.path();
            EXPECT_FALSE(std::regex_search(contents, contentSemanticToken))
                << entry.path();
        }
    }
}

TEST(TransmitBoundaryTests, AudioPolicyHasNoLegacyTransportLocation) {
    const auto repositoryRoot = RepositoryRoot();
    for (const char* filename : {
             "AudioTimingGeometry.hpp",
             "AudioGeometryPolicy.hpp",
             "AudioHalBufferProfiles.hpp",
         }) {
        EXPECT_TRUE(std::filesystem::exists(
            repositoryRoot / "ASFWDriver" / "Audio" / "Shared" / filename));
        EXPECT_FALSE(std::filesystem::exists(
            repositoryRoot / "ASFWDriver" / "Audio" / "Config" / filename));
        EXPECT_FALSE(std::filesystem::exists(
            repositoryRoot / "ASFWDriver" / "Shared" / "Isoch" / filename));
    }
    EXPECT_TRUE(std::filesystem::exists(
        repositoryRoot / "ASFWDriver" / "Audio" / "Runtime" /
        "ZtsTelemetry.hpp"));
    EXPECT_FALSE(std::filesystem::exists(
        repositoryRoot / "ASFWDriver" / "Isoch" / "Receive" /
        "ZtsTelemetry.hpp"));
}

} // namespace

// A cache-inhibited DMA mapping does not accept `dc zva`, which Apple's __bzero
// selects by block size -- so memset over a DMA region is safe only below a
// libc-internal threshold. IsochTxDescriptorSlab did exactly that and survived
// at a 4 KiB descriptor slab; the first start after the TX ring deepened it to
// 32 KiB took EXC_ARM_DA_ALIGN inside __bzero. Size is not the fix, the store
// instruction is: Shared::FillUncachedDma.
//
// Matches the call shape that bit us (a memset whose destination is a DMA
// region's base) rather than memset in general, so the fixed-size descriptor
// fills stay legal.
TEST(TransmitBoundaryTests, NoMemsetOverDmaRegionBases) {
    const auto repositoryRoot = RepositoryRoot();
    const std::vector<std::filesystem::path> dmaRoots{
        repositoryRoot / "ASFWDriver" / "Isoch",
        repositoryRoot / "ASFWDriver" / "Shared" / "Memory",
        repositoryRoot / "ASFWDriver" / "Async",
    };
    const std::regex memsetOverDmaBase(
        R"((memset|bzero)\s*\([^;]*(virtualBase|slabVirt_|payloadBase|BaseVirtual\(\)))");

    for (const auto& root : dmaRoots) {
        if (!std::filesystem::exists(root)) {
            continue;
        }
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto extension = entry.path().extension();
            if (extension != ".hpp" && extension != ".cpp") {
                continue;
            }
            std::ifstream source(entry.path());
            std::ostringstream buffer;
            buffer << source.rdbuf();
            const std::string contents = StripComments(buffer.str());
            EXPECT_FALSE(std::regex_search(contents, memsetOverDmaBase))
                << entry.path()
                << ": fill DMA regions with ASFW::Shared::FillUncachedDma";
        }
    }
}
