// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <mach/message.h>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace Pro40Audio {
inline constexpr mach_msg_option_t nativeAuditReceiveOptions =
    MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
    MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);

// Structural extraction only, NOT authorization. Call only on the buffer from a
// successful mach_msg receive with audit trailers requested. A byte span cannot
// prove kernel provenance. Caller still validates the envelope and destroys any
// received rights, including on rejection. Never interprets opaque token fields.
// SDK mach/message.h: mach_msg_audit_trailer_t and round_msg define this layout.
[[nodiscard]] inline std::optional<audit_token_t> nativeAuditToken(
    std::span<const std::byte> received) noexcept {
    mach_msg_header_t header{};
    if(received.size()<sizeof(header))return std::nullopt;
    std::memcpy(&header,received.data(),sizeof(header));
    constexpr size_t padding=sizeof(natural_t)-1;
    static_assert((sizeof(natural_t)&padding)==0);
    // Guard the SDK macro's mach_msg_size_t addition before rounding.
    if(header.msgh_size<sizeof(header) ||
       header.msgh_size>std::numeric_limits<mach_msg_size_t>::max()-padding)
        return std::nullopt;
    const size_t offset=round_msg(header.msgh_size);
    if(offset>received.size() || received.size()-offset<sizeof(mach_msg_trailer_t))
        return std::nullopt;
    mach_msg_trailer_t prefix{};
    std::memcpy(&prefix,received.data()+offset,sizeof(prefix));
    if(prefix.msgh_trailer_type!=MACH_MSG_TRAILER_FORMAT_0 ||
       prefix.msgh_trailer_size<sizeof(mach_msg_audit_trailer_t) ||
       prefix.msgh_trailer_size>received.size()-offset)return std::nullopt;
    mach_msg_audit_trailer_t trailer{};
    std::memcpy(&trailer,received.data()+offset,sizeof(trailer));
    const audit_token_t invalid=INVALID_AUDIT_TOKEN_VALUE;
    if(std::memcmp(&trailer.msgh_audit,&invalid,sizeof(invalid))==0)return std::nullopt;
    return trailer.msgh_audit;
}
}
