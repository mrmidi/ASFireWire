// SPDX-License-Identifier: Apache-2.0
#include "DescriptorAccessor.hpp"
#include "../../../Common/OnceCompletion.hpp"
#include "ParseReader.hpp"
#include <algorithm>
#include <numeric>
#include <variant>

namespace ASFW::Protocols::AVC {
namespace Avc = ASFW::AVC;
namespace Cmd = Avc::Cmd;
namespace {
struct Opening {};
struct Reading { size_t offset{}; size_t declaredTotal{}; size_t chunks{}; };
struct Closing { DescriptorAccessor::ReadDescriptorResult primary; };
struct Done {};
using Phase = std::variant<Opening, Reading, Closing, Done>;
static_assert(std::is_nothrow_move_constructible_v<Phase>);
struct SessionId { uint64_t value; friend bool operator==(SessionId, SessionId) = default; };
struct OperationSerial { uint64_t value; friend bool operator==(OperationSerial, OperationSerial) = default; };
struct ReplyIdentity {
    SessionId session;
    OperationSerial serial;
    Discovery::DeviceRouteToken route;
};
DescriptorAccessor::ReadDescriptorResult Cancelled() {
    DescriptorAccessor::ReadDescriptorResult result;
    result.avcResult = AVCResult::kBusReset;
    result.primaryError = Avc::AvcError::Of(Avc::AvcErrorKind::kBusReset);
    result.cancelled = true;
    return result;
}
AVCResult LegacyError(const Avc::AvcError& error) {
    if (error.response) return CTypeToResult(static_cast<uint8_t>(*error.response));
    switch (error.kind) {
        case Avc::AvcErrorKind::kTimeout: return AVCResult::kTimeout;
        case Avc::AvcErrorKind::kBusReset: return AVCResult::kBusReset;
        case Avc::AvcErrorKind::kTransportError: return AVCResult::kTransportError;
        case Avc::AvcErrorKind::kBusy: return AVCResult::kBusy;
        default: return AVCResult::kInvalidResponse;
    }
}
} // namespace

/// Each submitted frame's callback holds the operation (a lease), so an
/// operation whose accessor is gone still finishes its CLOSE. The operation
/// reaches its unit only through LiveRef; no callback retains a parser span.
class DescriptorReadOperation final : public std::enable_shared_from_this<DescriptorReadOperation> {
public:
    DescriptorReadOperation(Avc::IAvcUnit& unit, Avc::SubunitAddress address,
                            Cmd::DescriptorSpecifier specifier, SessionId id,
                            DescriptorAccessor::ReadCompletion completion)
        : unit_(unit), route_(unit.CurrentRoute()), address_(address), specifier_(specifier), id_(id),
          completion_([completion = std::move(completion)](DescriptorAccessor::ReadDescriptorResult result) mutable {
              completion(result);
          }, Cancelled()) {}
    [[nodiscard]] bool IsDone() const noexcept { return std::holds_alternative<Done>(phase_); }

    void Abort() {
        if (IsDone()) return;
        ++serial_.value;
        Finish(Cancelled());
    }
    void Cancel() {
        if (IsDone()) return;
        ++serial_.value; // No cancelled reply can resume reading.
        cancelled_ = true;
        if (!RouteCurrent()) { Finish(Cancelled()); return; }
        if (std::holds_alternative<Opening>(phase_) || std::holds_alternative<Closing>(phase_)) {
            // The already submitted OPEN/CLOSE may only finish cleanup. It
            // cannot create another READ after cancellation.
            return;
        }
        pending_ = false;
        phase_ = Closing{Cancelled()};
        Pump();
    }

    void Pump() {
        if (pumping_) return;
        auto keepAlive = shared_from_this();
        pumping_ = true;
        while (!pending_ && !IsDone()) {
            auto* unit = unit_.Get();
            if (!unit || !RouteCurrent()) { Finish(Cancelled()); break; }
            pending_ = true;
            const ReplyIdentity identity{id_, OperationSerial{++serial_.value}, *route_};
            inFlightSerial_ = identity.serial;
            if (auto* reading = std::get_if<Reading>(&phase_)) {
                if (++reading->chunks > DescriptorAccessor::kMaxChunks) {
                    pending_ = false;
                    FailRead(Avc::AvcError::Of(Avc::AvcErrorKind::kMalformedOperands));
                    continue;
                }
                const auto remaining = reading->declaredTotal ? reading->declaredTotal - reading->offset
                                                              : DescriptorAccessor::kMaxDescriptorBytes;
                const auto requested = static_cast<uint16_t>(std::min<size_t>(MAX_DESCRIPTOR_CHUNK_SIZE, remaining));
                Cmd::ReadDescriptorCommand command;
                command.address = address_;
                command.operands = {.specifier = specifier_, .offset = static_cast<uint16_t>(reading->offset),
                                    .length = requested};
                unit->Control(command, ASFW::FW::Generation{route_->generation},
                    [lease = shared_from_this(), identity, requested](Avc::Expected<Cmd::ReadDescriptorReply> reply) {
                        if (lease->Accept(identity)) lease->OnRead(reply, requested);
                    });
            } else {
                const bool opening = std::holds_alternative<Opening>(phase_);
                Cmd::OpenDescriptorCommand command;
                command.address = address_;
                command.operands.specifier = specifier_;
                command.operands.subfunction = opening ? Cmd::OpenDescriptorSubfunction::kReadOpen
                                                      : Cmd::OpenDescriptorSubfunction::kClose;
                unit->Control(command, ASFW::FW::Generation{route_->generation},
                    [lease = shared_from_this(), identity, opening](Avc::Expected<Cmd::OpenDescriptorReply> reply) {
                        if (lease->Accept(identity)) lease->OnOpenClose(reply, opening);
                    });
            }
        }
        pumping_ = false;
    }
private:
    [[nodiscard]] bool RouteCurrent() const {
        const auto* unit = unit_.Get();
        return unit && route_ && unit->IsCurrentRoute(*route_);
    }
    bool Accept(const ReplyIdentity& identity) {
        const bool cleanupReply = cancelled_ && identity.serial == inFlightSerial_ &&
            (std::holds_alternative<Opening>(phase_) || std::holds_alternative<Closing>(phase_));
        if (IsDone() || identity.session != id_ ||
            (identity.serial != serial_ && !cleanupReply) || identity.route != route_) return false;
        if (!RouteCurrent()) { Finish(Cancelled()); return false; }
        pending_ = false;
        return true;
    }
    void Finish(DescriptorAccessor::ReadDescriptorResult result) {
        if (IsDone()) return;
        phase_ = Done{};
        ++serial_.value;
        completion_.Invoke(std::move(result));
    }
    void FailRead(Avc::AvcError error) {
        DescriptorAccessor::ReadDescriptorResult result;
        result.avcResult = LegacyError(error);
        result.primaryError = error;
        phase_ = Closing{std::move(result)};
    }
    void OnOpenClose(Avc::Expected<Cmd::OpenDescriptorReply> reply, bool opening) {
        const auto expected = opening ? Cmd::OpenDescriptorSubfunction::kReadOpen : Cmd::OpenDescriptorSubfunction::kClose;
        if (reply && reply->subfunction != expected) reply = Avc::Fail(Avc::AvcErrorKind::kMalformedOperands);
        if (opening) {
            if (!reply) {
                // Nothing is open, so nothing to close. A cancelled request
                // reports the cancellation, not the OPEN answer.
                DescriptorAccessor::ReadDescriptorResult result = cancelled_ ? Cancelled() : DescriptorAccessor::ReadDescriptorResult{};
                if (!cancelled_) { result.avcResult = LegacyError(reply.error()); result.primaryError = reply.error(); }
                Finish(std::move(result));
            } else if (cancelled_) phase_ = Closing{Cancelled()};
            else phase_ = Reading{};
        } else {
            auto result = cancelled_ ? Cancelled() : std::move(std::get<Closing>(phase_).primary);
            if (!reply) result.cleanupError = reply.error();
            Finish(std::move(result));
        }
        Pump();
    }
    void OnRead(const Avc::Expected<Cmd::ReadDescriptorReply>& reply, uint16_t requested) {
        if (!reply) { FailRead(reply.error()); Pump(); return; }
        auto& reading = std::get<Reading>(phase_);
        const auto bytes = reply->data;
        if (bytes.empty() || bytes.size() > requested || reply->reportedOffset != reading.offset ||
            reply->reportedLength != bytes.size()) {
            FailRead(Avc::AvcError::Of(Avc::AvcErrorKind::kMalformedOperands)); Pump(); return;
        }
        if (reading.offset == 0) {
            if (bytes.size() < 2) { FailRead(Avc::AvcError::Of(Avc::AvcErrorKind::kOperandsTooShort)); Pump(); return; }
            // descriptor_length excludes itself (TA 2002013 §5). Saturating, so
            // no declared length can wrap into a small "complete" total.
            reading.declaredTotal = std::add_sat<size_t>(Descriptors::ParseReader(bytes).BE16().value_or(0), 2);
            if (reading.declaredTotal > DescriptorAccessor::kMaxDescriptorBytes) {
                FailRead(Avc::AvcError::Of(Avc::AvcErrorKind::kMalformedOperands)); Pump(); return;
            }
        }
        if (bytes.size() > reading.declaredTotal - reading.offset) {
            FailRead(Avc::AvcError::Of(Avc::AvcErrorKind::kMalformedOperands)); Pump(); return;
        }
        data_.insert(data_.end(), bytes.begin(), bytes.end()); // Own before callback returns.
        reading.offset = std::add_sat(reading.offset, bytes.size());
        // Apple MusicSubunitController.cpp:929-937 uses declared length even
        // when read_result_status is inaccurate. No additional read at EOF.
        if (reading.offset == reading.declaredTotal) {
            DescriptorAccessor::ReadDescriptorResult result;
            result.success = true; result.avcResult = AVCResult::kAccepted; result.data = std::move(data_);
            phase_ = Closing{std::move(result)};
        }
        Pump();
    }
    Common::LiveRef<Avc::IAvcUnit> unit_;
    std::optional<Discovery::DeviceRouteToken> route_;
    Avc::SubunitAddress address_;
    Cmd::DescriptorSpecifier specifier_;
    SessionId id_;
    OperationSerial serial_{0}, inFlightSerial_{0};
    Phase phase_{Opening{}};
    std::vector<uint8_t> data_;
    Common::OnceCompletion<DescriptorAccessor::ReadDescriptorResult> completion_;
    bool pending_{false}, pumping_{false}, cancelled_{false};
};

DescriptorAccessor::DescriptorAccessor(Avc::IAvcUnit& unit, uint8_t address)
    : DescriptorAccessor(unit, Avc::SubunitAddress::FromByte(address)) {}
DescriptorAccessor::DescriptorAccessor(Avc::IAvcUnit& unit, Avc::SubunitAddress address)
    : unit_(unit), address_(address) {}
DescriptorAccessor::~DescriptorAccessor() { Cancel(); }
void DescriptorAccessor::Abort() {
    auto operation = std::move(operation_);
    if (operation) operation->Abort();
}
void DescriptorAccessor::Cancel() {
    auto operation = std::move(operation_);
    if (operation) operation->Cancel();
}
void DescriptorAccessor::readUnitIdentifier(ReadCompletion completion) {
    readWithOpenCloseSequence(DescriptorSpecifier::forUnitIdentifier(), std::move(completion));
}
void DescriptorAccessor::readStatusDescriptor(uint8_t type, ReadCompletion completion) {
    readWithOpenCloseSequence(DescriptorSpecifier{.type = static_cast<DescriptorSpecifierType>(type),
                                                .typeSpecificFields = {}}, std::move(completion));
}
void DescriptorAccessor::readComplete(const DescriptorSpecifier& specifier, ReadCompletion completion) {
    readWithOpenCloseSequence(specifier, std::move(completion));
}
void DescriptorAccessor::readWithOpenCloseSequence(const DescriptorSpecifier& specifier, ReadCompletion completion) {
    const auto bytes = specifier.buildSpecifier();
    if (bytes.empty() || bytes.size() > Cmd::DescriptorSpecifier::kMaxBytes) {
        ReadDescriptorResult result; result.avcResult = AVCResult::kInvalidResponse;
        result.primaryError = Avc::AvcError::Of(Avc::AvcErrorKind::kInvalidArgument); completion(result); return;
    }
    readWithOpenCloseSequence(Cmd::DescriptorSpecifier::Raw(bytes), std::move(completion));
}
void DescriptorAccessor::readWithOpenCloseSequence(const Cmd::DescriptorSpecifier& specifier, ReadCompletion completion) {
    if (operation_ && !operation_->IsDone()) {
        ReadDescriptorResult result; result.avcResult = AVCResult::kBusy;
        result.primaryError = Avc::AvcError::Of(Avc::AvcErrorKind::kBusy); completion(result); return;
    }
    auto* unit = unit_.Get();
    if (!unit) { completion(Cancelled()); return; }
    if (specifier.length == 0 || specifier.length > Cmd::DescriptorSpecifier::kMaxBytes) {
        ReadDescriptorResult result; result.avcResult = AVCResult::kInvalidResponse;
        result.primaryError = Avc::AvcError::Of(Avc::AvcErrorKind::kInvalidArgument); completion(result); return;
    }
    static uint64_t nextSession = 0; // Driver serial queue; no concurrent access.
    auto operation = std::make_shared<DescriptorReadOperation>(*unit, address_,
        specifier, SessionId{++nextSession}, std::move(completion));
    operation_ = operation;
    operation->Pump();
}
} // namespace ASFW::Protocols::AVC
