#pragma once

#include <cstddef>
#include <cstdint>

namespace ASFW::Isoch::Tx {

[[nodiscard]] inline uint64_t HashTxPayload(const void* bytes, size_t length) noexcept {
    constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;

    if (!bytes || length == 0) {
        return 0;
    }

    const auto* input = static_cast<const uint8_t*>(bytes);
    uint64_t hash = kOffsetBasis;
    for (size_t index = 0; index < length; ++index) {
        hash ^= input[index];
        hash *= kPrime;
    }
    return hash;
}

} // namespace ASFW::Isoch::Tx
