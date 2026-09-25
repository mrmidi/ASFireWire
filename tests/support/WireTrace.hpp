// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// WireTrace.hpp - Record what a host puts on the wire, and compare it with a
// checked-in golden file.
//
// One line per event, so a behaviour change shows up as a readable diff:
//
//   R ffff.e0000028 380 @s400                read request (length)
//   W ffff.e0000074 0000020c @s400           write request (payload hex)
//   L ffff.e0000028 ffff000000000000 -> ffc0000100000000 = ffff000000000000 @s400
//                                            compare-swap (expected -> desired = previous)
//   R ffff.e000007c 4 @s400 !timeout         a request that did not succeed
//   # notify 0x23                            device-side event
//   ! enable-with-unarmed-stream             invariant the device observed being broken
//
// Golden files live under tests/golden/. Set ASFW_UPDATE_GOLDEN=1 to rewrite
// them from the current behaviour; review the diff before committing.

#pragma once

#include "TestDataUtils.hpp"

#include "Async/AsyncTypes.hpp"
#include "Common/FWTypes.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ASFW::Testing {

class WireTrace final {
public:
    void Add(std::string_view line) { lines_.emplace_back(line); }

    void Read(uint16_t hi, uint32_t lo, uint32_t length, ::ASFW::FW::FwSpeed speed,
              ::ASFW::Async::AsyncStatus status) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "R %04x.%08x %u", hi, lo, length);
        lines_.push_back(Finish(buf, speed, status));
    }

    void Write(uint16_t hi, uint32_t lo, std::span<const uint8_t> payload,
               ::ASFW::FW::FwSpeed speed, ::ASFW::Async::AsyncStatus status) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "W %04x.%08x ", hi, lo);
        lines_.push_back(Finish(std::string(buf) + Hex(payload), speed, status));
    }

    void CompareSwap(uint16_t hi, uint32_t lo, uint64_t expected, uint64_t desired,
                     std::optional<uint64_t> previous, ::ASFW::FW::FwSpeed speed,
                     ::ASFW::Async::AsyncStatus status) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "L %04x.%08x %016llx -> %016llx", hi, lo,
                      static_cast<unsigned long long>(expected),
                      static_cast<unsigned long long>(desired));
        std::string line = buf;
        if (previous) {
            std::snprintf(buf, sizeof(buf), " = %016llx", static_cast<unsigned long long>(*previous));
            line += buf;
        }
        lines_.push_back(Finish(line, speed, status));
    }

    [[nodiscard]] const std::vector<std::string>& Lines() const noexcept { return lines_; }
    void Clear() { lines_.clear(); }

    [[nodiscard]] std::string Text() const {
        std::string out;
        for (const auto& line : lines_) {
            out += line;
            out += '\n';
        }
        return out;
    }

private:
    static std::string Hex(std::span<const uint8_t> bytes) {
        std::string out;
        char pair[3];
        for (const uint8_t b : bytes) {
            std::snprintf(pair, sizeof(pair), "%02x", b);
            out += pair;
        }
        return out;
    }

    static const char* SpeedName(::ASFW::FW::FwSpeed speed) {
        switch (speed) {
        case ::ASFW::FW::FwSpeed::S100: return "s100";
        case ::ASFW::FW::FwSpeed::S200: return "s200";
        case ::ASFW::FW::FwSpeed::S400: return "s400";
        case ::ASFW::FW::FwSpeed::S800: return "s800";
        }
        return "s?";
    }

    static const char* StatusName(::ASFW::Async::AsyncStatus status) {
        using ::ASFW::Async::AsyncStatus;
        switch (status) {
        case AsyncStatus::kSuccess: return nullptr;
        case AsyncStatus::kTimeout: return "timeout";
        case AsyncStatus::kStaleGeneration: return "stale-generation";
        default: return "error";
        }
    }

    static std::string Finish(std::string line, ::ASFW::FW::FwSpeed speed,
                              ::ASFW::Async::AsyncStatus status) {
        line += " @";
        line += SpeedName(speed);
        if (const char* name = StatusName(status)) {
            line += " !";
            line += name;
        }
        return line;
    }

    std::vector<std::string> lines_;
};

// Compare `trace` with tests/golden/<relativePath>. With ASFW_UPDATE_GOLDEN set
// the file is (re)written instead and the comparison passes.
inline void ExpectMatchesGolden(const WireTrace& trace, std::string_view relativePath) {
    const auto path = ::ASFW::Tests::ResolveRepoRoot() / "tests" / "golden" /
                      std::filesystem::path(relativePath);
    const std::string actual = trace.Text();

    if (const char* update = std::getenv("ASFW_UPDATE_GOLDEN"); update && *update && *update != '0') {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << actual;
        return;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "missing golden " << path
                      << " (run with ASFW_UPDATE_GOLDEN=1 to create it)\n--- actual ---\n"
                      << actual;
        return;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string expected = buffer.str();
    if (expected == actual) {
        return;
    }

    auto split = [](const std::string& text) {
        std::vector<std::string> lines;
        std::stringstream ss(text);
        for (std::string line; std::getline(ss, line);) {
            lines.push_back(line);
        }
        return lines;
    };
    const auto want = split(expected);
    const auto got = split(actual);
    size_t first = 0;
    while (first < want.size() && first < got.size() && want[first] == got[first]) {
        ++first;
    }
    std::string report;
    const size_t from = first > 3 ? first - 3 : 0;
    for (size_t i = from; i < first; ++i) {
        report += "  " + want[i] + "\n";
    }
    for (size_t i = first; i < want.size() && i < first + 12; ++i) {
        report += "- " + want[i] + "\n";
    }
    for (size_t i = first; i < got.size() && i < first + 12; ++i) {
        report += "+ " + got[i] + "\n";
    }
    ADD_FAILURE() << "wire trace differs from " << path << " at line " << (first + 1)
                  << " (golden " << want.size() << " lines, actual " << got.size()
                  << " lines)\n" << report
                  << "If the change is intended, rerun with ASFW_UPDATE_GOLDEN=1 and review the diff.";
}

} // namespace ASFW::Testing
