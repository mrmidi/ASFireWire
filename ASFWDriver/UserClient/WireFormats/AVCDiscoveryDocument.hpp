// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCDiscoveryDocument.hpp - Versioned diagnostics document for one AV/C unit.
//
// One JSON document per unit, built from the unit's immutable discovery
// snapshot, its graph and its FCP exchange log (timed). The user client serves
// it in pages of at most 4 KiB. Every page carries the discovery session, the
// route generation, the total length and a checksum of the whole document, so
// a reader can tell when pages from two different documents were mixed
// (a refresh or new traffic between page reads) and start again.
//
// Format "asfw.avc.discovery", version 1. Additive changes keep the version;
// a removed or retyped field bumps it.

#pragma once

#include "../../Protocols/AVC/Discovery/DiscoverySnapshot.hpp"
#include "../../Protocols/AVC/FcpExchangeRecorder.hpp"
#include "../../Protocols/AVC/Graph/AvcDeviceGraph.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ASFW::UserClient::Wire {

inline constexpr uint32_t kAVCDiscoveryDocumentMagic = 0x44445641; // "AVDD", little-endian
inline constexpr uint16_t kAVCDiscoveryDocumentVersion = 1;

/// Little-endian page header, followed by `length` document bytes.
struct AVCDiscoveryPageWire {
    uint32_t magic{kAVCDiscoveryDocumentMagic};
    uint16_t version{kAVCDiscoveryDocumentVersion};
    uint16_t headerBytes{sizeof(AVCDiscoveryPageWire)};
    uint32_t session{0};      ///< Discovery session of the snapshot (0: none yet).
    uint32_t generation{0};   ///< Bus generation of the snapshot's route.
    uint32_t totalBytes{0};   ///< Whole document length.
    uint32_t offset{0};       ///< Where this page's bytes start in the document.
    uint32_t length{0};       ///< Document bytes in this page.
    uint32_t checksum{0};     ///< FNV-1a of the whole document.
} __attribute__((packed));
static_assert(sizeof(AVCDiscoveryPageWire) == 32, "AVCDiscoveryPageWire must be 32 bytes");

[[nodiscard]] constexpr uint32_t Fnv1a32(std::string_view bytes) noexcept {
    uint32_t hash = 0x811C9DC5u;
    for (const char c : bytes) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 0x01000193u;
    }
    return hash;
}
static_assert(Fnv1a32("") == 0x811C9DC5u && Fnv1a32("a") == 0xE40C292Cu);

/// The document. `snapshot` and `graph` may be null (no discovery has
/// finished: a profile-owned device still has an exchange log).
[[nodiscard]] std::string BuildAVCDiscoveryDocument(
    const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot* snapshot,
    const Protocols::AVC::Graph::DeviceGraph* graph,
    const Protocols::AVC::FcpExchangeLog& exchanges);

/// One page starting at `offset`, at most `maxBytes` including the header.
/// An offset at or past the end yields a header with length 0.
[[nodiscard]] std::vector<uint8_t> SerializeDiscoveryPage(std::string_view document, uint32_t session,
                                                          uint32_t generation, uint32_t offset,
                                                          size_t maxBytes);

} // namespace ASFW::UserClient::Wire
