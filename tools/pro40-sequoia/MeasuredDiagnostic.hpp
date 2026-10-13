// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DiagnosticRecords.hpp"
#include "NativePacketBuffer.hpp"
struct MeasuredDiagnostic {
    unsigned stage=0,index=0,transport=0,sequence=0;
    uint32_t status=0,stamp=0;
    NativePackets::PreparedBuffer copy;
};
template<size_t Count>
void emitMeasuredDiagnostics(FILE *log,const DiagnosticRecords<MeasuredDiagnostic,Count> &records,bool includePayload=true) {
    // Preparation can fail before any native publication record exists.
    for(size_t n=records.first();n<records.end();++n) {
        const auto &diag=records.read(n);
        if(!diag.stage)continue;
        const unsigned port=unsigned(n%2)+2;
        std::fprintf(log,"transmit-measured-result port=%u stage=%u index=%u transport=%u sequence=%u native-status=%08x stamp=%08x\n",
            port,diag.stage,diag.index,diag.transport,diag.sequence,diag.status,diag.stamp);
        if(diag.stage!=5 && includePayload) {
            std::fprintf(log,"transmit-measured-failed-copy port=%u hex=",port);
            for(auto byte:diag.copy.bytes)std::fprintf(log,"%02x",byte);
            std::fputc('\n',log);
        }
    }
}
