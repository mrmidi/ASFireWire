// LoginStateMachine — one test per transition in documentation/sbp2-login-fsm.md.
// Transition() is pure, so these need no bus, scheduler or address space.

#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/SBP2/Session/LoginStateMachine.hpp"

namespace {

using namespace ASFW::Protocols::SBP2;
using namespace ASFW::Protocols::SBP2::LoginFsm;

constexpr Route kOld{1, 0xFFC2};
constexpr Route kNow{2, 0xFFC3};

HeldLogin MakeLogin(uint16_t id) {
    HeldLogin login{};
    login.id = id;
    login.commandBlockAgentHi = 0xFFFF;
    login.commandBlockAgentLo = 0xF0010000;
    return login;
}

Step Expect(const State& state, const Event& event, const Route& now = kNow) {
    auto step = Transition(state, event, now);
    EXPECT_TRUE(step.has_value());
    return step.value_or(Step{state, {}});
}

void ExpectIgnored(const State& state, const Event& event) {
    EXPECT_FALSE(Transition(state, event, kNow).has_value());
}

template <class S> const S& As(const Step& step) {
    EXPECT_TRUE(std::holds_alternative<S>(step.next));
    static const S fallback{};
    const auto* s = std::get_if<S>(&step.next);
    return s ? *s : fallback;
}

void ExpectRoute(const Route& actual, const Route& expected) {
    EXPECT_EQ(expected.generation, actual.generation);
    EXPECT_EQ(expected.node, actual.node);
}

// Idle / Failed --------------------------------------------------------------

TEST(LoginStateMachineTests, IdleLoginStartsLoginAtCurrentRoute) {
    const Step step = Expect(St::Idle{}, Ev::Login{});
    ExpectRoute(As<St::LoggingIn>(step).route, kNow);
    EXPECT_EQ(Op::Login, step.fx.write);
    EXPECT_EQ(Timer::LoginTimeout, step.fx.arm);
}

TEST(LoginStateMachineTests, FailedLoginStartsWithFreshRetryBudget) {
    const Step step = Expect(St::Failed{}, Ev::Login{});
    EXPECT_EQ(0u, As<St::LoggingIn>(step).retries);
    EXPECT_EQ(Op::Login, step.fx.write);
}

TEST(LoginStateMachineTests, FailedBusResetReleasesResources) {
    const Step step = Expect(St::Failed{1}, Ev::BusReset{2});
    EXPECT_EQ(2u, As<St::Failed>(step).generation);
    EXPECT_TRUE(step.fx.release);
}

TEST(LoginStateMachineTests, IdleIgnoresEverythingButLogin) {
    ExpectIgnored(St::Idle{}, Ev::Logout{});
    ExpectIgnored(St::Idle{}, Ev::Resume{});
    ExpectIgnored(St::Idle{}, Ev::BusReset{2});
    ExpectIgnored(St::Idle{}, Ev::DeviceGone{});
}

// LoggingIn ------------------------------------------------------------------

TEST(LoginStateMachineTests, LoggingInResourcesUnavailableFails) {
    const Step step = Expect(St::LoggingIn{kOld}, Ev::ResourcesUnavailable{});
    As<St::Failed>(step);
    EXPECT_EQ(Notify::None, step.fx.notify);
}

TEST(LoginStateMachineTests, LoggingInRefusedWriteFails) {
    const Step step = Expect(St::LoggingIn{kOld}, Ev::WriteRefused{Op::Login});
    As<St::Failed>(step);
    EXPECT_TRUE(step.fx.cancelTimer);
}

TEST(LoginStateMachineTests, LoggingInAckedWriteWaitsForStatus) {
    const Step step = Expect(St::LoggingIn{kOld}, Ev::WriteDone{Op::Login, kOld.generation, true});
    ExpectRoute(As<St::LoggingIn>(step).route, kOld);
    EXPECT_EQ(Timer::LoginTimeout, step.fx.arm);
    EXPECT_FALSE(step.fx.write.has_value());
}

TEST(LoginStateMachineTests, LoggingInFailedWriteBacksOff) {
    const Step step = Expect(St::LoggingIn{kOld, 3}, Ev::WriteDone{Op::Login, kOld.generation, false});
    EXPECT_EQ(4u, As<St::LoggingIn>(step).retries);
    EXPECT_EQ(Timer::LoginRetry, step.fx.arm);
}

TEST(LoginStateMachineTests, LoggingInFailedWriteWithNoRetriesLeftFails) {
    const Step step = Expect(St::LoggingIn{kOld, kLoginRetryMax},
                             Ev::WriteDone{Op::Login, kOld.generation, false});
    As<St::Failed>(step);
    EXPECT_EQ(Notify::LoginFailed, step.fx.notify);
    EXPECT_EQ(-1, step.fx.status);
}

TEST(LoginStateMachineTests, LoggingInDropsStaleOrForeignWriteCompletions) {
    ExpectIgnored(St::LoggingIn{kOld}, Ev::WriteDone{Op::Login, 7, false});
    ExpectIgnored(St::LoggingIn{kOld}, Ev::WriteDone{Op::Logout, kOld.generation, false});
    ExpectIgnored(St::LoggingIn{kOld}, Ev::Timeout{Op::Reconnect});
}

TEST(LoginStateMachineTests, LoggingInTimeoutLogsInAgainAtCurrentRoute) {
    const Step step = Expect(St::LoggingIn{kOld, 2}, Ev::Timeout{Op::Login});
    const auto& next = As<St::LoggingIn>(step);
    ExpectRoute(next.route, kNow);
    EXPECT_EQ(3u, next.retries);
    EXPECT_EQ(Op::Login, step.fx.write);
}

TEST(LoginStateMachineTests, LoggingInTimeoutWithNoRetriesLeftFails) {
    const Step step = Expect(St::LoggingIn{kOld, kLoginRetryMax}, Ev::Timeout{Op::Login});
    As<St::Failed>(step);
    EXPECT_EQ(-2, step.fx.status);
}

TEST(LoginStateMachineTests, LoggingInRetryDueLogsInAtCurrentRoute) {
    const Step step = Expect(St::LoggingIn{kOld, 4}, Ev::LoginRetryDue{});
    const auto& next = As<St::LoggingIn>(step);
    ExpectRoute(next.route, kNow);
    EXPECT_EQ(4u, next.retries);
    EXPECT_EQ(Op::Login, step.fx.write);
}

TEST(LoginStateMachineTests, LoggingInAcceptedBindsAndReportsUp) {
    const Step step = Expect(St::LoggingIn{kOld}, Ev::LoginAccepted{MakeLogin(0x42), true});
    const auto& next = As<St::LoggedIn>(step);
    EXPECT_EQ(0x42u, next.login.id);
    ExpectRoute(next.route, kOld);
    EXPECT_EQ(Op::Login, step.fx.bind);
    EXPECT_EQ(Notify::LoggedIn, step.fx.notify);
    EXPECT_EQ(0, step.fx.status);
    EXPECT_TRUE(step.fx.cancelWindow);
}

TEST(LoginStateMachineTests, LoggingInAcceptedWithoutPhysicalDmaHoldsLoginSuspended) {
    const Step step = Expect(St::LoggingIn{kOld}, Ev::LoginAccepted{MakeLogin(0x42), false});
    EXPECT_EQ(0x42u, As<St::SuspendedHeld>(step).login.id);
    EXPECT_EQ(Notify::None, step.fx.notify);
}

TEST(LoginStateMachineTests, LoggingInRejectedBacksOffThenFails) {
    EXPECT_EQ(Timer::LoginRetry, Expect(St::LoggingIn{kOld}, Ev::LoginRejected{}).fx.arm);
    const Step last = Expect(St::LoggingIn{kOld, kLoginRetryMax}, Ev::LoginRejected{});
    As<St::Failed>(last);
    EXPECT_EQ(-1, last.fx.status);
}

TEST(LoginStateMachineTests, LoggingInBusResetSuspendsWithoutLogin) {
    const Step step = Expect(St::LoggingIn{kOld, 5}, Ev::BusReset{2});
    EXPECT_EQ(2u, As<St::SuspendedNoLogin>(step).generation);
    EXPECT_TRUE(step.fx.release);
    EXPECT_FALSE(step.fx.write.has_value());
}

TEST(LoginStateMachineTests, LoggingInIgnoresLogout) {
    ExpectIgnored(St::LoggingIn{kOld}, Ev::Logout{});
}

// LoggedIn -------------------------------------------------------------------

TEST(LoginStateMachineTests, LoggedInBusResetHoldsLogin) {
    const Step step = Expect(St::LoggedIn{MakeLogin(0x42), kOld}, Ev::BusReset{2});
    const auto& next = As<St::SuspendedHeld>(step);
    EXPECT_EQ(0x42u, next.login.id);
    EXPECT_EQ(2u, next.generation);
    EXPECT_TRUE(step.fx.unbind);
    EXPECT_TRUE(step.fx.release);
    EXPECT_TRUE(step.fx.armWindow);
}

TEST(LoginStateMachineTests, LoggedInLogoutWritesLogout) {
    const Step step = Expect(St::LoggedIn{MakeLogin(0x42), kOld}, Ev::Logout{});
    ExpectRoute(As<St::LoggingOut>(step).route, kOld);
    EXPECT_TRUE(step.fx.unbind);
    EXPECT_EQ(Op::Logout, step.fx.write);
    EXPECT_EQ(Timer::LogoutTimeout, step.fx.arm);
}

TEST(LoginStateMachineTests, LoggedInIgnoresLoginAndResume) {
    ExpectIgnored(St::LoggedIn{MakeLogin(1), kOld}, Ev::Login{});
    ExpectIgnored(St::LoggedIn{MakeLogin(1), kOld}, Ev::Resume{});
}

// Reconnecting ---------------------------------------------------------------

TEST(LoginStateMachineTests, ReconnectingResourcesUnavailableFails) {
    As<St::Failed>(Expect(St::Reconnecting{MakeLogin(1), kNow}, Ev::ResourcesUnavailable{}));
}

TEST(LoginStateMachineTests, ReconnectingRefusedWriteRetriesThroughTimeout) {
    const Step step = Expect(St::Reconnecting{MakeLogin(1), kNow}, Ev::WriteRefused{Op::Reconnect});
    As<St::Reconnecting>(step);
    EXPECT_EQ(Timer::ReconnectRetry, step.fx.arm);
}

// The window from the reset bounds the wait for status; an ACK arms nothing.
TEST(LoginStateMachineTests, ReconnectingWriteCompletion) {
    const St::Reconnecting s{MakeLogin(1), kNow};
    const Step acked = Expect(s, Ev::WriteDone{Op::Reconnect, kNow.generation, true});
    EXPECT_FALSE(acked.fx.arm.has_value());
    EXPECT_FALSE(acked.fx.armWindow);
    EXPECT_EQ(Timer::ReconnectRetry,
              Expect(s, Ev::WriteDone{Op::Reconnect, kNow.generation, false}).fx.arm);
    ExpectIgnored(s, Ev::WriteDone{Op::Reconnect, kOld.generation, true});
}

TEST(LoginStateMachineTests, ReconnectingWindowClosedReportsLostThenLogsIn) {
    const Step step = Expect(St::Reconnecting{MakeLogin(1), kOld}, Ev::ReconnectWindowClosed{});
    ExpectRoute(As<St::LoggingIn>(step).route, kNow);
    EXPECT_TRUE(step.fx.sessionLostFirst);
    EXPECT_EQ(Op::Login, step.fx.write);
}

TEST(LoginStateMachineTests, ReconnectingTimeoutReportsLostThenLogsIn) {
    const Step step = Expect(St::Reconnecting{MakeLogin(1), kOld}, Ev::Timeout{Op::Reconnect});
    ExpectRoute(As<St::LoggingIn>(step).route, kNow);
    EXPECT_TRUE(step.fx.sessionLostFirst);
    EXPECT_EQ(kOld.generation, step.fx.generation);
    EXPECT_EQ(Op::Login, step.fx.write);
}

TEST(LoginStateMachineTests, ReconnectingAcceptedBindsAndReportsUp) {
    const Step step = Expect(St::Reconnecting{MakeLogin(0x42), kNow}, Ev::ReconnectAccepted{true});
    const auto& next = As<St::LoggedIn>(step);
    EXPECT_EQ(0x42u, next.login.id);
    ExpectRoute(next.route, kNow);
    EXPECT_EQ(Op::Reconnect, step.fx.bind);
    EXPECT_EQ(Notify::LoggedIn, step.fx.notify);
    EXPECT_TRUE(step.fx.cancelWindow);
}

TEST(LoginStateMachineTests, ReconnectingAcceptedWithoutPhysicalDmaStaysSuspended) {
    const Step step = Expect(St::Reconnecting{MakeLogin(0x42), kNow}, Ev::ReconnectAccepted{false});
    EXPECT_EQ(0x42u, As<St::SuspendedHeld>(step).login.id);
}

TEST(LoginStateMachineTests, ReconnectingRejectedReportsLostThenLogsIn) {
    const Step step = Expect(St::Reconnecting{MakeLogin(1), kOld}, Ev::ReconnectRejected{});
    As<St::LoggingIn>(step);
    EXPECT_TRUE(step.fx.sessionLostFirst);
    EXPECT_EQ(Op::Login, step.fx.write);
}

TEST(LoginStateMachineTests, ReconnectingUnspecifiedErrorResendsAfterABackoff) {
    const Step step = Expect(St::Reconnecting{MakeLogin(0x42), kOld}, Ev::ReconnectRejected{true});
    ExpectRoute(As<St::Reconnecting>(step).route, kOld);
    EXPECT_EQ(Timer::ReconnectResend, step.fx.arm);
    EXPECT_FALSE(step.fx.sessionLostFirst);
    EXPECT_FALSE(step.fx.cancelWindow);
}

TEST(LoginStateMachineTests, ReconnectingResendGoesToTheCurrentRoute) {
    const Step step = Expect(St::Reconnecting{MakeLogin(0x42), kOld}, Ev::ReconnectRetryDue{});
    const auto& next = As<St::Reconnecting>(step);
    EXPECT_EQ(0x42u, next.login.id);
    ExpectRoute(next.route, kNow);
    EXPECT_EQ(Op::Reconnect, step.fx.write);
}

TEST(LoginStateMachineTests, ReconnectingBusResetKeepsLoginHeld) {
    const Step step = Expect(St::Reconnecting{MakeLogin(0x42), kNow}, Ev::BusReset{3});
    EXPECT_EQ(0x42u, As<St::SuspendedHeld>(step).login.id);
    EXPECT_TRUE(step.fx.release);
    EXPECT_TRUE(step.fx.armWindow);
}

// SuspendedHeld --------------------------------------------------------------

// The H4D assigns login_ID 0; a held login reconnects whatever its ID.
TEST(LoginStateMachineTests, SuspendedHeldResumeReconnectsEvenWithLoginIdZero) {
    const Step step = Expect(St::SuspendedHeld{MakeLogin(0), 2}, Ev::Resume{});
    const auto& next = As<St::Reconnecting>(step);
    EXPECT_EQ(0u, next.login.id);
    ExpectRoute(next.route, kNow);
    EXPECT_EQ(Op::Reconnect, step.fx.write);
    EXPECT_FALSE(step.fx.armWindow);  // the window runs from the reset, not the write
}

TEST(LoginStateMachineTests, SuspendedHeldLogoutWritesLogoutAtCurrentRoute) {
    const Step step = Expect(St::SuspendedHeld{MakeLogin(0x42), 2}, Ev::Logout{});
    ExpectRoute(As<St::LoggingOut>(step).route, kNow);
    EXPECT_EQ(Op::Logout, step.fx.write);
}

TEST(LoginStateMachineTests, SuspendedHeldDeviceGoneReportsLost) {
    const Step step = Expect(St::SuspendedHeld{MakeLogin(1), 2}, Ev::DeviceGone{});
    As<St::Failed>(step);
    EXPECT_EQ(Notify::SessionLost, step.fx.notify);
    EXPECT_EQ(kStatusSessionLost, step.fx.status);
}

TEST(LoginStateMachineTests, SuspendedHeldBusResetRestartsTheWindow) {
    const Step step = Expect(St::SuspendedHeld{MakeLogin(0x42), 2}, Ev::BusReset{3});
    EXPECT_EQ(3u, As<St::SuspendedHeld>(step).generation);
    EXPECT_FALSE(step.fx.write.has_value());
    EXPECT_TRUE(step.fx.armWindow);
}

TEST(LoginStateMachineTests, SuspendedHeldWindowClosedReportsLostAndWaitsToLogIn) {
    const Step step = Expect(St::SuspendedHeld{MakeLogin(0x42), 2}, Ev::ReconnectWindowClosed{});
    EXPECT_EQ(2u, As<St::SuspendedNoLogin>(step).generation);
    EXPECT_EQ(Notify::SessionLost, step.fx.notify);
    EXPECT_EQ(kStatusSessionLost, step.fx.status);
    EXPECT_FALSE(step.fx.write.has_value());
}

TEST(LoginStateMachineTests, WindowClosingOutsideTheReconnectIsDropped) {
    ExpectIgnored(St::LoggedIn{MakeLogin(1), kNow}, Ev::ReconnectWindowClosed{});
    ExpectIgnored(St::SuspendedNoLogin{2}, Ev::ReconnectWindowClosed{});
    ExpectIgnored(St::LoggingIn{kNow}, Ev::ReconnectWindowClosed{});
}

TEST(LoginStateMachineTests, SuspendedHeldIgnoresLogin) {
    ExpectIgnored(St::SuspendedHeld{MakeLogin(1), 2}, Ev::Login{});
}

// SuspendedNoLogin -----------------------------------------------------------

TEST(LoginStateMachineTests, SuspendedNoLoginResumeLogsIn) {
    const Step step = Expect(St::SuspendedNoLogin{2}, Ev::Resume{});
    ExpectRoute(As<St::LoggingIn>(step).route, kNow);
    EXPECT_EQ(Op::Login, step.fx.write);
}

TEST(LoginStateMachineTests, SuspendedNoLoginLoginLogsIn) {
    EXPECT_EQ(Op::Login, Expect(St::SuspendedNoLogin{2}, Ev::Login{}).fx.write);
}

// No login was granted, so there is nothing to log out of on the wire.
TEST(LoginStateMachineTests, SuspendedNoLoginLogoutCompletesWithoutWrite) {
    const Step step = Expect(St::SuspendedNoLogin{2}, Ev::Logout{});
    As<St::Idle>(step);
    EXPECT_FALSE(step.fx.write.has_value());
    EXPECT_EQ(Notify::LogoutDone, step.fx.notify);
    EXPECT_EQ(0, step.fx.status);
}

TEST(LoginStateMachineTests, SuspendedNoLoginDeviceGoneReportsLost) {
    EXPECT_EQ(Notify::SessionLost, Expect(St::SuspendedNoLogin{2}, Ev::DeviceGone{}).fx.notify);
}

TEST(LoginStateMachineTests, SuspendedNoLoginBusResetTracksGeneration) {
    EXPECT_EQ(3u, As<St::SuspendedNoLogin>(Expect(St::SuspendedNoLogin{2}, Ev::BusReset{3})).generation);
}

// LoggingOut -----------------------------------------------------------------

TEST(LoginStateMachineTests, LoggingOutRefusedOrUnallocatedFails) {
    const St::LoggingOut s{MakeLogin(1), kOld};
    As<St::Failed>(Expect(s, Ev::WriteRefused{Op::Logout}));
    As<St::Failed>(Expect(s, Ev::ResourcesUnavailable{}));
}

TEST(LoginStateMachineTests, LoggingOutWriteCompletion) {
    const St::LoggingOut s{MakeLogin(1), kOld};
    EXPECT_EQ(Timer::LogoutTimeout, Expect(s, Ev::WriteDone{Op::Logout, kOld.generation, true}).fx.arm);
    const Step failed = Expect(s, Ev::WriteDone{Op::Logout, kOld.generation, false});
    As<St::Idle>(failed);
    EXPECT_EQ(-1, failed.fx.status);
    ExpectIgnored(s, Ev::WriteDone{Op::Logout, kNow.generation, true});
}

TEST(LoginStateMachineTests, LoggingOutEndsInIdle) {
    const St::LoggingOut s{MakeLogin(1), kOld};
    const Step timeout = Expect(s, Ev::Timeout{Op::Logout});
    As<St::Idle>(timeout);
    EXPECT_EQ(-2, timeout.fx.status);
    const Step done = Expect(s, Ev::LogoutDone{});
    EXPECT_EQ(Notify::LogoutDone, done.fx.notify);
    EXPECT_EQ(0, done.fx.status);
}

TEST(LoginStateMachineTests, LoggingOutBusResetDropsToIdle) {
    const Step step = Expect(St::LoggingOut{MakeLogin(1), kOld}, Ev::BusReset{2});
    As<St::Idle>(step);
    EXPECT_TRUE(step.fx.release);
    EXPECT_EQ(Notify::None, step.fx.notify);
}

// Reporting -------------------------------------------------------------------

TEST(LoginStateMachineTests, BothSuspendedStatesReportSuspended) {
    EXPECT_EQ(LoginState::Suspended, Reported(St::SuspendedHeld{}));
    EXPECT_EQ(LoginState::Suspended, Reported(St::SuspendedNoLogin{}));
    EXPECT_STREQ("SuspendedHeld", Name(St::SuspendedHeld{}));
    EXPECT_STREQ("SuspendedNoLogin", Name(St::SuspendedNoLogin{}));
}

TEST(LoginStateMachineTests, OnlyStatesWithALoginExposeOne) {
    EXPECT_EQ(nullptr, Held(St::Idle{}));
    EXPECT_EQ(nullptr, Held(St::LoggingIn{}));
    EXPECT_EQ(nullptr, Held(St::SuspendedNoLogin{}));
    ASSERT_NE(nullptr, Held(St::SuspendedHeld{MakeLogin(0), 1}));
    EXPECT_EQ(0u, Held(St::SuspendedHeld{MakeLogin(0), 1})->id);
}

} // namespace
