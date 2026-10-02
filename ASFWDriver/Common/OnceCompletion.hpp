// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "MoveOnlyCallback.hpp"
#include "../Logging/Logging.hpp"
#include <utility>

namespace ASFW::Common {
/// A completion that runs exactly once. A moved-from completion is disarmed;
/// an abandoned armed completion delivers `cancelled`. The callback is held
/// move-only, so it can never be duplicated. Destruction and invocation must
/// occur on the owning serial queue.
template<class T>
class OnceCompletion final {
public:
    using Callback = MoveOnlyCallback<void(T)>;
    OnceCompletion(Callback callback, T cancelled)
        : callback_(std::move(callback)), cancelled_(std::move(cancelled)) {}
    OnceCompletion(const OnceCompletion&) = delete("a completion runs exactly once; copying would run it twice");
    OnceCompletion& operator=(const OnceCompletion&) = delete("a completion runs exactly once; copying would run it twice");
    OnceCompletion(OnceCompletion&& other) noexcept
        : callback_(std::exchange(other.callback_, nullptr)), cancelled_(std::move(other.cancelled_)) {}
    OnceCompletion& operator=(OnceCompletion&&) = delete("rebinding would drop an armed completion");
    ~OnceCompletion() { if (callback_) Invoke(std::move(cancelled_)); }

    void Invoke(T value) {
        auto callback = std::exchange(callback_, nullptr);
        if (callback) callback(std::move(value));
        else ASFW_LOG_WARNING(AVC, "[AvcCompletion] duplicate completion ignored");
    }
    [[nodiscard]] bool Armed() const noexcept { return static_cast<bool>(callback_); }
private:
    Callback callback_;
    T cancelled_;
};
} // namespace ASFW::Common
