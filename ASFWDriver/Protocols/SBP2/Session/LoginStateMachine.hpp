#pragma once

// LoginStateMachine — the SBP-2 management-plane states as data.
//
// Each state carries only what is true in it. A login ID exists only while the
// target holds our login, so "do we hold a login?" is the state's type, never a
// sentinel value: SBP-2 does not reserve any login_ID, and the H4D hands out 0.
// Neither reference uses one either (Apple decides by fLoginState,
// IOFireWireSBP2Login.cpp:2067; Linux by its work function, sbp2.c:849,937).
//
// Transition() is pure: LoginSession feeds it events and carries out the
// returned effects (bus writes, timers, callbacks). The table, and how it
// compares with Apple and Linux, is in documentation/sbp2-login-fsm.md.

#include "../SBP2WireFormats.hpp"

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace ASFW::Protocols::SBP2 {

enum class LoginState : uint8_t {
    Idle,
    LoggingIn,
    LoggedIn,
    Reconnecting,
    LoggingOut,
    Suspended,     // Lost after bus reset, waiting for rediscovery
    Failed
};

[[nodiscard]] inline constexpr const char* ToString(LoginState s) noexcept {
    switch (s) {
        case LoginState::Idle:         return "Idle";
        case LoginState::LoggingIn:    return "LoggingIn";
        case LoginState::LoggedIn:     return "LoggedIn";
        case LoginState::Reconnecting: return "Reconnecting";
        case LoginState::LoggingOut:   return "LoggingOut";
        case LoginState::Suspended:    return "Suspended";
        case LoginState::Failed:       return "Failed";
    }
    return "Unknown";
}

namespace LoginFsm {

enum class Op : uint8_t { Login, Reconnect, Logout };

struct Route {
    uint16_t generation{0};
    uint16_t node{0xFFFF};
};

// A login the target holds for us, from its login response.
struct HeldLogin {
    uint16_t id{0};
    uint16_t commandBlockAgentHi{0};
    uint32_t commandBlockAgentLo{0};
    uint16_t reconnectHold{0};
    Wire::LoginResponse response{};
};

namespace St {
struct Idle { uint16_t generation{0}; };
struct LoggingIn { Route route; uint32_t retries{0}; };
struct LoggedIn { HeldLogin login; Route route; };
struct Reconnecting { HeldLogin login; Route route; };
// Bus reset with a login held: reconnect once rediscovery supplies the route.
struct SuspendedHeld { HeldLogin login; uint16_t generation{0}; };
// Bus reset before the login completed: log in once rediscovery supplies the route.
struct SuspendedNoLogin { uint16_t generation{0}; };
struct LoggingOut { HeldLogin login; Route route; };
struct Failed { uint16_t generation{0}; };
} // namespace St

using State = std::variant<St::Idle, St::LoggingIn, St::LoggedIn, St::Reconnecting,
                           St::SuspendedHeld, St::SuspendedNoLogin, St::LoggingOut, St::Failed>;

namespace Ev {
struct Login {};
struct Logout {};
struct Resume {};                       // rediscovered; `now` is the fresh route
struct BusReset { uint16_t generation{0}; };
struct DeviceGone {};
struct ResourcesUnavailable {};
struct WriteRefused { Op op; };         // the bus refused the write synchronously
struct WriteDone { Op op; uint16_t generation{0}; bool ok{false}; };
struct Timeout { Op op; };
struct LoginRetryDue {};
struct ReconnectWindowClosed {};
struct LoginAccepted { HeldLogin login; bool physicalDmaOk{false}; };
struct LoginRejected {};                // error status, or the response is unreadable
struct ReconnectAccepted { bool physicalDmaOk{false}; };
struct ReconnectRejected {};
struct LogoutDone {};
} // namespace Ev

using Event = std::variant<Ev::Login, Ev::Logout, Ev::Resume, Ev::BusReset, Ev::DeviceGone,
                           Ev::ResourcesUnavailable, Ev::WriteRefused, Ev::WriteDone, Ev::Timeout,
                           Ev::LoginRetryDue, Ev::ReconnectWindowClosed, Ev::LoginAccepted,
                           Ev::LoginRejected,
                           Ev::ReconnectAccepted, Ev::ReconnectRejected, Ev::LogoutDone>;

enum class Timer : uint8_t {
    LoginTimeout,       // management timeout → Timeout{Login}
    LoginRetry,         // 1 s → LoginRetryDue
    ReconnectRetry,     // 1 s → Timeout{Reconnect}
    LogoutTimeout,      // management timeout → Timeout{Logout}
};

enum class Notify : uint8_t { None, LoggedIn, LoginFailed, SessionLost, LogoutDone };

// LoginCompleteParams.status for a lost session (reconnect rejected/timed out,
// device gone): terminal for the OLD login.
inline constexpr int kStatusSessionLost = -3;

// Carried out by LoginSession in this order: cancelTimer/cancelWindow, unbind,
// release, sessionLostFirst, (state committed), write, arm/armWindow, bind, notify.
//
// The reconnect window is a second timer, beside the management one: it runs
// from the bus reset (management timeout + 1 s → ReconnectWindowClosed) and a
// held login that has not reconnected when it closes is lost. Apple starts it
// at the reset and restarts it on every further reset
// (IOFireWireSBP2Login::suspendedNotify → startReconnectTimer, :2067-2077,
// :2202-2235).
struct Effects {
    bool cancelTimer{false};
    bool cancelWindow{false};
    bool unbind{false};               // detach the fetch agent
    bool release{false};              // free the management address spaces
    bool sessionLostFirst{false};     // down-edge for the old login before what follows
    std::optional<Op> write;
    std::optional<Timer> arm;
    bool armWindow{false};
    std::optional<Op> bind;           // bind the command plane after a login or reconnect
    Notify notify{Notify::None};
    int status{0};
    uint16_t generation{0};
};

struct Step {
    State next;
    Effects fx;
};

inline constexpr uint32_t kLoginRetryMax = 32;  // Apple default (IOFireWireSBP2Login.cpp:142)

[[nodiscard]] inline LoginState Reported(const State& s) noexcept {
    return std::visit([](const auto& st) -> LoginState {
        using T = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<T, St::Idle>) return LoginState::Idle;
        else if constexpr (std::is_same_v<T, St::LoggingIn>) return LoginState::LoggingIn;
        else if constexpr (std::is_same_v<T, St::LoggedIn>) return LoginState::LoggedIn;
        else if constexpr (std::is_same_v<T, St::Reconnecting>) return LoginState::Reconnecting;
        else if constexpr (std::is_same_v<T, St::LoggingOut>) return LoginState::LoggingOut;
        else if constexpr (std::is_same_v<T, St::Failed>) return LoginState::Failed;
        else return LoginState::Suspended;
    }, s);
}

[[nodiscard]] inline const char* Name(const State& s) noexcept {
    if (std::holds_alternative<St::SuspendedHeld>(s)) return "SuspendedHeld";
    if (std::holds_alternative<St::SuspendedNoLogin>(s)) return "SuspendedNoLogin";
    return ToString(Reported(s));
}

[[nodiscard]] inline const HeldLogin* Held(const State& s) noexcept {
    return std::visit([](const auto& st) -> const HeldLogin* {
        if constexpr (requires { st.login; }) return &st.login;
        else return nullptr;
    }, s);
}

[[nodiscard]] inline std::optional<Route> RouteOf(const State& s) noexcept {
    return std::visit([](const auto& st) -> std::optional<Route> {
        if constexpr (requires { st.route; }) return st.route;
        else return std::nullopt;
    }, s);
}

[[nodiscard]] inline uint16_t GenerationOf(const State& s) noexcept {
    return std::visit([](const auto& st) -> uint16_t {
        if constexpr (requires { st.route; }) return st.route.generation;
        else return st.generation;
    }, s);
}

namespace detail {

inline Step StartLogin(const Route& now, uint32_t retries) {
    return {St::LoggingIn{now, retries}, {.write = Op::Login, .arm = Timer::LoginTimeout}};
}

inline Step FailLogin(const Route& route, int status) {
    return {St::Failed{route.generation},
            {.cancelTimer = true, .notify = Notify::LoginFailed, .status = status,
             .generation = route.generation}};
}

// A failed attempt backs off 1 s and logs in again, up to kLoginRetryMax
// (Apple: completeLogin arms a 1 s retry, IOFireWireSBP2Login.cpp:1703-1710).
inline Step RetryLogin(const St::LoggingIn& s) {
    if (s.retries >= kLoginRetryMax) {
        return FailLogin(s.route, -1);
    }
    return {St::LoggingIn{s.route, s.retries + 1}, {.arm = Timer::LoginRetry}};
}

// The target dropped the old login: report it lost, then log in afresh
// (Apple: ReconnectFailed to the client, which logs in again,
// IOFireWireSBP2Login.cpp:2565-2583, IOFireWireSerialBusProtocolTransport.cpp:794-806).
inline Step FallBackToLogin(const St::Reconnecting& s, const Route& now) {
    Step step = StartLogin(now, 0);
    step.fx.cancelTimer = true;
    step.fx.cancelWindow = true;
    step.fx.sessionLostFirst = true;
    step.fx.generation = s.route.generation;
    return step;
}

inline Step Bound(const HeldLogin& login, const Route& route, Op op) {
    return {St::LoggedIn{login, route},
            {.cancelTimer = true, .cancelWindow = true, .bind = op, .notify = Notify::LoggedIn,
             .generation = route.generation}};
}

inline Step Suspend(State next) {
    return {std::move(next), {.cancelTimer = true, .unbind = true, .release = true}};
}

// A reset with a login held (re)starts the reconnect window.
inline Step SuspendHeld(const HeldLogin& login, uint16_t generation) {
    Step step = Suspend(St::SuspendedHeld{login, generation});
    step.fx.armWindow = true;
    return step;
}

// Physical DMA was refused: a reset is in flight. Hold the login meanwhile.
inline Step HoldWithoutDma(const HeldLogin& login, uint16_t generation) {
    return {St::SuspendedHeld{login, generation}, {.cancelTimer = true, .armWindow = true}};
}

inline Step Lost(uint16_t generation) {
    return {St::Failed{generation},
            {.cancelTimer = true, .cancelWindow = true, .notify = Notify::SessionLost,
             .status = kStatusSessionLost, .generation = generation}};
}

inline Step LoggedOut(const Route& route, int status) {
    return {St::Idle{route.generation},
            {.cancelTimer = true, .notify = Notify::LogoutDone, .status = status,
             .generation = route.generation}};
}

inline bool Current(const Ev::WriteDone& e, Op op, const Route& route) {
    return e.op == op && e.generation == route.generation;
}

template <class... F> struct Overload : F... { using F::operator()...; };

} // namespace detail

// nullopt: the event means nothing in this state (stale or out of order) and is dropped.
[[nodiscard]] inline std::optional<Step> Transition(const State& state, const Event& event,
                                                    const Route& now) {
    using namespace detail;
    using R = std::optional<Step>;

    return std::visit(Overload{
        // Idle / Failed ------------------------------------------------------
        [&](const St::Idle&, const Ev::Login&) -> R { return StartLogin(now, 0); },
        [&](const St::Failed&, const Ev::Login&) -> R { return StartLogin(now, 0); },
        [&](const St::Failed&, const Ev::BusReset& e) -> R {
            return Step{St::Failed{e.generation}, {.release = true}};
        },

        // LoggingIn ----------------------------------------------------------
        [&](const St::LoggingIn& s, const Ev::ResourcesUnavailable&) -> R {
            return Step{St::Failed{s.route.generation}, {.cancelTimer = true}};
        },
        [&](const St::LoggingIn& s, const Ev::WriteRefused& e) -> R {
            if (e.op != Op::Login) return std::nullopt;
            return Step{St::Failed{s.route.generation}, {.cancelTimer = true}};
        },
        [&](const St::LoggingIn& s, const Ev::WriteDone& e) -> R {
            if (!Current(e, Op::Login, s.route)) return std::nullopt;
            if (e.ok) return Step{s, {.arm = Timer::LoginTimeout}};
            return RetryLogin(s);
        },
        [&](const St::LoggingIn& s, const Ev::Timeout& e) -> R {
            if (e.op != Op::Login) return std::nullopt;
            if (s.retries >= kLoginRetryMax) return FailLogin(s.route, -2);
            return StartLogin(now, s.retries + 1);
        },
        [&](const St::LoggingIn& s, const Ev::LoginRetryDue&) -> R {
            return StartLogin(now, s.retries);
        },
        [&](const St::LoggingIn& s, const Ev::LoginAccepted& e) -> R {
            if (!e.physicalDmaOk) return HoldWithoutDma(e.login, s.route.generation);
            return Bound(e.login, s.route, Op::Login);
        },
        [&](const St::LoggingIn& s, const Ev::LoginRejected&) -> R { return RetryLogin(s); },
        [&](const St::LoggingIn&, const Ev::BusReset& e) -> R {
            return Suspend(St::SuspendedNoLogin{e.generation});
        },

        // LoggedIn -----------------------------------------------------------
        [&](const St::LoggedIn& s, const Ev::BusReset& e) -> R {
            return SuspendHeld(s.login, e.generation);
        },
        [&](const St::LoggedIn& s, const Ev::Logout&) -> R {
            return Step{St::LoggingOut{s.login, s.route},
                        {.unbind = true, .write = Op::Logout, .arm = Timer::LogoutTimeout}};
        },

        // Reconnecting -------------------------------------------------------
        [&](const St::Reconnecting& s, const Ev::ResourcesUnavailable&) -> R {
            return Step{St::Failed{s.route.generation}, {.cancelTimer = true}};
        },
        // Apple reports a failed reconnect write as a lost session
        // (IOFireWireSBP2Login::doReconnect, IOFireWireSBP2Login.cpp:2285).
        [&](const St::Reconnecting& s, const Ev::WriteRefused& e) -> R {
            if (e.op != Op::Reconnect) return std::nullopt;
            return Step{s, {.arm = Timer::ReconnectRetry}};
        },
        // ACKed: the status is due before the reconnect window closes.
        [&](const St::Reconnecting& s, const Ev::WriteDone& e) -> R {
            if (!Current(e, Op::Reconnect, s.route)) return std::nullopt;
            if (e.ok) return Step{s, {}};
            return Step{s, {.arm = Timer::ReconnectRetry}};
        },
        [&](const St::Reconnecting& s, const Ev::ReconnectWindowClosed&) -> R {
            return FallBackToLogin(s, now);
        },
        [&](const St::Reconnecting& s, const Ev::Timeout& e) -> R {
            if (e.op != Op::Reconnect) return std::nullopt;
            return FallBackToLogin(s, now);
        },
        [&](const St::Reconnecting& s, const Ev::ReconnectAccepted& e) -> R {
            if (!e.physicalDmaOk) return HoldWithoutDma(s.login, s.route.generation);
            return Bound(s.login, s.route, Op::Reconnect);
        },
        [&](const St::Reconnecting& s, const Ev::ReconnectRejected&) -> R {
            return FallBackToLogin(s, now);
        },
        [&](const St::Reconnecting& s, const Ev::BusReset& e) -> R {
            return SuspendHeld(s.login, e.generation);
        },

        // SuspendedHeld ------------------------------------------------------
        [&](const St::SuspendedHeld& s, const Ev::Resume&) -> R {
            return Step{St::Reconnecting{s.login, now}, {.write = Op::Reconnect}};
        },
        [&](const St::SuspendedHeld& s, const Ev::Logout&) -> R {
            return Step{St::LoggingOut{s.login, now},
                        {.cancelWindow = true, .unbind = true, .write = Op::Logout,
                         .arm = Timer::LogoutTimeout}};
        },
        [&](const St::SuspendedHeld& s, const Ev::DeviceGone&) -> R { return Lost(s.generation); },
        [&](const St::SuspendedHeld& s, const Ev::BusReset& e) -> R {
            return Step{St::SuspendedHeld{s.login, e.generation}, {.armWindow = true}};
        },
        // Not rediscovered in time: the target has dropped the login. Report
        // it lost; rediscovery then logs in afresh (Apple: reconnectTimeout →
        // ReconnectFailed, :2395-2413, and the client logs in again).
        [&](const St::SuspendedHeld& s, const Ev::ReconnectWindowClosed&) -> R {
            return Step{St::SuspendedNoLogin{s.generation},
                        {.notify = Notify::SessionLost, .status = kStatusSessionLost,
                         .generation = s.generation}};
        },

        // SuspendedNoLogin ---------------------------------------------------
        [&](const St::SuspendedNoLogin&, const Ev::Resume&) -> R { return StartLogin(now, 0); },
        [&](const St::SuspendedNoLogin&, const Ev::Login&) -> R { return StartLogin(now, 0); },
        // Nothing to log out of: the login never completed.
        [&](const St::SuspendedNoLogin& s, const Ev::Logout&) -> R {
            return LoggedOut(Route{s.generation, now.node}, 0);
        },
        [&](const St::SuspendedNoLogin& s, const Ev::DeviceGone&) -> R { return Lost(s.generation); },
        [&](const St::SuspendedNoLogin&, const Ev::BusReset& e) -> R {
            return Step{St::SuspendedNoLogin{e.generation}, {}};
        },

        // LoggingOut ---------------------------------------------------------
        [&](const St::LoggingOut& s, const Ev::ResourcesUnavailable&) -> R {
            return Step{St::Failed{s.route.generation}, {.cancelTimer = true}};
        },
        [&](const St::LoggingOut& s, const Ev::WriteRefused& e) -> R {
            if (e.op != Op::Logout) return std::nullopt;
            return Step{St::Failed{s.route.generation}, {.cancelTimer = true}};
        },
        [&](const St::LoggingOut& s, const Ev::WriteDone& e) -> R {
            if (!Current(e, Op::Logout, s.route)) return std::nullopt;
            if (e.ok) return Step{s, {.arm = Timer::LogoutTimeout}};
            return LoggedOut(s.route, -1);
        },
        [&](const St::LoggingOut& s, const Ev::Timeout& e) -> R {
            if (e.op != Op::Logout) return std::nullopt;
            return LoggedOut(s.route, -2);
        },
        [&](const St::LoggingOut& s, const Ev::LogoutDone&) -> R { return LoggedOut(s.route, 0); },
        [&](const St::LoggingOut&, const Ev::BusReset& e) -> R {
            return Suspend(St::Idle{e.generation});
        },

        [](const auto&, const auto&) -> R { return std::nullopt; },
    }, state, event);
}

} // namespace LoginFsm
} // namespace ASFW::Protocols::SBP2
