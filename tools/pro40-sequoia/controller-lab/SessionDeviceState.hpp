// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Pro40Lab {
struct SessionDeviceState { uint32_t generation; uint16_t node; };
// Exact, bounded helper protocol. No partial results or cached generation.
inline std::optional<SessionDeviceState> parseSessionDeviceState(std::string_view text, uint64_t guid) {
    constexpr std::string_view prefix="PRO40_DEVICE_STATE 1 ";
    if(!text.starts_with(prefix) || text.size()>96 || !text.ends_with('\n'))return std::nullopt;
    text.remove_prefix(prefix.size());text.remove_suffix(1);
    const auto first=text.find(' ');
    if(first!=16)return std::nullopt;
    uint64_t observed=0;
    auto parsed=std::from_chars(text.data(),text.data()+first,observed,16);
    if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+first || observed!=guid || !guid)return std::nullopt;
    text.remove_prefix(first+1);
    const auto next=text.find(' ');
    if(next==std::string_view::npos || !next)return std::nullopt;
    uint32_t generation=0;uint16_t node=0;
    parsed=std::from_chars(text.data(),text.data()+next,generation);
    if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+next)return std::nullopt;
    text.remove_prefix(next+1);
    parsed=std::from_chars(text.data(),text.data()+text.size(),node);
    if(text.empty() || parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || (node&63)==63)return std::nullopt;
    return SessionDeviceState{generation,node};
}
}
