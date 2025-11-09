#pragma once

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace ASFW::Tests {

inline std::filesystem::path ResolveRepoRoot() {
    auto path = std::filesystem::absolute(std::filesystem::path(__FILE__));
    for (int i = 0; i < 3; ++i) {
        path = path.parent_path();
    }
    return path;
}

inline bool LoadHexArrayFromCFile(const std::filesystem::path& filePath,
                                  std::string_view arrayName,
                                  std::vector<uint32_t>& outWords,
                                  std::string* errorMessage) {
    outWords.clear();

    if (!std::filesystem::exists(filePath)) {
        if (errorMessage) {
            *errorMessage = "Missing reference data file: " + filePath.string();
        }
        return false;
    }

    std::ifstream stream(filePath);
    if (!stream.is_open()) {
        if (errorMessage) {
            *errorMessage = "Unable to open reference data file: " + filePath.string();
        }
        return false;
    }

    std::string line;
    bool foundDeclaration = false;
    bool capturingValues = false;

    while (std::getline(stream, line)) {
        if (!foundDeclaration) {
            if (line.find(arrayName) != std::string::npos) {
                foundDeclaration = true;
                if (line.find('{') != std::string::npos) {
                    capturingValues = true;
                }
            }
            continue;
        }

        if (!capturingValues) {
            if (line.find('{') != std::string::npos) {
                capturingValues = true;
            }
            continue;
        }

        size_t searchPos = 0;
        while ((searchPos = line.find("0x", searchPos)) != std::string::npos) {
            const size_t valueStart = searchPos + 2;
            size_t valueEnd = valueStart;
            while (valueEnd < line.size() && std::isxdigit(static_cast<unsigned char>(line[valueEnd]))) {
                ++valueEnd;
            }
            if (valueEnd > valueStart) {
                const auto token = line.substr(valueStart, valueEnd - valueStart);
                uint32_t word = static_cast<uint32_t>(std::stoul(token, nullptr, 16));
                outWords.push_back(word);
            }
            searchPos = valueEnd;
        }

        if (line.find('}') != std::string::npos) {
            break;
        }
    }

    if (!foundDeclaration || outWords.empty()) {
        if (errorMessage) {
            *errorMessage = "Failed to parse array '" + std::string(arrayName) + "' from " + filePath.string();
        }
        return false;
    }

    return true;
}

inline bool LoadHexArrayFromRepoFile(std::string_view relativePath,
                                     std::string_view arrayName,
                                     std::vector<uint32_t>& outWords,
                                     std::string* errorMessage) {
    const auto absolutePath = ResolveRepoRoot() / std::filesystem::path(relativePath);
    return LoadHexArrayFromCFile(absolutePath, arrayName, outWords, errorMessage);
}

} // namespace ASFW::Tests
