// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SequentialInputReader.hpp"
#include "SourceFrameClock.hpp"
namespace Pro40Audio {
template<size_t Capacity> class NativeAudioStartup;
// Offline serialized content owner. Only successful startup may initialize it.
// Owns copied PCM, publication and cursor: no view points back into startup.
// Input blocks must already have passed transport/timing validation externally.
template<size_t Capacity> class NativeInputContinuation {
    friend class NativeAudioStartup<Capacity>;
public:
    using Reader=SequentialInputReader<Capacity>;
    using Result=typename Reader::Result;
    enum class State {empty,running,draining,finished,failed};
    NativeInputContinuation()=default;
    NativeInputContinuation(const NativeInputContinuation &)=delete;
    NativeInputContinuation &operator=(const NativeInputContinuation &)=delete;
    State state() const noexcept {return state_;}
    SourceFrameClock::Identity identity() const noexcept {return identity_;}
    uint64_t nextRead() const noexcept {return reader_?reader_->next():0;}
    uint64_t nextInput() const noexcept {return window_?window_->next():0;}
    bool append(const InputBlock &block) noexcept {
        if(state_!=State::running)return fail();
        return window_->append(block)==WindowResult::ok?true:fail();
    }
    bool publish() noexcept {
        if(state_!=State::running)return fail();
        return publication_->tryPublish(*window_)==PublicationResult::ok?true:fail();
    }
    Result read(std::span<int32_t> output) noexcept {
        std::fill(output.begin(),output.end(),0);
        if(state_==State::finished)return Result::end;
        if(state_!=State::running&&state_!=State::draining)return Result::failed;
        const auto result=reader_->read(output);
        if(result==Result::failed){fail();return result;}
        if(reader_->endFrame()&&reader_->next()==*reader_->endFrame()){
            state_=State::finished;publication_->close();
        }
        return result;
    }
    bool finish() noexcept {
        if(!publish())return false;
        const auto result=reader_->finish(window_->next());
        if(result!=Result::ok&&result!=Result::end)return fail();
        state_=result==Result::end?State::finished:State::draining;
        if(state_==State::finished)publication_->close();
        return true;
    }
    void stop() noexcept {if(state_!=State::finished)fail();}
private:
    bool adopt(const AudioInputWindow<Capacity> &window,SourceFrameClock::Identity id,uint64_t first) noexcept {
        if(state_!=State::empty||window.epoch()!=id.epoch||first<window.oldest()||first>window.next())return false;
        window_.emplace(window);publication_.emplace(*window_);
        reader_.emplace(*publication_,id.epoch,first);identity_=id;state_=State::running;return true;
    }
    bool fail() noexcept {
        if(reader_)reader_->stop();if(publication_)publication_->close();state_=State::failed;return false;
    }
    State state_=State::empty;SourceFrameClock::Identity identity_{};
    std::optional<AudioInputWindow<Capacity>> window_;
    std::optional<AudioWindowPublication<Capacity>> publication_;
    // Destroy cursor (and final pin) before publication and mutable storage.
    std::optional<Reader> reader_;
};
}
