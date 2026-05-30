#pragma once

#include <expected>
#include <vector>

#include "SelfIDCapture.hpp"
#include "TopologyTypes.hpp"

namespace ASFW::Driver {

/**
 * @class SelfIDStreamParser
 * @brief Decodes raw Self-ID quadlets into ordered node records.
 *
 * This parser handles the low-level validation of the Self-ID stream, ensuring
 * that all nodes have base packets, physical IDs are contiguous, and extended
 * packets are correctly associated. It does not build topology links; that is
 * the responsibility of the topology normalizer.
 */
class SelfIDStreamParser {
public:
    /**
     * Parse a Self-ID capture result into a vector of node records.
     *
     * @param result The capture result from SelfIDCapture.
     * @return A vector of records ordered by physical ID, or a build error.
     */
    static std::expected<std::vector<SelfIDNodeRecord>, TopologyBuildError>
    Parse(const SelfIDCapture::Result& result);

private:
    static bool ValidateContiguousPhysicalIds(const std::vector<SelfIDNodeRecord>& records) noexcept;
};

} // namespace ASFW::Driver
