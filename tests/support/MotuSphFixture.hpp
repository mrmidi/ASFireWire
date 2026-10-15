// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "TestDataUtils.hpp"
#include <charconv>

namespace ASFW::Tests {
struct MotuCapturedSph {
    uint64_t cycle;
    uint32_t sph;
};
// Compact passive-observer capture: absolute (unwrapped) bus cycle, first SPH.
inline bool LoadMotuSphFixture(std::string_view name, std::vector<MotuCapturedSph>& out) {
    out.clear();
    std::ifstream file(ResolveRepoRoot() / "documentation/fixtures/MOTU/828mk3-48k" / name);
    std::string line;
    if (!std::getline(file, line) || line != "cycle,sph")
        return false;
    while (std::getline(file, line)) {
        const auto comma = line.find(',');
        if (comma == std::string::npos)
            return false;
        MotuCapturedSph row{};
        const auto cycle = std::from_chars(line.data(), line.data() + comma, row.cycle);
        const auto sph =
            std::from_chars(line.data() + comma + 1, line.data() + line.size(), row.sph, 16);
        if (cycle.ec != std::errc{} || cycle.ptr != line.data() + comma || sph.ec != std::errc{} ||
            sph.ptr != line.data() + line.size() || (!out.empty() && row.cycle <= out.back().cycle))
            return false;
        out.push_back(row);
    }
    return file.eof() && !out.empty();
}
} // namespace ASFW::Tests
