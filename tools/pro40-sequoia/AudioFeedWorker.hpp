// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFeedSession.hpp"
#include "AudioFeedControl.hpp"
#include <chrono>
#include <thread>
#include <utility>

namespace Pro40Audio {
enum class FeedPoll {message, idle, disconnected, error};
enum class WorkerEnd {stopped, idleTimeout, pendingTimeout, disconnected, transportError, feedFault, invalidConfiguration, drained, drainTimeout};
struct WorkerReport {
    WorkerEnd end;
    SessionResult feed;
    bool transportClosed;
    uint64_t acceptedNext,publishedNext;
};
// Exactly one externally owned non-RT thread calls run() once. Worker owns its
// transport and session. Transport poll(buffer, used, deadline) must return by
// deadline and yield ONE complete message; close() must be bounded, noexcept.
// requestStop/tryAcquire/readInput/report may be called concurrently (reader
// API availability depends on Session). Destruction requires
// run thread joined AND all reader callers/leases quiesced. No implicit detach.
template<size_t Capacity,class Transport,class Session=AudioFeedSession<Capacity>> class AudioFeedWorker {
public:
    using Clock=std::chrono::steady_clock;
    struct Limits {
        std::chrono::milliseconds poll{5},idle{1000},pending{100};
    };
private:
    Transport transport_;
    Session session_;
    const Limits limits_;
    std::atomic<bool> started_{false},stop_{false},finished_{false};
    WorkerReport report_{WorkerEnd::invalidConfiguration,SessionResult::ok,false,0,0};
    void finish(WorkerEnd end) noexcept {
        const auto feed=session_.terminal();
        session_.close();
        report_={end,feed,transport_.close(),session_.acceptedNext(),session_.publishedNext()};
        finished_.store(true,std::memory_order_release);
    }
public:
    template<class Anchor>
    AudioFeedWorker(Transport transport,Anchor anchor,uint64_t firstFrame,Limits limits={}):
        transport_(std::move(transport)),session_(anchor,firstFrame),limits_(limits) {
        static_assert(noexcept(transport_.poll(std::declval<std::span<uint8_t>>(),
            std::declval<size_t &>(),std::declval<Clock::time_point>())));
        static_assert(noexcept(transport_.close()));
    }
    void requestStop() noexcept {stop_.store(true,std::memory_order_release);}
    auto tryAcquire() noexcept {return session_.tryAcquire();}
    bool postDrainAck(std::span<const uint8_t> bytes) noexcept requires requires(Session &s) {s.postDrainAck(bytes);} {
        return session_.postDrainAck(bytes);
    }
    template<class Result> bool completeRead(const Result &result) noexcept {
        static_assert(noexcept(session_.completeRead(result)));
        return session_.completeRead(result);
    }
    // Available when the session supplies callback reads. No mutable session
    // escapes, and all worker shutdown/deadline logic remains shared.
    template<class... Args> auto readInput(Args&&... args) noexcept {
        static_assert(noexcept(session_.readInput(std::forward<Args>(args)...)));
        return session_.readInput(std::forward<Args>(args)...);
    }
    std::optional<WorkerReport> report() const noexcept {
        if(!finished_.load(std::memory_order_acquire))return std::nullopt;
        return report_; // Written once, published only after transport close.
    }
    bool run() {
        if(started_.exchange(true))return false; // No concurrent run or restart.
        if(session_.closed() || limits_.poll.count()<=0 || limits_.idle.count()<=0 || limits_.pending.count()<=0 ||
           limits_.poll>std::chrono::seconds(1) || limits_.idle>std::chrono::minutes(1) || limits_.pending>std::chrono::minutes(1)) {
            finish(WorkerEnd::invalidConfiguration);return true;
        }
        auto idleSince=Clock::now(),pendingSince=idleSince;
        std::array<uint8_t,Session::maxPacketBytes> bytes{};
        for(;;) {
            if(stop_.load(std::memory_order_acquire)){finish(WorkerEnd::stopped);return true;}
            if constexpr(requires(Session &s){s.pollLifecycle();}) {
                const auto life=session_.pollLifecycle();
                if(life!=FeedLifecycle::active) {
                    finish(life==FeedLifecycle::drained?WorkerEnd::drained:
                           life==FeedLifecycle::timedOut?WorkerEnd::drainTimeout:WorkerEnd::feedFault);
                    return true;
                }
            }
            const auto now=Clock::now();
            if(session_.pending()) {
                if(now-pendingSince>=limits_.pending){finish(WorkerEnd::pendingTimeout);return true;}
                const auto result=session_.flush();
                if(result==SessionResult::pending) {
                    // Owner thread only. Never poll another message while this
                    // one is pending; no unbounded queue or realtime retry.
                    std::this_thread::sleep_until(std::min(pendingSince+limits_.pending,now+limits_.poll));
                    continue;
                }
                if(result!=SessionResult::ok){finish(WorkerEnd::feedFault);return true;}
                idleSince=Clock::now();
            }
            if(Clock::now()-idleSince>=limits_.idle){finish(WorkerEnd::idleTimeout);return true;}
            size_t used=0;
            auto pollDeadline=std::min(idleSince+limits_.idle,Clock::now()+limits_.poll);
            if constexpr(requires(Session &s){s.pollDeadline(pollDeadline);})
                pollDeadline=session_.pollDeadline(pollDeadline);
            const auto polled=transport_.poll(bytes,used,pollDeadline);
            if(stop_.load(std::memory_order_acquire)){finish(WorkerEnd::stopped);return true;}
            if(Clock::now()-idleSince>=limits_.idle){finish(WorkerEnd::idleTimeout);return true;}
            if(polled==FeedPoll::idle) {std::this_thread::sleep_until(pollDeadline);continue;}
            if(polled==FeedPoll::disconnected){finish(WorkerEnd::disconnected);return true;}
            if(polled==FeedPoll::error || used>bytes.size()) {finish(WorkerEnd::transportError);return true;}
            if(polled!=FeedPoll::message){finish(WorkerEnd::transportError);return true;}
            const auto accepted=session_.receive(std::span(bytes).first(used));
            if(accepted!=SessionResult::ok && accepted!=SessionResult::pending){finish(WorkerEnd::feedFault);return true;}
            idleSince=Clock::now();
            if(accepted==SessionResult::pending)pendingSince=idleSince;
        }
    }
};
}
