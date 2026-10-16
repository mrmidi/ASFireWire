// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFeedReceiver.hpp"
#include "AudioWindowPublication.hpp"

namespace Pro40Audio {
enum class SessionResult {ok, pending, busy, closed, malformed, wrongEpoch, discontinuity, publicationFailure};
// Process-local composition. One non-RT owner calls receive/flush/close and
// reads status. Concurrent readers may only call tryAcquire() and use leases.
// Close admission, quiesce/join ALL callers, release leases, then destroy.
// Neither the class nor a lease may outlive the owning session allocation.
template<size_t Capacity,size_t Channels=1> class AudioFeedSession {
    AudioFeedReceiver<Capacity,Channels> receiver_;
    AudioWindowPublication<Capacity,Channels> publication_;
    bool pending_=false,closed_=false;
    SessionResult terminal_=SessionResult::ok;
    uint64_t publishedNext_;
    SessionResult fail(SessionResult reason) {
        receiver_.close();publication_.close();pending_=false;closed_=true;
        terminal_=reason;return reason;
    }
public:
    static constexpr size_t maxPacketBytes=AudioFeedPacketFor<Channels>::maxBytes;
    // Retain an entire maximum-size pending batch until it can be published.
    static_assert(Capacity>=AudioFeedBatch::maxFrames);
    AudioFeedSession(uint64_t epoch,uint64_t firstFrame):receiver_(epoch,firstFrame),
        publication_(AudioInputWindow<Capacity,Channels>(epoch,firstFrame)),publishedNext_(firstFrame) {
        if(!epoch)fail(SessionResult::wrongEpoch);
    }
    auto tryAcquire() noexcept {return publication_.tryAcquire();}
    bool closed() const {return closed_;} // Owner-only, not reader telemetry.
    bool pending() const {return pending_;}
    SessionResult terminal() const {return terminal_;}
    uint64_t acceptedNext() const {return receiver_.next();}
    uint64_t publishedNext() const {return publishedNext_;}
    void close() {if(!closed_)fail(SessionResult::closed);}
    SessionResult flush() {
        if(closed_)return SessionResult::closed;
        if(!pending_)return SessionResult::ok;
        const auto snapshot=receiver_.snapshot();
        if(!snapshot)return fail(SessionResult::publicationFailure);
        const auto result=publication_.tryPublish(*snapshot);
        if(result==PublicationResult::noSlot)return SessionResult::pending;
        if(result!=PublicationResult::ok)return fail(SessionResult::publicationFailure);
        publishedNext_=snapshot->next();pending_=false;return SessionResult::ok;
    }
    SessionResult receive(std::span<const uint8_t> bytes) {
        if(closed_)return SessionResult::closed;
        // Busy means this message was NOT consumed. Pending means it WAS
        // accepted but is not yet published; caller must flush, not resend it.
        if(pending_)return SessionResult::busy;
        const auto result=receiver_.receive(bytes);
        switch(result) {
            case FeedResult::ok:pending_=true;return flush();
            case FeedResult::wrongEpoch:return fail(SessionResult::wrongEpoch);
            case FeedResult::discontinuity:return fail(SessionResult::discontinuity);
            default:return fail(SessionResult::malformed);
        }
    }
};
}
