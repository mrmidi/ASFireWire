# Contributing to ASFireWire

Hardware tests, reliable bug reports, documentation improvements, and focused code
changes are welcome. Testing hardware can be more useful than adding code. Testers
are credited alongside engineers in the compatibility wiki and release history.

## What helps the project

ASFireWire does not need more code simply for the sake of adding functionality.
The default priorities are to fix what is broken, improve what is insufficient,
simplify existing implementations, remove unnecessary complexity, and extend
hardware support where a clear need exists.

Before adding code, ask whether existing mechanisms or removing code can solve the
problem. Fewer lines help when they improve clarity without compromising correctness.
A new abstraction, subsystem, or feature must justify its maintenance cost.

## Discuss substantial changes first

Open a [GitHub issue](https://github.com/mrmidi/ASFireWire/issues) before significant
development. Explain the problem, why the current implementation is insufficient,
whether existing facilities can solve it, and which devices or protocols may be affected.
Small typo fixes and similarly narrow corrections do not need a design discussion.

Agreement to investigate a problem is not approval of a particular implementation.
Understand the expected value and maintenance burden before building a large solution.

## Make pull requests reviewable

Keep changes small, focused, and independently reviewable. Audio-engine changes,
UI changes, and MCP features normally belong in separate PRs. Do not mix unrelated
cleanup into a fix.

PRs with thousands of changed lines are unlikely to be reviewed without prior
agreement. Exceptions may be appropriate for critical fixes or substantial work
from trusted engineers or original hardware vendors. Reviewability, architectural
impact, and demonstrated correctness matter more than a line-count threshold.

Use the PR template to explain the problem, change, architectural fit, affected
subsystems, tests, and regression risks. State what could not be tested.

## Understand and take responsibility for your code

Be able to explain in your own words why code was added, changed, or removed; its
assumptions; why it fits the architecture; and which evidence supports correctness.
A copied explanation from an AI agent is not sufficient.

AI-assisted development is acceptable. Contributors must review generated code and
understand its consequences across the system. Shared infrastructure serves many
device families: a patch that fixes one device can break another. AI-generated code
receives no exemption from engineering, testing, or ownership requirements.

Take reasonable ownership: respond to review, revise the implementation, investigate
regressions, and follow through on problems introduced by the change. The maintainer
should not routinely need to rewrite a contribution to integrate it.

Ask yourself: **Does this meaningfully help the project, or create additional work
for its maintainer?**

## Respect the architecture and wire behavior

Start with [Architecture Overview](https://github.com/mrmidi/ASFireWire/wiki/Architecture-Overview)
and the repository's [engineering guidance](CLAUDE.md).

- FireWire transport moves fully framed packets opaquely. Keep audio semantics and
  CIP/IEC 61883/AM824 framing out of OHCI, DMA, async, isoch, and bus mechanism code.
- Audio owns format, clock, and Core Audio semantics. Cross the transport seam using
  neutral, lifetime-owned contracts; do not retain raw pointers into another service's memory.
- Quiesce dependent queues and release views before freeing buffers or detaching
  hardware. No MMIO is legal after hardware detach.
- Check relevant Linux, Apple, FFADO, or ALSA behavioral references before changing
  observable bus behavior. Cite the source and location. Missing references or an
  unresolved specification question need discussion, not a guessed implementation.
- References are read-only. Do not copy GPL, LGPL, or APSL implementation fragments
  into this Apache-2.0 project. Any proposed adaptation needs prior license review.
- Investigate history and reach suspect DMA/ring behavior with a test before removing
  it. Ring tests must exercise production allocation, wrap padding, empty/full states,
  and hot append across wrap. State transaction counts and approximate wraps for
  hardware validation; a short clean run is limited evidence.

`project.yml` is the build-settings source of truth. The generated Xcode project is
committed for Xcode Cloud; do not edit it manually. When adding/removing/renaming
sources or changing settings, update YAML, regenerate, and include the generated project.

## Provide testing and regression evidence

For shared-code changes, list hardware, revisions, firmware, Mac architecture,
macOS and ASFW builds, configurations, tested functionality, and untested cases.
Explain the shared behaviors at risk and how tests or reference checks constrain
regressions. One device's success is useful evidence, not proof across the system.

Use host-side tests for logic and fixtures. They cannot validate real DriverKit
lifecycle or controller behavior. Batch expensive hardware verification after the
solution is designed and software checks pass. See
[Building and Signing](https://github.com/mrmidi/ASFireWire/wiki/Building-and-Signing).

Hardware results and bugs belong in issues or the
[website report form](https://www.asfw.net/test-device/), not code PRs by default.
Follow [Reporting Issues](https://github.com/mrmidi/ASFireWire/wiki/Reporting-Issues)
and [Logging](https://github.com/mrmidi/ASFireWire/wiki/Logging). Attach logs and
relevant AV/C, DICE, or meaningful 1394 diagnostics; include crash reports for crashes.
If evidence is unavailable, explain why. Descriptions help, but are usually not enough
to diagnose a failure. Report physical playback/capture results, not just packet counters.

## Review and licensing

The maintainer may decline changes that duplicate functionality, introduce unjustified
complexity, violate architectural boundaries, are too large to review, lack suitable
testing or explanations, or create disproportionate maintenance work.

Contributions are accepted under [Apache License 2.0](LICENSE), the same terms as the
project (inbound = outbound, per section 5). Only submit material you have the right
to contribute, and preserve required attribution.
