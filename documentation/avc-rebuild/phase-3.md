# Phase 3: Transaction engine (FCPTransport rewrite)

**Status 2026-10-02: DONE at `8d1b47b7`** on `feat/avc-transaction-engine` (stacked on `fix/fcp-wrresp-order`).
FCPTransport rewritten in place as a phase `std::variant` (Writing / AwaitingResponse / AwaitingRoute / Answered);
IAvcUnit::Submit is the only API; AvcError only; Apple timing (250 ms / 4 / 10 s); per-unit opcode policy.
2707/2707 host tests, goldens byte-identical, dext builds. **Not run on hardware.** Left for later: the legacy
`IAVCCommandSubmitter::SubmitCommand(AVCCdb)` / `AVCCommand` path (only the 1814 clock command uses it) belongs
to phase 2's caller migration.

Source: `00-overview.md` stage A2. User decision: rewrite it too.

- **References:**
  - Apple `references/IOFireWireAVC/IOFireWireAVCCommand.cpp` (timeouts, INTERIM, retries, write node/generation
    `:481-491`);
  - Linux `fcp.c` (`fcp_avc_transaction`; INTERIM deferral only for CONTROL `0x00` and NOTIFY `0x03`, `:243`).
- **Keep the proven behaviour of today's `FCPTransport`:**
  - route tokens, where a response matches only the attempt that was written;
  - no automatic replay of CONTROL;
  - STATUS retries only after the route is rebound;
  - the allowlist enforced at submit;
  - `FCPResponseRouter` contract.
- **New:**
  - `std::expected<Response, AvcError>` out; the `IOReturn` mapping in exactly one place;
  - the **per-unit stream-format opcode policy**: fixed by the catalog, or learned once from NOT IMPLEMENTED where
    the catalog allows (AVCVideoServices style), never a blind probe per call.
- **The bar:** `FCPTransportTests`, `FCPResponseRouterTests`, and the phase-2 goldens unchanged.
- Delete the old transport.

**Seam (2026-09-28):** the new engine is an implementation of the phase-2a `IAvcUnit`. Callers already go through
the seam after phase 2c, so swapping `FCPTransport` for the engine changes no caller.
