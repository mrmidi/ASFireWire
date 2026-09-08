// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace ASFW::Audio {

// Controller-global host-stream reservation. The caller serializes every
// access with its control-plane lock; backend work runs outside that lock.
// Failed cleanup keeps ownership but must never be mistaken for running audio.
class AudioStreamReservation final {
public:
    enum class State { Idle, Starting, Running, Stopping, CleanupFailed };
    enum class Admission { Begin, AlreadyRunning, Busy, CleanupFailed };

    struct Token {
        uint64_t guid{0};
        uint64_t epoch{0};
    };
    struct Decision {
        Admission admission{Admission::Busy};
        Token token{};
    };

    [[nodiscard]] Decision BeginStart(uint64_t guid) noexcept {
        if (guid == 0) return {};
        if (guid_ == 0) return Begin(guid, State::Starting);
        if (guid_ != guid) return {};
        if (state_ == State::Running) return {Admission::AlreadyRunning, {}};
        if (state_ == State::CleanupFailed) return {Admission::CleanupFailed, {}};
        return {};
    }

    [[nodiscard]] Decision BeginStop(uint64_t guid) noexcept {
        if (guid == 0 || (guid_ != 0 && guid_ != guid)) return {};
        if (state_ == State::CleanupFailed) return {Admission::CleanupFailed, {}};
        if (state_ == State::Stopping) return {};
        // A stop may supersede an in-flight start. Its new epoch prevents a
        // late start completion from publishing Running over the stop state.
        return Begin(guid, State::Stopping);
    }

    bool CompleteStart(Token token, bool success) noexcept {
        if (!Matches(token, State::Starting)) return false;
        if (success) state_ = State::Running;
        else Clear(token.guid);
        return true;
    }

    bool CompleteStop(Token token, bool success) noexcept {
        if (!Matches(token, State::Stopping)) return false;
        if (success) Clear(token.guid);
        else state_ = State::CleanupFailed;
        return true;
    }

    // Only confirmed removal or service teardown clears failed cleanup. A bus
    // reset/resume alone does not prove outstanding device operations finished.
    void Clear(uint64_t guid) noexcept {
        if (guid_ != guid) return;
        guid_ = 0;
        state_ = State::Idle;
        ++epoch_;
    }
    void ClearAll() noexcept { Clear(guid_); }

    [[nodiscard]] uint64_t Guid() const noexcept { return guid_; }
    [[nodiscard]] State GetState() const noexcept { return state_; }
    [[nodiscard]] bool BlocksNewWork(uint64_t guid) const noexcept {
        return guid_ == guid &&
               (state_ == State::Stopping || state_ == State::CleanupFailed);
    }

private:
    [[nodiscard]] Decision Begin(uint64_t guid, State state) noexcept {
        guid_ = guid;
        state_ = state;
        return {Admission::Begin, {guid_, ++epoch_}};
    }
    [[nodiscard]] bool Matches(Token token, State state) const noexcept {
        return token.guid != 0 && token.guid == guid_ && token.epoch == epoch_ && state_ == state;
    }

    uint64_t guid_{0};
    uint64_t epoch_{0};
    State state_{State::Idle};
};

} // namespace ASFW::Audio
