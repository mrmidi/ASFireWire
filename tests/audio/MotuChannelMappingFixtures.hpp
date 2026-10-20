// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Audio/Protocols/MOTU/MotuRegisters.hpp"
#include "Audio/Wire/MOTU/MotuPortLayout.hpp"
#include <array>
#include <algorithm>
#include <string>
#include <vector>

namespace MotuChannelFixtures {
using namespace ASFW::Encoding::Motu;
struct Group {
    uint32_t version;
    bool capture;
    std::vector<std::string> names;
    std::array<int,3> firstChunk;
    bool followsOptical{false};
};
inline Group Analog(uint32_t version, bool capture, std::array<int,3> first) {
    Group group{version,capture,{},first};
    for (unsigned i=1;i<=8;++i) group.names.push_back("Analog "+std::to_string(i));
    return group;
}
inline Group Pair(uint32_t version, bool capture, const char* left, const char* right,
                  std::array<int,3> first, bool followsOptical = false) {
    return {version,capture,{left,right},first,followsOptical};
}
// Independent physical offsets extracted from vendor stream creation/setup.
// Negative offsets mean the named group is disabled in that rate mode.
// Addresses and disagreements with older FFADO tables are in the mapping audit.
inline const std::vector<Group> groups = [] {
    std::vector<Group> result;
    const auto analog = [&](uint32_t version,bool capture,std::array<int,3> first) {
        result.push_back(Analog(version,capture,first));
    };
    const auto pair = [&](uint32_t version,bool capture,const char* l,const char* r,
                          std::array<int,3> first,bool tail=false) {
        result.push_back(Pair(version,capture,l,r,first,tail));
    };
    for (bool capture : {false,true}) {
        analog(1,capture,{0,-1,-1});
        pair(1,capture,"S/PDIF 1","S/PDIF 2",{8,-1,-1},true);
        analog(2,capture,{0,-1,-1});
        pair(2,capture,"AES/EBU 1","AES/EBU 2",{8,-1,-1},true);
        analog(3,capture,{2,2,-1});
        pair(3,capture,"S/PDIF 1","S/PDIF 2",{12,12,-1});
        analog(5,capture,{2,2,0});
        pair(5,capture,"AES/EBU 1","AES/EBU 2",capture ? std::array{10,10,-1} : std::array{12,12,-1});
        analog(9,capture,{2,2,0});
        pair(9,capture,"AES/EBU 1","AES/EBU 2",{10,10,-1});
        pair(9,capture,"Digital 1","Digital 2",{12,12,-1});
        for (uint32_t version : {0x15U,0x17U,0x19U,0x1bU}) analog(version,capture,{2,2,2});
    }
    pair(3,true,"Mic 1","Mic 2",{10,10,-1});
    for (uint32_t version : {3U,0xdU}) {
        pair(version,false,"Main L","Main R",{10,10,-1});
        pair(version,false,"Phones L","Phones R",{0,0,-1});
    }
    pair(3,true,"Mix Return L","Mix Return R",{0,0,-1});
    pair(0xd,true,"Mic 1","Mic 2",{2,2,-1});
    // UltraLite V2 calls the other inputs Analog 3-8, not Analog 1-2.
    Group ultralite{0xd,true,{}, {4,4,-1}};
    for (unsigned i=3;i<=8;++i) ultralite.names.push_back("Analog "+std::to_string(i));
    result.push_back(std::move(ultralite));
    analog(0xd,false,{2,2,-1});
    pair(0xd,true,"S/PDIF 1","S/PDIF 2",{10,10,-1});
    pair(0xd,false,"S/PDIF 1","S/PDIF 2",{12,12,-1});
    pair(0xd,true,"Mix Return L","Mix Return R",{0,0,-1});
    pair(5,true,"Return L","Return R",{0,0,-1});
    pair(5,false,"Main L","Main R",{10,10,-1});
    pair(5,false,"Phones L","Phones R",{0,0,-1});
    pair(9,true,"Return L","Return R",{0,0,-1});
    pair(9,false,"Phones L","Phones R",{0,0,-1});
    analog(0xf,true,{2,2,-1});
    pair(0xf,true,"Return L","Return R",{0,0,-1});
    pair(0xf,false,"Main L","Main R",{2,2,-1});
    pair(0xf,false,"Phones L","Phones R",{0,0,-1});
    pair(0x15,true,"Mic 1","Mic 2",{0,0,0});
    pair(0x15,true,"Reverb L","Reverb R",{10,-1,-1});
    pair(0x15,true,"Return L","Return R",{12,12,12});
    pair(0x15,true,"S/PDIF 1","S/PDIF 2",{14,14,-1});
    pair(0x15,false,"Main L","Main R",{10,10,-1});
    pair(0x15,false,"Phones L","Phones R",{0,-1,-1});
    pair(0x15,false,"S/PDIF 1","S/PDIF 2",{12,12,-1});
    pair(0x17,false,"Main L","Main R",{0,0,0});
    pair(0x17,false,"Phones L","Phones R",{14,-1,-1});
    for (bool capture : {false,true}) {
        pair(0x17,capture,"AES/EBU 1","AES/EBU 2",{10,10,-1});
        pair(0x17,capture,"S/PDIF 1","S/PDIF 2",{12,12,-1});
        pair(0x1b,capture,"AES/EBU 1","AES/EBU 2",{10,10,-1});
        pair(0x1b,capture,"S/PDIF 1","S/PDIF 2",{12,12,-1});
    }
    for (uint32_t version : {0x17U,0x1bU}) {
        pair(version,true,"Return L","Return R",{0,0,0});
        pair(version,true,"Reverb L","Reverb R",{16,-1,-1});
    }
    pair(0x1b,false,"Phones L","Phones R",{0,0,0});
    pair(0x19,true,"Return L","Return R",{0,0,0});
    pair(0x19,true,"Reverb L","Reverb R",{14,-1,-1});
    pair(0x19,true,"S/PDIF 1","S/PDIF 2",{10,10,-1});
    pair(0x19,false,"Main L","Main R",{0,0,0});
    pair(0x19,false,"Phones L","Phones R",{10,10,10});
    pair(0x19,false,"S/PDIF 1","S/PDIF 2",{12,12,-1});
    // Track16: vendor BoxTrack16::SetupStrmFwIds 0x51330 / SetupStreams 0x515a0.
    pair(0x39,false,"Main L","Main R",{0,0,0});
    pair(0x39,false,"Phones L","Phones R",{2,2,2});
    pair(0x39,false,"Analog 1","Analog 2",{4,4,4});
    pair(0x39,true,"Mic 1","Mic 2",{0,0,0});
    pair(0x39,true,"Guitar 1","Guitar 2",{2,2,2});
    result.push_back({0x39,true,{"Analog 1","Analog 2","Analog 3","Analog 4"},{4,4,4}});
    pair(0x39,true,"Return L","Return R",{8,8,8});
    pair(0x39,true,"Reverb L","Reverb R",{12,12,-1});
    return result;
}();

template <typename Fn>
void EachFormation(bool capture, Fn fn) {
    for (const auto& model : kModels) {
        if (!FireWireOnly(model.unitVersion)) continue;
        const std::vector<uint32_t> words =
            model.protocol == ProtocolVersion::V1 ? std::vector<uint32_t>{0,0x8000,0x4000,0xc000} :
            model.protocol == ProtocolVersion::V2 ? std::vector<uint32_t>{0,0x100,0x400,0x500,0xa00} :
            std::vector<uint32_t>{0,0x101,0x202,0x303,0x50101,0x550303};
        for (uint32_t rate : kClockRates) {
            if (!SupportsRate(model,rate)) continue;
            const auto mode = IndexToMode(static_cast<uint32_t>(RateToIndex(rate)));
            for (uint32_t word : words) {
                const auto chunks = ASFW::Audio::Motu::ResolvePcmChunks(word,mode,model.unitVersion);
                if (!chunks.opticalDecoded) continue;
                const auto wire = capture ? chunks.tx : chunks.rx;
                const auto fixed = capture ? model.captureChunks[mode] : model.playbackChunks[mode];
                fn(model,rate,mode,wire,wire-fixed);
            }
        }
    }
}
} // namespace MotuChannelFixtures

