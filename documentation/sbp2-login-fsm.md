# SBP-2 login state machine

`LoginSession` drives the management plane (login, reconnect, logout) through
`LoginFsm` in `ASFWDriver/Protocols/SBP2/Session/LoginStateMachine.hpp`. Each
state is a type that carries only the data that is valid in it, and one pure
function, `Transition(state, event, now)`, returns the next state plus the
effects (`Effects`) the session carries out: bus writes, timers, binding the
command plane, callbacks. `now` is the current bus generation and the target's
node from the last `Configure()`.

A login ID exists only in the states where the target holds our login. Whether
a reset is followed by a reconnect or a fresh login is decided by the state's
type, not by a sentinel value. SBP-2 reserves no login_ID, and the Hasselblad
H4D assigns 0. Neither reference uses a sentinel for this: Apple decides by
`fLoginState` (`IOFireWireSBP2Login.cpp:2067`), Linux by its work function
(`sbp2.c:849`, `937`; its `INVALID_LOGIN_ID` at `sbp2.c:184` only gates the
logout in remove).

## States

| State | Data | Reported as |
|---|---|---|
| `Idle` | generation | Idle |
| `LoggingIn` | route, retries used | LoggingIn |
| `LoggedIn` | login, route | LoggedIn |
| `Reconnecting` | login, route | Reconnecting |
| `SuspendedHeld` | login, generation | Suspended |
| `SuspendedNoLogin` | generation | Suspended |
| `LoggingOut` | login, route | LoggingOut |
| `Failed` | generation | Failed |

`login` is the target's login response: login_ID, command block agent,
reconnect_hold. `route` is the generation and node the management writes go to.
`LoginState` stays the reported enum, so the registry, the HBA bridge and the
user client are unchanged.

## Transitions

Write completions carry the operation and the generation they were issued in;
one from an older generation, or for another operation, is dropped. Any pair
not listed is dropped.

Two timers run independently. The management timer covers login, reconnect
and logout timeouts and retries. The reconnect window (management timeout +
1 s) starts at a reset with a login held and restarts on every further reset,
as Apple's `startReconnectTimer` does from `suspendedNotify` (A:2067-2077,
A:2202-2235). A held login that has not reconnected when it closes is lost.

| From | Event | To | Effects | Reference |
|---|---|---|---|---|
| Idle, Failed | Login | LoggingIn(now) | write login, login timeout | Apple `submitLogin` A:1463-1533 |
| Failed | bus reset | Failed | release | |
| LoggingIn | write refused, resources unavailable | Failed | cancel timer | |
| LoggingIn | write ACKed | LoggingIn | login timeout | |
| LoggingIn | write failed, status error, unreadable response | LoggingIn(retries+1) | 1 s retry; Failed + report at 32 | Apple `completeLogin` A:1703-1710 |
| LoggingIn | login timeout | LoggingIn(now, retries+1) | write login; Failed + report at 32 | |
| LoggingIn | retry due | LoggingIn(now) | write login | |
| LoggingIn | accepted, physical DMA on | LoggedIn | bind, busy timeout, report up | Apple A:1965-2033; Linux `sbp2_login` L:833-886 |
| LoggingIn | accepted, physical DMA refused | SuspendedHeld | open window | Linux enables at login L:1146 |
| LoggingIn | bus reset | SuspendedNoLogin | cancel timer, unbind, release | |
| LoggedIn | bus reset | SuspendedHeld | cancel timer, unbind, release, open window | Apple `suspendedNotify` A:2067 |
| LoggedIn | Logout | LoggingOut(route) | unbind, write logout, timeout | Apple `executeLogout` A:2664-2682 |
| SuspendedHeld | Resume | Reconnecting(now) | write reconnect | Apple `resumeNotify` A:2121-2125, 2278; Linux `sbp2_reconnect` L:917-924 |
| SuspendedNoLogin | Resume, Login | LoggingIn(now) | write login | |
| SuspendedHeld | Logout | LoggingOut(now) | close window, unbind, write logout, timeout | see deviation 3 |
| SuspendedHeld | window closed | SuspendedNoLogin | report session lost | Apple `reconnectTimeout` A:2395-2413 |
| SuspendedNoLogin | Logout | Idle | report logout done, nothing on the wire | |
| Suspended* | device gone | Failed | close window, report session lost | Apple drops the login on terminate A:2158-2196 |
| SuspendedHeld | bus reset | SuspendedHeld | track generation, restart window | Apple A:2072-2077 |
| SuspendedNoLogin | bus reset | SuspendedNoLogin | track generation | |
| Reconnecting | resources unavailable | Failed | cancel timer | |
| Reconnecting | write refused or failed | Reconnecting | 1 s, then as a timeout | Apple A:2285 |
| Reconnecting | write ACKed | Reconnecting | (status due before the window closes) | Apple A:2349-2362 |
| Reconnecting | accepted, physical DMA on | LoggedIn | close window, bind, busy timeout, report up | Apple A:2458-2496; Linux L:944-954 |
| Reconnecting | accepted, physical DMA refused | SuspendedHeld | open window | Linux enables at reconnect L:1198 |
| Reconnecting | rejected: request complete, unspecified error | Reconnecting | resend after 100 ms | Apple A:2498-2503, 589 |
| Reconnecting | resend due | Reconnecting(now) | write reconnect | Apple `reconnectRetryTimeout` → `doReconnect` |
| Reconnecting | rejected otherwise, retry timeout, window closed | LoggingIn(now) | close window, report session lost, then write login | Apple ReconnectFailed A:2504-2519, 2395-2413 → client logs in T:794-806 |
| Reconnecting | bus reset | SuspendedHeld | cancel timer, unbind, release, restart window | Apple A:2072-2077 |
| LoggingOut | write refused, resources unavailable | Failed | cancel timer | |
| LoggingOut | write ACKed | LoggingOut | logout timeout | |
| LoggingOut | status, write failed, timeout | Idle | report logout done (0 / -1 / -2) | Apple A:1916-1944, 2730-2757 |
| LoggingOut | bus reset | Idle | cancel timer, unbind, release | see deviation 4 |

A = `references/IOFireWireSBP2/Source/IOFireWireSBP2/IOFireWireSBP2Login.cpp`,
T = `IOFireWireSerialBusProtocolTransport.cpp`,
L = `references/linux-ohci-firewire-low-level-stack/sbp2.c`.

## Threading

Timers, write completions, status blocks and bus resets arrive on the driver
work queue, and the HBA pumps its commands there too. The probe user client
calls `StartLogin`/`ReleaseSession` from its own queue (no `QUEUENAME`, no hop).
So every access to the state goes through `LoginSession::Serialized()`: a queue
of work run one piece at a time by whichever thread finds it idle. Its lock
covers only the queue; the work itself runs unlocked, so a callback may take
the registry's lock without an AB-BA against `SessionRegistry::OnBusReset`,
which calls in under that lock. Work raised while other work runs (an inline
write completion, a callback that calls back in, a call from another queue) is
queued behind it: run-to-completion. A request queued behind another thread's
work returns true before it has run.

Readers on other queues (the HBA through `SessionRegistry::GetSessionState`,
the user client at `SBP2Handler.hpp:227`, `SubmitORB`) use
`LoginSession::Snapshot()`: state, generation and login ID packed in one atomic
word, so a reader never sees the ID of one transition with the state of
another.

The user-client path still drives `CommandExecutor` and `FetchAgent` from its
own queue, as before this change; only the login state is serialized here.

## Behaviour changes in this refactor

- After a reset, a login with login_ID 0 reconnects instead of logging in again.
- A logout after a reset that interrupted the login completes without a write.
  It used to send a logout ORB with login_ID 0.
- A logout from Suspended allocates the management address spaces again. They
  were released by the reset, so the ORB pointer used to address nothing.
- Login is accepted only from Idle, Failed and SuspendedNoLogin. The registry
  already started logins only from Idle.
- A write that completes inline no longer has its retry timer overwritten by
  the timeout armed after it.
- A timer that fires after it was cancelled or re-armed is dropped.
- `Resume()` from LoggedIn is dropped; the old `Reconnect()` accepted it. There
  was no caller.
- The reported generation follows every reset: in Suspended, Failed, and in
  Idle after a reset during logout. It used to keep the first reset's value,
  so a logout from Suspended after two resets went out at a stale generation.
- The reported login ID is 0 outside the states that hold a login. It used to
  keep a stale ID, for example in Failed after a fallback login failed.
- `LoginCompleteParams` no longer carries the status block. Nothing read it.

## Open deviations from the references

Kept as they were; each changes bus behaviour and needs hardware to verify.

3. **Logout while suspended.** Apple defers it until the reconnect resolves
   (`fLogoutPending`, A:2639-2655). We send it at once, to the node of the last
   rediscovery.
4. **Bus reset while logging out.** Apple ignores it and waits for the logout
   to complete or time out (A:2088-2092). We go to Idle.
