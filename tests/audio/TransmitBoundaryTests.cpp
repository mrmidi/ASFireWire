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
            const std::string contents = buffer.str();
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
