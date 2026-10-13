// Read-only timing diagnostic shared by standalone and signed controller probes.
#pragma once
#include "JobObservation.hpp"
#include <cstdio>
namespace Pro40Lab {
inline int reportBtmTiming(std::chrono::seconds budget) {
    const auto start=std::chrono::steady_clock::now();
    auto r=Pro40Lab::observeProcess({"/usr/bin/sfltool","dumpbtm"},
        budget,1048576);
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::printf("{\"exit\":%d,\"usable\":%s,\"reaped\":%s,\"timeout\":%s,"
        "\"overflow\":%s,\"io_error\":%s,\"spawn_error\":%d,\"stdout_bytes\":%zu,"
        "\"stderr_bytes\":%zu,\"seconds\":%.6f}\n",r.exit,r.usable()?"true":"false",
        r.reaped?"true":"false",r.timedOut?"true":"false",r.overflow?"true":"false",
        r.ioError?"true":"false",r.spawnError,r.out.size(),r.err.size(),seconds);
    return r.reaped?0:2;
}
}
