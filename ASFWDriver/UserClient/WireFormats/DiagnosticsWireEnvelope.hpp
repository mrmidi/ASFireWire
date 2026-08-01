//
//  DiagnosticsWireEnvelope.hpp
//  ASFWDriver
//
//  The diagnostics ABI is a wire contract even when collecting a snapshot
//  fails.  In particular, callers need a valid header to learn the status.
//

#pragma once

#include "../../Shared/ASFWDiagnosticsABI.h"

#include <cstddef>
#include <type_traits>

namespace ASFW::UserClient::Wire {

/// Establishes the portion of a diagnostics response header that remains
/// meaningful when the collector cannot produce a snapshot.
inline void PrimeDiagnosticsErrorHeader(ASFWDiagHeader& header,
                                        uint32_t structSize) noexcept {
    header.abiVersion = ASFW_DIAG_ABI_VERSION;
    header.structSize = structSize;
}

/// All diagnostics responses begin with ASFWDiagHeader.  Prime that envelope
/// before serializing an unsuccessful collection so clients can safely inspect
/// header.status instead of mistaking the response for an ABI mismatch.
template <typename T>
inline void PrimeDiagnosticsErrorHeader(T& value) noexcept {
    static_assert(std::is_standard_layout_v<T>);
    static_assert(offsetof(T, header) == 0,
                  "diagnostics response must begin with ASFWDiagHeader");
    PrimeDiagnosticsErrorHeader(value.header, static_cast<uint32_t>(sizeof(T)));
}

/// Repairs a response whose collector returned before it could initialize the
/// ABI header. A fully initialized success response is left intact.
template <typename T>
inline void EnsureDiagnosticsWireEnvelope(T& value) noexcept {
    if (value.header.abiVersion != ASFW_DIAG_ABI_VERSION ||
        value.header.structSize != static_cast<uint32_t>(sizeof(T))) {
        PrimeDiagnosticsErrorHeader(value);
    }
}

} // namespace ASFW::UserClient::Wire
