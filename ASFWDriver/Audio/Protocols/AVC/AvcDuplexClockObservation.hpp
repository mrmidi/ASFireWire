// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <utility>

#include "../../../Protocols/AVC/Core/IAvcUnit.hpp"
#include "../../../Protocols/AVC/Commands/GeneralCommands.hpp"
#include <DriverKit/IOReturn.h>
#include <atomic>
#include <functional>
#include <memory>

namespace ASFW::Audio {

struct AvcDuplexClockObservation final {
    uint32_t outputRateHz{0}; // device -> host
    uint32_t inputRateHz{0};  // host -> device
};

// Read-only observation. Linux bebob_stream.c:63-82 queries OUTPUT then INPUT;
// fcp.c:84-136 uses AM824 wildcard STATUS and rejects IN TRANSITION. Unlike
// Linux's get-rate synchronization, observation never writes either plug.
class AvcDuplexClockRead final : public std::enable_shared_from_this<AvcDuplexClockRead> {
public:
    using Completion = std::function<void(IOReturn, AvcDuplexClockObservation)>;
    static void Start(std::shared_ptr<AVC::IAvcUnit> unit,
                      Discovery::DeviceRouteToken route, uint8_t outputPlug,
                      uint8_t inputPlug, std::shared_ptr<std::atomic<bool>> active,
                      Completion completion, bool inputOnly = false) {
        auto read = std::shared_ptr<AvcDuplexClockRead>(new AvcDuplexClockRead(
            std::move(unit), route, outputPlug, inputPlug, std::move(active), std::move(completion)));
        read->input_ = inputOnly;
        read->Ask();
    }
private:
    AvcDuplexClockRead(std::shared_ptr<AVC::IAvcUnit> unit, Discovery::DeviceRouteToken route,
                      uint8_t outputPlug, uint8_t inputPlug,
                      std::shared_ptr<std::atomic<bool>> active, Completion completion)
        : unit_(std::move(unit)), route_(route), outputPlug_(outputPlug), inputPlug_(inputPlug),
          active_(std::move(active)), completion_(std::move(completion)) {}
    void Finish(IOReturn status) {
        if (completion_) std::exchange(completion_, {})(status, observation_);
    }
    void Ask() {
        if (!active_->load(std::memory_order_acquire) || !unit_ || !unit_->IsCurrentRoute(route_)) {
            Finish(kIOReturnAborted); return;
        }
        const AVC::Cmd::PlugSignalFormatCommand command{.operands = {
            .direction = input_ ? AVC::Cmd::PlugSignalDirection::kInput : AVC::Cmd::PlugSignalDirection::kOutput,
            .plugId = input_ ? inputPlug_ : outputPlug_,
            .query = AVC::Cmd::SignalFormatQuery::kAm824Wildcard}};
        const auto encoded = command.Encode(AVC::CommandType::kStatus);
        if (!encoded) { Finish(kIOReturnBadArgument); return; }
        unit_->Submit(*encoded, route_.generation,
            [self = shared_from_this(), command](AVC::Expected<AVC::Response> reply) {
                if (!self->active_->load(std::memory_order_acquire) || !self->unit_->IsCurrentRoute(self->route_)) {
                    self->Finish(kIOReturnAborted); return;
                }
                if (!reply) { self->Finish(AVC::ToIOReturn(reply.error())); return; }
                if (reply->code == AVC::ResponseCode::kInTransition) {
                    if (++self->attempt_ < 3) self->Ask();
                    else self->Finish(kIOReturnNotReady);
                    return;
                }
                if (reply->code != AVC::ResponseCode::kImplementedStable) {
                    self->Finish(kIOReturnUnsupported); return;
                }
                const auto format = command.Decode(reply->operands);
                const auto sfc = format ? AVC::Cmd::SfcOf(*format) : std::nullopt;
                const auto rate = sfc ? AVC::ToHz(*sfc) : std::nullopt;
                if (!format || format->plugId != (self->input_ ? self->inputPlug_ : self->outputPlug_) || !rate) {
                    self->Finish(kIOReturnNotReady); return;
                }
                if (self->input_) {
                    self->observation_.inputRateHz = *rate;
                    self->Finish(kIOReturnSuccess);
                } else {
                    self->observation_.outputRateHz = *rate;
                    self->input_ = true;
                    self->attempt_ = 0;
                    self->Ask();
                }
            });
    }
    std::shared_ptr<AVC::IAvcUnit> unit_;
    Discovery::DeviceRouteToken route_;
    uint8_t outputPlug_, inputPlug_;
    bool input_{false};
    uint8_t attempt_{0};
    AvcDuplexClockObservation observation_{};
    std::shared_ptr<std::atomic<bool>> active_;
    Completion completion_;
};

} // namespace ASFW::Audio
