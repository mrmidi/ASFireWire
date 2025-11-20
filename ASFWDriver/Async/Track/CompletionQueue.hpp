#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "AsyncTypes.hpp"
#include "OHCIEventCodes.hpp"
#include "../../Shared/Completion/CompletionQueue.hpp"

namespace ASFW::Async {

/**
 * CompletionRecord - Async-specific completion token
 *
 * Contains Async transaction handle, OHCI event code, and metadata.
 * Satisfies the Shared::CompletionToken concept (trivially copyable, 4-byte aligned).
 */
struct CompletionRecord {
    AsyncHandle handle{};
    OHCIEventCode eventCode{};
    uint32_t actualLength{0};
    uint16_t hardwareTimeStamp{0};

    static constexpr std::size_t kInlinePayloadSize = 16;
    std::byte inlinePayload[kInlinePayloadSize]{};
} __attribute__((aligned(4)));  // Ensure 4-byte alignment for IODataQueue

// Validate CompletionRecord satisfies requirements
static_assert(std::is_trivially_copyable_v<CompletionRecord>,
              "CompletionRecord must be trivially copyable to enqueue across IODataQueue");
static_assert((sizeof(CompletionRecord) % 4) == 0,
              "CompletionRecord size must be a multiple of 4 bytes");
static_assert(alignof(CompletionRecord) >= 4,
              "CompletionRecord must be at least 4-byte aligned");

// Validate CompletionRecord satisfies Shared::CompletionToken concept
static_assert(Shared::CompletionToken<CompletionRecord>,
              "CompletionRecord must satisfy Shared::CompletionToken concept");

/**
 * CompletionQueue - Type alias for Async-specific completion queue
 *
 * Uses the generic Shared::CompletionQueue with CompletionRecord as the token type.
 * All SPSC semantics and atomic guards are provided by the Shared implementation.
 */
using CompletionQueue = Shared::CompletionQueue<CompletionRecord>;

} // namespace ASFW::Async
