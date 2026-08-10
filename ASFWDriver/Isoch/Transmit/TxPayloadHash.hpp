#pragma once

#include "../../Shared/Isoch/TxPayloadSeal.hpp"

namespace ASFW::Isoch::Tx {

[[nodiscard]] inline uint64_t HashTxPayload(const void* bytes, size_t length) noexcept {
    return ASFW::Shared::Isoch::SealTxPayload(bytes, length);
}

} // namespace ASFW::Isoch::Tx
