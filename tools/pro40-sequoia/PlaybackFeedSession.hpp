// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFeedSession.hpp"
#include "PlaybackPcmQueue.hpp"
namespace Pro40Audio {
// Non-RT P4F0 receiver for the playback direction. Own one decoded pending
// message under backpressure; never resend an accepted batch or overwrite unread
// native PCM. Admission/port authentication and clock negotiation are external.
template<size_t Capacity=8192,size_t Channels=6> class PlaybackFeedSession {
    static_assert(Capacity>=512); // Must retain one maximum-sized P4F0 batch.
public:
    using Queue=PlaybackPcmQueue<Capacity,Channels>;
    struct Anchor {Queue &queue;uint64_t epoch;};
    static constexpr size_t maxPacketBytes=AudioFeedPacketFor<Channels>::maxBytes;
private:
    Queue &queue_;
    const uint64_t epoch_;
    uint64_t accepted_;
    AudioFeedBatchFor<Channels> batch_{};
    bool pending_=false,closed_=false;
    SessionResult terminal_=SessionResult::ok;
    SessionResult fail(SessionResult why) noexcept {
        closed_=true;pending_=false;terminal_=why;queue_.fail(PlaybackQueueResult::state);return why;
    }
public:
    PlaybackFeedSession(Anchor anchor,uint64_t firstFrame)
        :queue_(anchor.queue),epoch_(anchor.epoch),accepted_(firstFrame) {
        if(!epoch_ || queue_.written()!=firstFrame || queue_.consumed()!=firstFrame || queue_.fault()!=PlaybackQueueResult::ok)
            fail(SessionResult::wrongEpoch);
    }
    bool pending() const noexcept {return pending_;}
    bool closed() const noexcept {return closed_;}
    SessionResult terminal() const noexcept {return terminal_;}
    uint64_t acceptedNext() const noexcept {return accepted_;}
    uint64_t publishedNext() const noexcept {return queue_.written();}
    void close() noexcept {if(!closed_)fail(SessionResult::closed);}
    SessionResult flush() noexcept {
        if(closed_)return SessionResult::closed;
        if(queue_.fault()!=PlaybackQueueResult::ok)return fail(SessionResult::publicationFailure);
        if(!pending_)return SessionResult::ok;
        const auto result=queue_.write(epoch_,batch_.firstFrame,std::span(batch_.samples).first(batch_.frames*Channels));
        if(result==PlaybackQueueResult::full)return SessionResult::pending;
        if(result!=PlaybackQueueResult::ok)return fail(SessionResult::publicationFailure);
        pending_=false;return SessionResult::ok;
    }
    SessionResult receive(std::span<const uint8_t> message) noexcept {
        if(closed_)return SessionResult::closed;
        if(pending_)return SessionResult::busy;
        if(AudioFeedPacketFor<Channels>::decode(message,batch_)!=AudioFeedError::ok)return fail(SessionResult::malformed);
        if(batch_.epoch!=epoch_)return fail(SessionResult::wrongEpoch);
        if(batch_.firstFrame!=accepted_)return fail(SessionResult::discontinuity);
        accepted_+=batch_.frames;pending_=true;return flush();
    }
};
}
