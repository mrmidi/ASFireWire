// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceDeviceIo.cpp - Blocking DICE register access for the linear bring-up.

#include "DiceDeviceIo.hpp"

#include "../../../../Logging/Logging.hpp"

#include <memory>
#include <span>
#include <utility>

namespace ASFW::Audio::DICE {

namespace {

// Completion state shared with the callback. It outlives a timed-out wait, so
// a late completion never writes into a returned stack frame.
template <typename T>
struct Pending {
    std::atomic<bool> done{false};
    IOReturn status{kIOReturnSuccess};
    T value{};

    void Complete(IOReturn completionStatus, T completionValue) {
        status = completionStatus;
        value = std::move(completionValue);
        done.store(true, std::memory_order_release);
    }
};

struct NoValue {};

[[nodiscard]] IOReturn MapStatus(Async::AsyncStatus status) noexcept {
    return Protocols::Ports::MapAsyncStatusToIOReturn(status);
}

} // namespace

bool DiceDeviceIo::AwaitCompletion(const std::atomic<bool>& done, const char* what) {
    const uint64_t startMs = clock_.NowMs();
    for (uint32_t attempt = 0; !done.load(std::memory_order_acquire); ++attempt) {
        if (clock_.NowMs() - startMs >= kSafetyDeadlineMs) {
            ASFW_LOG_ERROR(DICE,
                           "DiceDeviceIo: %{public}s did not complete within %u ms; "
                           "was the bring-up called on the bus completion queue?",
                           what, kSafetyDeadlineMs);
            return false;
        }
        clock_.Backoff(attempt);
    }
    return true;
}

// Issue one asynchronous operation through `start`, which receives the shared
// completion state, and wait for it.
template <typename T, typename Start>
std::expected<T, IOReturn> DiceDeviceIo::Run(const char* what, Start&& start) {
    auto pending = std::make_shared<Pending<T>>();
    start(pending);
    if (!AwaitCompletion(pending->done, what)) {
        return std::unexpected(kIOReturnTimeout);
    }
    if (pending->status != kIOReturnSuccess) {
        return std::unexpected(pending->status);
    }
    return std::move(pending->value);
}

std::expected<uint32_t, IOReturn> DiceDeviceIo::ReadQuad(uint32_t offset) {
    return Run<uint32_t>("ReadQuad", [&](auto pending) {
        (void)io_.ReadQuadBE(MakeDICEAddress(offset),
                             [pending](Async::AsyncStatus status, uint32_t value) {
                                 pending->Complete(MapStatus(status), value);
                             });
    });
}

std::expected<std::vector<uint8_t>, IOReturn> DiceDeviceIo::ReadBlock(uint32_t offset,
                                                                     uint32_t length) {
    return Run<std::vector<uint8_t>>("ReadBlock", [&](auto pending) {
        (void)io_.ReadBlock(MakeDICEAddress(offset), length,
                            [pending](Async::AsyncStatus status, std::span<const uint8_t> payload) {
                                pending->Complete(MapStatus(status),
                                                  std::vector<uint8_t>(payload.begin(), payload.end()));
                            });
    });
}

std::expected<void, IOReturn> DiceDeviceIo::WriteQuad(uint32_t offset, uint32_t value) {
    const auto result = Run<NoValue>("WriteQuad", [&](auto pending) {
        (void)io_.WriteQuadBE(MakeDICEAddress(offset), value,
                              [pending](Async::AsyncStatus status) {
                                  pending->Complete(MapStatus(status), NoValue{});
                              });
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

std::expected<uint64_t, IOReturn> DiceDeviceIo::CompareSwap64(uint32_t offset,
                                                              uint64_t expected,
                                                              uint64_t desired) {
    return Run<uint64_t>("CompareSwap64", [&](auto pending) {
        (void)io_.CompareSwap64BE(MakeDICEAddress(offset), expected, desired,
                                  [pending](Async::AsyncStatus status, uint64_t previous) {
                                      pending->Complete(MapStatus(status), previous);
                                  });
    });
}

std::expected<GeneralSections, IOReturn> DiceDeviceIo::ReadGeneralSections() {
    return Run<GeneralSections>("ReadGeneralSections", [&](auto pending) {
        transaction_.ReadGeneralSections([pending](IOReturn status, GeneralSections sections) {
            pending->Complete(status, sections);
        });
    });
}

std::expected<GlobalState, IOReturn> DiceDeviceIo::ReadGlobalStateFull(const GeneralSections& sections) {
    return Run<GlobalState>("ReadGlobalStateFull", [&](auto pending) {
        transaction_.ReadGlobalStateFull(sections, [pending](IOReturn status, GlobalState state) {
            pending->Complete(status, state);
        });
    });
}

std::expected<GlobalState, IOReturn> DiceDeviceIo::ReadGlobalState(const GeneralSections& sections) {
    return Run<GlobalState>("ReadGlobalState", [&](auto pending) {
        transaction_.ReadGlobalState(sections, [pending](IOReturn status, GlobalState state) {
            pending->Complete(status, state);
        });
    });
}

std::expected<StreamConfig, IOReturn> DiceDeviceIo::ReadTxStreamConfig(const GeneralSections& sections) {
    return Run<StreamConfig>("ReadTxStreamConfig", [&](auto pending) {
        transaction_.ReadTxStreamConfig(sections, [pending](IOReturn status, StreamConfig config) {
            pending->Complete(status, config);
        });
    });
}

std::expected<StreamConfig, IOReturn> DiceDeviceIo::ReadRxStreamConfig(const GeneralSections& sections) {
    return Run<StreamConfig>("ReadRxStreamConfig", [&](auto pending) {
        transaction_.ReadRxStreamConfig(sections, [pending](IOReturn status, StreamConfig config) {
            pending->Complete(status, config);
        });
    });
}

} // namespace ASFW::Audio::DICE
