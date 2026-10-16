// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeIdentityRequest.hpp"
#include <bit>
#include <span>
namespace Pro40Audio {
// Private same-host laboratory protocol. Integer fields are big endian; audit
// token is opaque bytes copied unchanged. Never serialize C++ struct padding.
inline constexpr size_t identityRequestSize=96,identityResultSize=108;
using IdentityRequestBytes=std::array<uint8_t,identityRequestSize>;
using IdentityResultBytes=std::array<uint8_t,identityResultSize>;
namespace IdentityCodec {
inline void put(std::span<uint8_t> b,size_t at,uint64_t value,size_t count){
    for(size_t i=0;i<count;++i)b[at+count-1-i]=uint8_t(value>>(8*i));
}
inline uint64_t get(std::span<const uint8_t> b,size_t at,size_t count){
    uint64_t value=0;for(size_t i=0;i<count;++i)value=(value<<8)|b[at+i];return value;
}
}
inline IdentityRequestBytes encodeIdentityRequest(const NativeIdentityRequest &r){
    static_assert(sizeof(audit_token_t)==32);
    IdentityRequestBytes b{};using namespace IdentityCodec;
    put(b,0,0x50344951,4);put(b,4,r.version,4);put(b,8,r.role,4);
    put(b,12,r.guid,8);put(b,20,r.epoch,8);
    std::memcpy(b.data()+28,r.requestID.data(),16);std::memcpy(b.data()+44,r.policyHash.data(),20);
    std::memcpy(b.data()+64,&r.token,32);return b;
}
inline std::optional<NativeIdentityRequest> decodeIdentityRequest(std::span<const uint8_t> b){
    using namespace IdentityCodec;
    if(b.size()!=identityRequestSize || get(b,0,4)!=0x50344951)return std::nullopt;
    NativeIdentityRequest r;r.version=get(b,4,4);r.role=get(b,8,4);r.guid=get(b,12,8);r.epoch=get(b,20,8);
    std::memcpy(r.requestID.data(),b.data()+28,16);std::memcpy(r.policyHash.data(),b.data()+44,20);
    std::memcpy(&r.token,b.data()+64,32);if(!r.valid())return std::nullopt;return r;
}
inline IdentityResultBytes encodeIdentityResult(const NativeIdentityResult &r){
    IdentityResultBytes b{};IdentityCodec::put(b,0,0x50344952,4);IdentityCodec::put(b,4,r.version,4);
    const auto request=encodeIdentityRequest(r.request);std::memcpy(b.data()+8,request.data(),request.size());
    IdentityCodec::put(b,104,std::bit_cast<uint32_t>(r.status),4);return b;
}
inline std::optional<NativeIdentityResult> decodeIdentityResult(std::span<const uint8_t> b){
    using namespace IdentityCodec;
    if(b.size()!=identityResultSize || get(b,0,4)!=0x50344952 || get(b,4,4)!=1)return std::nullopt;
    const auto r=decodeIdentityRequest(b.subspan(8,identityRequestSize));if(!r)return std::nullopt;
    return NativeIdentityResult{1,*r,std::bit_cast<int32_t>(uint32_t(get(b,104,4)))};
}
}
