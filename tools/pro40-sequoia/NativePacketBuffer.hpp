// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <span>
namespace NativePackets {
// Neutral fully-framed payload handed to native transport, with explicit length.
struct PreparedBuffer { std::array<uint8_t,448> bytes{}; uint32_t length=0; };
// Borrowed completed slot: retain the source mapping, or the journal's unread
// group, for the entire use. Journal consumption invalidates its borrowed views.
struct CompletedBuffer { std::span<const uint8_t> bytes{}; uint32_t status=0,timestamp=0; };
}
