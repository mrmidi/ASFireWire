//
//  AVCWireFormats.hpp
//  ASFWDriver
//
//  Wire formats for AV/C data serialization
//

#pragma once

#include <stdint.h>

namespace ASFW::UserClient::Wire {

/**
 * @brief Wire format for av/C query response
 *
 * Structure:
 * - AVCQueryWire header
 * - Array of 'unitCount' × AVCUnitWire structures
 */
struct AVCQueryWire {
    uint32_t unitCount;       ///< Number of AV/C units
    uint32_t _padding;        ///< Padding for alignment
} __attribute__((packed));

static_assert(sizeof(AVCQueryWire) == 8, "AVCQueryWire must be 8 bytes");

/**
 * @brief Wire format for single AV/C unit
 *
 * Contains basic unit information: GUID, initialization status, subunit count
 */
struct AVCUnitWire {
    uint64_t guid;            ///< Unit GUID (from parent device)
    uint16_t nodeId;          ///< Current node ID
    uint8_t isInitialized;    ///< 1 if initialized, 0 otherwise
    uint8_t subunitCount;     ///< Number of discovered subunits
    uint32_t _padding;        ///< Padding for alignment
} __attribute__((packed));

static_assert(sizeof(AVCUnitWire) == 16, "AVCUnitWire must be 16 bytes");

} // namespace ASFW::UserClient::Wire
