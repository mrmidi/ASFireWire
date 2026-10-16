// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <optional>
namespace Pro40Lab {
// The command may wait for an app for hours, but once shutdown is observed it
// must still join promptly or return an unresolved failure. This owns no child.
class CommandWaitTiming final {
public:
    using Clock=std::chrono::steady_clock;
    using Time=Clock::time_point;
    static constexpr auto cleanup=std::chrono::seconds(30);
private:
    Time began_,last_;
    std::optional<std::chrono::milliseconds> limit_;
    std::optional<Time> stopped_;
    bool expired_=false;
public:
    CommandWaitTiming(Time began,std::optional<std::chrono::milliseconds> limit):began_(began),last_(began),limit_(limit){}
    bool expired(Time now,bool stopping) {
        if(expired_)return true;
        if(now<last_)return expired_=true;
        last_=now;
        if(stopping && !stopped_)stopped_=now;
        // Preserve the existing finite diagnostic envelope. A session instead
        // measures its final grace from the first observed stop, never renews it.
        expired_=limit_?now-began_>=*limit_:stopped_ && now-*stopped_>=cleanup;
        return expired_;
    }
};
}
