#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

} // namespace
