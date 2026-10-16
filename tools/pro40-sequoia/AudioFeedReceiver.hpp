// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFeedPacket.hpp"
#include "AudioInputWindow.hpp"
#include <optional>

namespace Pro40Audio {
enum class FeedResult {ok, closed, malformed, wrongEpoch, discontinuity};
// Single non-RT owner. Admission (expected epoch/firstFrame) comes from a future
// authenticated control handshake, never from the first unsolicited packet.
// After fault/close, construct a fresh receiver for a newly admitted session.
template<size_t Capacity,size_t Channels=1> class AudioFeedReceiver {
    AudioInputWindow<Capacity,Channels> window_;
    bool closed_=false;
public:
    AudioFeedReceiver(uint64_t epoch,uint64_t firstFrame):window_(epoch,firstFrame),closed_(epoch==0) {}
    void close() {closed_=true;}
    bool closed() const {return closed_;}
    uint64_t next() const {return window_.next();}
    // Owner-thread only: copy owned storage, never expose a mutable/shared view.
    std::optional<AudioInputWindow<Capacity,Channels>> snapshot() const {
        if(closed_)return std::nullopt;
        return window_;
    }
    FeedResult receive(std::span<const uint8_t> message) {
        if(closed_)return FeedResult::closed;
        AudioFeedBatchFor<Channels> batch;
        if(AudioFeedPacketFor<Channels>::decode(message,batch)!=AudioFeedError::ok) {closed_=true;return FeedResult::malformed;}
        if(batch.epoch!=window_.epoch()) {closed_=true;return FeedResult::wrongEpoch;}
        if(batch.firstFrame!=window_.next()) {closed_=true;return FeedResult::discontinuity;}
        // Whole-message preflight completed: append cannot partially fail from
        // malformed data, a range overflow or a discontinuous eight-frame block.
        for(uint32_t at=0;at<batch.frames;at+=8) {
            if(window_.append(batch.epoch,batch.firstFrame+at,
                std::span(batch.samples).subspan(at*Channels,8*Channels))!=WindowResult::ok) {
                closed_=true;return FeedResult::malformed;
            }
        }
        return FeedResult::ok;
    }
    WindowResult read(uint64_t epoch,uint64_t firstFrame,std::span<int32_t> output) const {
        if(closed_) {std::fill(output.begin(),output.end(),0);return WindowResult::notYetAvailable;}
        return window_.read(epoch,firstFrame,output);
    }
};
}
