// SPDX-License-Identifier: Apache-2.0
#include "DiscoverySession.hpp"
namespace ASFW::AVC::DiscoveryEngine {
Session::Session(IAvcUnit& unit, SessionId id, Completion completion, Extension extension)
    : unit_(unit), id_(id), completion_(std::move(completion), SnapshotLease{}), extension_(std::move(extension)) {}
std::shared_ptr<Session> Session::Create(IAvcUnit& unit, SessionId id, Completion completion, Extension extension) {
    return std::shared_ptr<Session>(new Session(unit, id, std::move(completion), std::move(extension)));
}
Session::~Session() { lifetime_.Invalidate(); if (descriptor_) descriptor_->Cancel(); }
void Session::Start() {
    auto* unit = unit_.Get();
    const auto route = unit ? unit->CurrentRoute() : std::nullopt;
    if (!route) { completion_.Invoke({}); return; }
    Deliver(DiscoveryEngine::Start{id_, *route, unit->Identity(), unit->GetStreamFormatOpcodePolicy(),
                                  unit->UsesStreamFormatSupportOpcode()});
}
void Session::Cancel() {
    const auto keepAlive = shared_from_this();
    Deliver(DiscoveryEngine::Cancel{}); // Invalidate identity before descriptor cancellation can call back.
    if (descriptor_) descriptor_->Cancel();
}
void Session::RouteLost() {
    const auto keepAlive = shared_from_this();
    Deliver(DiscoveryEngine::RouteLost{});
    if (descriptor_) descriptor_->Abort();
}
void Session::Deliver(Event event) {
    events_.push_back(std::move(event));
    if (pumping_) return;
    const auto keepAlive = shared_from_this();
    pumping_ = true;
    while (!events_.empty()) {
        auto next = std::move(events_.front()); events_.pop_front();
        auto transition = Step(std::move(state_), std::move(next));
        state_ = std::move(transition.state);
        for (auto& action : transition.actions) Execute(std::move(action));
    }
    pumping_ = false;
}
void Session::Execute(Action action) {
    const Common::LiveRef<Session> live{*this};
    std::visit([&](auto&& a) {
        using A = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<A, Commit>) {
            completion_.Invoke(std::move(a.snapshot));
        } else if constexpr (std::is_same_v<A, LearnSupportOpcode>) {
            if (auto* unit = unit_.Get()) unit->LearnStreamFormatSupportOpcode();
        } else {
            auto* unit = unit_.Get();
            if (!unit || !unit->IsCurrentRoute(a.operation.route)) {
                Deliver(DiscoveryEngine::RouteLost{}); return;
            }
            if constexpr (std::is_same_v<A, Send>) {
                // Submit is still the per-frame policy admission point. Never
                // bypass it, including replay and descriptor operations.
                unit->Submit(a.frame, a.operation.route.generation,
                    [live, identity = a.operation](Expected<Response> reply) {
                        auto* session = live.Get(); if (!session) return;
                        auto* owner = session->unit_.Get();
                        if (!owner || !owner->IsCurrentRoute(identity.route)) {
                            session->RouteLost(); return;
                        }
                        Expected<OwnedResponse> owned = reply ? Expected<OwnedResponse>{OwnedResponse{
                            reply->code, reply->address, reply->opcode,
                            {reply->operands.begin(), reply->operands.end()}}} :
                            Expected<OwnedResponse>{std::unexpected(reply.error())};
                        session->Deliver(Reply{identity, std::move(owned)});
                    });
            } else if constexpr (std::is_same_v<A, ReadDescriptor>) {
                const auto address = a.probe.subunit.type == SubunitType::kUnit ? SubunitAddress::Unit() : a.probe.subunit.ToAddress();
                descriptor_ = std::make_shared<ASFW::Protocols::AVC::DescriptorAccessor>(*unit, address);
                const auto accessor = descriptor_; // Immediate callbacks may replace the session's accessor.
                accessor->readWithOpenCloseSequence(a.probe.specifier,
                    [live, identity = a.operation](const auto& result) {
                        if (auto* session = live.Get()) session->Deliver(DescriptorReply{identity, result});
                    });
            } else if constexpr (std::is_same_v<A, RunExtension>) {
                const auto done = [live, identity = a.operation] {
                    if (auto* session = live.Get()) {
                        auto* owner = session->unit_.Get();
                        if (!owner || !owner->IsCurrentRoute(identity.route)) session->RouteLost();
                        else session->Deliver(ExtensionComplete{identity});
                    }
                };
                if (extension_) extension_(std::make_shared<const DiscoverySnapshot>(state_.builder), done); else done();
            }
        }
    }, std::move(action));
}
} // namespace ASFW::AVC::DiscoveryEngine
