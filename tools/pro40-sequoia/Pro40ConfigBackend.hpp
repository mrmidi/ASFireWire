// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Pro40OwnedSession.hpp"
#include "NativeReservations.hpp"
struct ConfigBackend {
    Pro40OwnedSession &session;
    int32_t guard() {
        Pro40ClockState state{};
        auto result=session.stable();
        if(!result) result=session.state(state);
        if(!result && (session.ownerOf(state)!=session.owner || state.enabled ||
            state.selected!=0x20c || state.measured!=48000 || state.status!=0x201)) result=kIOReturnNotPermitted;
        return result;
    }
    int32_t read(uint64_t address,uint32_t &value) {
        std::array<uint8_t,4> bytes{};
        const auto result=pro40_read(session.generation,address,4,bytes.data());
        if(!result) value=ownedBe32(bytes.data());
        std::fprintf(session.log,"config-read address=%012llx value=%08x status=%08x\n",
            (unsigned long long)address,value,unsigned(result));
        return result;
    }
    int32_t write(uint64_t address,uint32_t value) {
        FWAddress target{}; target.nodeID=session.remote;
        target.addressHi=UInt16(address>>32); target.addressLo=UInt32(address);
        const UInt32 bytes=OSSwapHostToBigInt32(value);UInt32 length=4;
        std::fprintf(session.log,"config-write address=%012llx value=%08x\n",(unsigned long long)address,value);
        Pro40Resources::checkpoint(session.log);
        const auto status=(*session.device)->Write(session.device,0,&target,&bytes,&length,true,session.generation);
        session.event("config-write-result",unsigned(status));
        return status?status:length==4?0:kIOReturnUnderrun;
    }
};
