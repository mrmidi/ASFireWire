// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeSessionRunner.hpp"
#include <array>
#include <span>
namespace Pro40 {
enum class SequenceStop {completed, invalidOptions, stopRequested, sessionFailed};
constexpr const char *name(SequenceStop stop) {
    switch(stop) {
    case SequenceStop::completed:return "completed";
    case SequenceStop::invalidOptions:return "invalid-options";
    case SequenceStop::stopRequested:return "stop-requested";
    default:return "session-failed";
    }
}
struct SessionSequenceResult {
    static constexpr unsigned maximum=8;
    SequenceStop stop=SequenceStop::invalidOptions;
    unsigned attempted=0;
    std::array<NativeSessionResult,maximum> sessions{};
};
// Fixed finite sequence, no retries. Run must be synchronous and return only
// after native cleanup. Stop persists across calls; no automatic flag reset.
// Only an accepted complete session permits the next call. Preflight every
// log path before the first call; native fopen still enforces exclusive create.
template<class Run>
SessionSequenceResult runSessionSequence(const NativeSessionOptions &base,
                                         std::span<const std::string> paths,Run &&run) {
    SessionSequenceResult result;
    if(paths.empty() || paths.size()>result.maximum)return result;
    for(unsigned i=0;i<paths.size();++i) {
        if(paths[i].empty())return result;
        for(unsigned j=0;j<i;++j)if(paths[i]==paths[j])return result;
    }
    auto options=base;
    for(const auto &path:paths) {
        if(options.stop.requested()) {result.stop=SequenceStop::stopRequested;return result;}
        options.logPath=path;
        auto &session=result.sessions[result.attempted++];
        session=run(options);
        if(!session.accepted()) {result.stop=SequenceStop::sessionFailed;return result;}
    }
    result.stop=SequenceStop::completed;
    return result;
}
}
