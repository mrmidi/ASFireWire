// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../Logging/Logging.hpp"
#include <functional>
#include <utility>

namespace ASFW::Common {
/// A moved-from completion is disarmed. An abandoned armed completion cancels.
/// Destruction and invocation must occur on the owning serial queue.
template<class T>
class OnceCompletion final {
public:
    using Callback = std::function<void(T)>;
    OnceCompletion(Callback callback, T cancelled)
        : callback_(std::move(callback)), cancelled_(std::move(cancelled)) {}
    OnceCompletion(const OnceCompletion&) = delete;
    OnceCompletion& operator=(const OnceCompletion&) = delete;
    OnceCompletion(OnceCompletion&& other) noexcept
        : callback_(std::exchange(other.callback_, {})), cancelled_(std::move(other.cancelled_)) {}
    OnceCompletion& operator=(OnceCompletion&&) = delete;
    ~OnceCompletion() { if (callback_) Invoke(std::move(cancelled_)); }

    void Invoke(T value) {
        auto callback = std::exchange(callback_, {});
        if (callback) callback(std::move(value));
        else ASFW_LOG_WARNING(AVC, "[AvcCompletion] duplicate completion ignored");
    }
private:
    Callback callback_;
    T cancelled_;
};
} // namespace ASFW::Common
