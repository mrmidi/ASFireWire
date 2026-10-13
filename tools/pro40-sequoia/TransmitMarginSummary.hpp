// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace Pro40 {
// Single event-loop owner. Completed native updates only; failed attempts remain
// separately counted and retain their detailed evidence in the existing tail.
struct TransmitMarginSummary {
    unsigned attempted=0,completed=0,failed=0;
    unsigned minimumBefore=0,minimumCut=0,minimumAfter=0;
    unsigned beforeOrdinal=0,cutOrdinal=0,afterOrdinal=0,slowestOrdinal=0;
    uint64_t maximumMicros=0;
    void observe(unsigned ordinal,unsigned before,unsigned cut,unsigned after,uint64_t micros,int32_t status) {
        ++attempted;
        if(status) {++failed;return;}
        if(!completed || before<minimumBefore) {minimumBefore=before;beforeOrdinal=ordinal;}
        if(!completed || cut<minimumCut) {minimumCut=cut;cutOrdinal=ordinal;}
        if(!completed || after<minimumAfter) {minimumAfter=after;afterOrdinal=ordinal;}
        if(!completed || micros>maximumMicros) {maximumMicros=micros;slowestOrdinal=ordinal;}
        ++completed;
    }
};
}
