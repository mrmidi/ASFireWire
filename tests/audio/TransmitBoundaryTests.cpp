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

TEST(TransmitBoundaryTests, CoreSourcesDoNotDependOnAudioPacketSemantics) {
    const std::string headers =
        ReadSource("ASFWDriver/Isoch/Transmit/IsochTransmitContext.hpp") +
        ReadSource("ASFWDriver/Isoch/Transmit/IsochTxDmaRing.hpp") +
        ReadSource("ASFWDriver/Isoch/Transmit/IsochTxLayout.hpp");
    const std::string sources =
        ReadSource("ASFWDriver/Isoch/Transmit/IsochTransmitContext.cpp") +
        ReadSource("ASFWDriver/Isoch/Transmit/IsochTxDmaRing.cpp");

    for (const char* forbidden : {
             "Audio/", "AudioTimingGeometry", "IsochAudioTransport",
             "AM824", "CipHeader", "SYT", "Replay", "ZTS", "sampleFrame",
         }) {
        EXPECT_EQ(headers.find(forbidden), std::string::npos) << forbidden;
        EXPECT_EQ(sources.find(forbidden), std::string::npos) << forbidden;
    }
}

// A cache-inhibited DMA mapping does not accept `dc zva`, which Apple's __bzero
// picks by block size, so memset over a DMA region works only below a libc
// threshold (UncachedFill.hpp). This matches the call shape that crashed on the
// midi branch -- a memset whose destination is a DMA region base -- so the
// fixed-size single-descriptor fills stay legal.
TEST(TransmitBoundaryTests, NoMemsetOverDmaRegionBases) {
    const auto repositoryRoot =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const std::vector<std::filesystem::path> dmaRoots{
        repositoryRoot / "ASFWDriver" / "Isoch",
        repositoryRoot / "ASFWDriver" / "Shared" / "Memory",
        repositoryRoot / "ASFWDriver" / "Async",
    };
    const std::regex memsetOverDmaBase(
        R"((memset|bzero)\s*\([^;]*(virtualBase|slabVirt_|payloadBase|BaseVirtual\(\)))");
    for (const auto& root : dmaRoots) {
        ASSERT_TRUE(std::filesystem::exists(root)) << root;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            const auto extension = entry.path().extension();
            if (!entry.is_regular_file() || (extension != ".hpp" && extension != ".cpp")) {
                continue;
            }
            std::ifstream source(entry.path());
            std::ostringstream contents;
            contents << source.rdbuf();
            EXPECT_FALSE(std::regex_search(contents.str(), memsetOverDmaBase))
                << entry.path() << ": fill DMA regions with ASFW::Shared::FillUncachedDma";
        }
    }
}

} // namespace
