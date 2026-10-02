// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DiscoveryReducer.hpp"
#include "../../../Common/OnceCompletion.hpp"
#include <deque>

namespace ASFW::AVC::DiscoveryEngine {
/// The serial-queue adapter is the only part of discovery which sees transport.
/// Its callbacks carry LiveRef; immutable results may outlive the session.
class Session final : public std::enable_shared_from_this<Session> {
public:
    /// Runs exactly once with the committed (or cancelled) snapshot.
    using Completion = Common::MoveOnlyCallback<void(SnapshotLease)>;
    using Extension = std::function<void(SnapshotLease, std::function<void(ExtensionFacts)>)>;
    static std::shared_ptr<Session> Create(IAvcUnit& unit, SessionId id, Completion completion,
                                           Extension extension = {});
    ~Session();
    Session(const Session&) = delete("a session owns its outstanding probe and its exactly-once completion");
    Session& operator=(const Session&) = delete("a session owns its outstanding probe and its exactly-once completion");
    [[nodiscard]] std::weak_ptr<const void> LifetimeToken() const noexcept { return lifetime_.Token(); }
    void Start();
    void Cancel();
    void RouteLost();
    [[nodiscard]] bool IsTerminal() const noexcept { return std::holds_alternative<Terminal>(state_.phase); }
private:
    Session(IAvcUnit& unit, SessionId id, Completion completion, Extension extension);
    void Deliver(Event event);
    void Execute(Action action);
    Common::LifetimeAnchor lifetime_;
    Common::LiveRef<IAvcUnit> unit_;
    SessionId id_;
    State state_;
    std::deque<Event> events_;
    bool pumping_{false};
    std::shared_ptr<ASFW::Protocols::AVC::DescriptorAccessor> descriptor_;
    Common::OnceCompletion<SnapshotLease> completion_;
    Extension extension_;
};
struct IdleSlot {};
struct RunningSlot { std::shared_ptr<Session> session; };
struct CancellingSlot { std::shared_ptr<Session> session; };
using SessionSlot = std::variant<IdleSlot, RunningSlot, CancellingSlot>;
static_assert(std::is_nothrow_move_constructible_v<SessionSlot>);
} // namespace ASFW::AVC::DiscoveryEngine
