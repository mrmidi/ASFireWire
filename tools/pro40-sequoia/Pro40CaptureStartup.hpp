// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Pro40Packets.hpp"

namespace Pro40Packets {
// Initial data is pre-roll, never captured PCM. Linux amdtp-stream.c:1442-1535
// drops initial receive batches before normal processing. Our retained Pro40
// capture (2026-09-20, transmit-measured/rejected) has one word of displacement
// in its first data payload; the following data is correctly framed. Quarantine
// exactly that first eight-frame payload, even when its labels are valid.
// Preserve/validate its envelope and timing so neither clock nor TX cadence is
// rebased. Every later packet still passes the ordinary receive parser. This
// object cannot be reset inside a stream. It never repairs or forwards samples.
class CaptureStartup {
    bool dataSeen_=false,preroll_=false;
    uint64_t ordinal_=0,prerollOrdinal_=UINT64_MAX;
    unsigned badPcm_=0,badMidi_=0;
    bool shifted_=false;
public:
    static constexpr unsigned discardedFrames=8;
    bool preroll() const {return preroll_;}
    uint64_t prerollOrdinal() const {return prerollOrdinal_;}
    unsigned badPcmLabels() const {return badPcm_;}
    unsigned badMidiLabels() const {return badMidi_;}
    bool shiftedLabels() const {return shifted_;}
    Parsed parse(std::span<const uint8_t> bytes,unsigned stream,uint8_t sender) {
        preroll_=false;
        if(stream>=capture.size())return {{},Error::layout};
        if(dataSeen_) {++ordinal_;return parseCaptureTiming(bytes,stream,sender);}
        const auto result=parseEnvelope(bytes,capture[stream],sender);
        if(!result)return result;
        if(result.packet.blocks) {
            dataSeen_=preroll_=true;prerollOrdinal_=ordinal_;
            const auto layout=capture[stream];
            shifted_=read32(bytes.data()+8)==0;
            for(unsigned f=0;f<8;++f)for(unsigned c=0;c<layout.slots();++c) {
                const unsigned word=f*layout.slots()+c;
                const auto label=bytes[8+word*4];
                if(c<layout.pcm)badPcm_+=label!=0x40;
                else badMidi_+=label<0x80 || label>0x83;
                if(word) {
                    const auto previous=(word-1)%layout.slots();
                    shifted_&=previous<layout.pcm?label==0x40:label>=0x80 && label<=0x83;
                }
            }
        }
        ++ordinal_;return result;
    }
};
}
