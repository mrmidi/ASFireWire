// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RecordingExport.hpp"
#include "../NativeTimingEvidence.hpp"
namespace Pro40 {
// Post-teardown only; independent diagnostic status cannot undo native cleanup.
inline bool exportTiming(const std::string &log,const NativeTimingEvidence &timing,
                         const Pro40Packets::InputRecording &recording,const NativeSessionResult &result) {
    const char *status="skipped";
    if(result.accepted()) {
        status="failed";
        if(timing.valid() && !recording.fault() && timing.packets.frames()==recording.pcm().size()/3) {
            std::string packets="ordinal,first_frame,generation,arrival_cycle,blocks,dbc,syt\n";
            char row[256];size_t ordinal=0;
            for(const auto &r:timing.packets.records()) {
                std::snprintf(row,sizeof(row),"%zu,%llu,%u,%u,%u,%u,%u\n",ordinal++,
                    (unsigned long long)r.firstFrame,r.generation,r.arrivalCycle,unsigned(r.blocks),unsigned(r.dbc),unsigned(r.syt));
                packets+=row;
            }
            std::string clocks="progress,generation,host_before,cycle_raw,uptime_raw,host_after,status,packet_count,before_receive\n";
            for(size_t n=0;n<timing.used;++n) {
                const auto &r=timing.clocks[n];
                std::snprintf(row,sizeof(row),"%u,%u,%llu,%u,%llu,%llu,%u,%u,%u\n",r.progress,r.generation,
                    (unsigned long long)r.before,r.cycle,(unsigned long long)r.uptime,(unsigned long long)r.after,r.status,
                    r.packetCount,unsigned(r.beforeReceive));
                clocks+=row;
            }
            auto bytes=[](const std::string &s){return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(s.data()),s.size());};
            if(publishRecordingFile(log+".input-timing.csv",bytes(packets)) && publishRecordingFile(log+".clock-pairs.csv",bytes(clocks)))status="saved";
        }
    }
    char report[384];
    const int size=std::snprintf(report,sizeof(report),
        "{\"schema\":2,\"status\":\"%s\",\"packets\":%zu,\"frames\":%llu,\"clock_pairs\":%zu,\"numer\":%u,\"denom\":%u,\"packet_fault\":%s,\"clock_fault\":%s}\n",
        status,timing.packets.records().size(),(unsigned long long)timing.packets.frames(),timing.used,timing.numer,timing.denom,
        timing.packets.fault()?"true":"false",timing.fault?"true":"false");
    const bool reported=size>0 && size_t(size)<sizeof(report) && publishRecordingFile(log+".timing.json",
        {reinterpret_cast<const uint8_t *>(report),size_t(size)});
    std::fprintf(stderr,"input-timing: %s report=%s\n",status,reported?"saved":"failed");
    return reported && std::string(status)=="saved";
}
}
