# PHY Command & Alpha PHY Packet Contracts

This document defines the behavioral contracts for:

* `ASFW::Async::PhyCommand`
* `ASFW::Driver::AlphaPhyConfig`
* `ASFW::Driver::AlphaPhyConfigPacket`
* `ASFW::Driver::PhyGlobalResumePacket`

The goal is to make the PHY layer predictable, debuggable, and compatible with real-world (buggy) PHY silicon.

---

## 1. `ASFW::Async::PhyCommand` — Contract

### 1.1. Purpose

`PhyCommand` represents *local* IEEE-1394 PHY packets (tCode `0xE`) sent via the OHCI “async transmit” machinery for:

* GAP count configuration
* Root hold-off / force-root
* PHY global resume / power events
* Other link-local PHY control

It **does not** model standard async read/write/lock transactions and **never** expects an AR response for completion.

### 1.2. Construction

```cpp
class PhyCommand : public AsyncCommand<PhyCommand> {
public:
    PhyCommand(PhyParams params, CompletionCallback callback);
    ...
};
```

**Caller responsibilities:**

* `params` must fully describe the PHY payload **in logical form** (e.g. via `AlphaPhyConfigPacket` / `PhyGlobalResumePacket`) and must be valid for the current bus generation.
* `callback`:

  * Must be callable from driver context (IOWorkLoop thread).
  * Must be non-blocking and must not perform operations that can deadlock with the async subsystem (no re-entrant submission back into the same Tx path under the same gate, etc.).
* Lifetime:

  * The command object must remain alive until completion callback runs or the command is explicitly cancelled by the subsystem.

### 1.3. `BuildMetadata` Contract

```cpp
TxMetadata PhyCommand::BuildMetadata(const TransactionContext& txCtx);
```

**Inputs:**

* `txCtx.generation`
  Valid, current bus generation for this operation. Must be obtained from the controller’s `getGeneration()` during submit.
* `txCtx.sourceNodeID`
  Local node ID for this host controller (including bus ID bits).

**Outputs & guarantees:**

* `meta.generation = txCtx.generation`

  * Used by the controller to detect stale-generation PHY commands.
* `meta.sourceNodeID = txCtx.sourceNodeID`

  * Provides the link with the proper local ID for logging/debugging; actual PHY packet routing is link-local.
* `meta.destinationNodeID = 0xFFFF`

  * Sentinel “no real destination” — indicates this is link-local; the transmit engine **must not** attempt normal async routing semantics for this packet.
* `meta.tCode = 0xE`

  * Marks this as a PHY packet.
* `meta.expectedLength = 0`

  * Async receive side must not expect an AR packet for this transaction.
* `meta.completionStrategy = CompletionStrategy::CompleteOnAT`

  * **Contract:** the transaction is considered complete when an AT-level ack for this PHY packet is observed, *or* when it times out at the hardware level.
  * AR packets with `tCode==0xE` must be treated as **independent bus events**, not part of this transaction.

**Subsystem expectations:**

* The async engine must **not** park this transaction in an “AwaitingAR” state.
* Timeout/ack handling must mirror `IOFWAsyncPHYCommand::gotAck()` semantics:

  * `ack_complete` → success (`kIOReturnSuccess`)
  * anything else → mapped to timeout/error (`kIOReturnTimeout` or more specific mapping if you implement it)

### 1.4. `BuildHeader` Contract

```cpp
size_t PhyCommand::BuildHeader(
    uint8_t label,
    const PacketContext& pktCtx,
    PacketBuilder& builder,
    uint8_t* buffer);
```

**Inputs:**

* `label`
  Currently ignored. PHY packets don’t participate in the split-transaction label space, but the label may still be allocated for flow control at the FWIM/Tx layer.

* `pktCtx`
  Currently ignored. All routing/topology information is encoded in the PHY payload itself.

* `builder`
  Must provide:

  ```cpp
  size_t PacketBuilder::BuildPhyPacket(
      const PhyParams& params,
      std::uint8_t* buffer,
      size_t maxBytes);
  ```

* `buffer`
  Must point to at least 16 bytes of writable memory, 16-byte aligned as required by the DMA engine.

**Behavior:**

* Delegates to:

  ```cpp
  return builder.BuildPhyPacket(params_, buffer, 16);
  ```

**Guarantees:**

* The function returns the number of header bytes written.
* `BuildPhyPacket` must:

  * Encode exactly **two quadlets** (8 bytes payload × 2) representing the PHY packet (usually quadlet + its bitwise inverse).
  * Use **bus byte order** (big-endian as seen on the wire).
  * Not write beyond the supplied `maxBytes` (16) and must return a value in range `[8, 16]`. In practice you expect `16`.

**Subsystem expectations:**

* The Tx engine will only DMA `returned_length` bytes as header for this transaction.
* No payload descriptors will be chained after this header (see `PreparePayload`).

### 1.5. `PreparePayload` Contract

```cpp
std::unique_ptr<PayloadContext>
PhyCommand::PreparePayload(ASFW::Driver::HardwareInterface&) {
    return nullptr;
}
```

**Contract:**

* PHY packets **never** have a DMA payload.
* Returning `nullptr` signals to the Tx engine that:

  * No payload descriptors should be appended.
  * No additional DMA buffers are required.

**Subsystem requirements:**

* The Tx engine must be robust to `PreparePayload()` returning `nullptr` and treat the command as “header-only”.

---

## 2. `ASFW::Driver::AlphaPhyConfig` — Contract

`AlphaPhyConfig` is a **host-order logical view** of a PHY CONFIG quadlet for the alpha PHY (classic 1394 + 1394a extensions).

### 2.1. Field semantics

```cpp
struct AlphaPhyConfig {
    std::uint8_t rootId{0};            // Bits[29:24]
    bool        forceRoot{false};      // Bit[23]
    bool        gapCountOptimization{false};  // Bit[22] ("T" bit)
    std::uint8_t gapCount{0x3F};       // Bits[21:16], ignored if T==0
    ...
};
```

* `rootId`

  * 6-bit node ID of the **intended root** (0–63).
  * Caller must ensure this is a valid node on the bus when `forceRoot == true`.
* `forceRoot` (R bit)

  * When `true`, instructs PHYs to set `rootId` as the bus root.
* `gapCountOptimization` (T bit)

  * When `true`, GAP count update is requested and `gapCount` is meaningful.
  * When `false`, GAP count should be *ignored* by compliant PHYs, but see the workaround below.
* `gapCount`

  * 6-bit GAP count value (0–63).
  * Must be set according to 1394a bus latency and hop count if `gapCountOptimization == true`.

### 2.2. Encoding / decoding

#### 2.2.1. `EncodeHostOrder()`

```cpp
[[nodiscard]] constexpr Quadlet EncodeHostOrder() const noexcept;
```

**Contract:**

* Returns a 32-bit **host-order** value representing the PHY CONFIG quadlet.
* Responsibilities:

  * Writes packet identifier = `0` (PHY CONFIG) via `kPacketIdentifierMask`.
  * Encodes `rootId` → bits `[29:24]`.
  * Encodes `forceRoot` → bit `[23]` if true.
  * Encodes `gapCountOptimization` → bit `[22]` if true.
  * When `gapCountOptimization == true`:

    * Encodes `gapCount` → bits `[21:16]`.
  * When `gapCountOptimization == false`:

    * Forces `gapCount` field to **0x3F**, regardless of `gapCount` member value.

**Important workaround:**

> When `T=0` (no GAP update), we still encode `gapCount=0x3F` to protect against PHYs that erroneously latch GAP from bits `[21:16]` even when T=0. This avoids accidental GAP=0 on buggy hardware and matches behavior seen with Apple drivers and FireBug traces.

#### 2.2.2. `DecodeHostOrder(Quadlet)`

```cpp
static constexpr AlphaPhyConfig DecodeHostOrder(Quadlet quad) noexcept;
```

**Contract:**

* Interprets `quad` as **host-order** config quadlet and reconstructs the logical config fields.
* Caller must **convert from bus order first** using `FromBusOrder()` if the value was read directly from hardware.

#### 2.2.3. `IsConfigQuadletHostOrder(Quadlet)`

```cpp
static constexpr bool IsConfigQuadletHostOrder(Quadlet quad) noexcept;
```

**Contract:**

* Returns `true` if `quad` has packet identifier == `0` in **host-order** representation.
* Caller must not pass bus-order values here without converting with `FromBusOrder()` first.

### 2.3. Invariants

* `rootId` and `gapCount` are always treated as **6-bit values** (`& 0x3F`).
* Configuration quadlet always has packet identifier = `0` (PHY CONFIG).
* For `gapCountOptimization == false`, the encoded GAP field in the quadlet is guaranteed to be `0x3F`.

---

## 3. `ASFW::Driver::AlphaPhyConfigPacket` — Contract

This represents the **full 2-quadlet PHY CONFIG packet** (value + bitwise complement).

### 3.1. Structure

```cpp
struct AlphaPhyConfigPacket {
    AlphaPhyConfig header{};

    std::array<Quadlet, 2> EncodeHostOrder() const noexcept;
    static AlphaPhyConfigPacket DecodeHostOrder(std::array<Quadlet, 2> quadlets) noexcept;
    std::array<Quadlet, 2> EncodeBusOrder() const noexcept;
};
```

### 3.2. `EncodeHostOrder()`

**Contract:**

* Returns `{ first, second }` where:

  * `first = header.EncodeHostOrder()`
  * `second = ~first`
* Caller responsibilities:

  * Provide this pair to the `PacketBuilder` for PHY CONFIG packets.
  * Never modify the second element; complement relationship is required by spec and used by real PHYs to detect corruption.

### 3.3. `DecodeHostOrder(...)`

**Contract:**

* Accepts an array of **host-order** quadlets.
* Ignores the complement quadlet and decodes only `quadlets[0]` into `AlphaPhyConfig`.
* Caller must perform bus→host conversion before calling if the data originate from hardware.

### 3.4. `EncodeBusOrder()`

**Contract:**

* Produces `{ first_be, second_be }` where:

  * `first_be = ToBusOrder(first)`
  * `second_be = ToBusOrder(second)`
* Intended as the **final representation** to write into DMA buffers for transmission.

---

## 4. `ASFW::Driver::PhyGlobalResumePacket` — Contract

Represents PHY GLOBAL RESUME packets that share the same identifier space but use both R and T bits cleared and a specific pattern in the upper bits.

```cpp
struct PhyGlobalResumePacket {
    std::uint8_t phyId{0};

    std::array<Quadlet, 2> EncodeHostOrder() const noexcept;
    std::array<Quadlet, 2> EncodeBusOrder() const noexcept;
};
```

### 4.1. Semantics

* `phyId`

  * 6-bit ID of the PHY initiating the global resume.
  * Must match the local PHY ID when used as a command from the host.

### 4.2. `EncodeHostOrder()`

**Contract:**

* Constructs:

  ```cpp
  const Quadlet first =
      (static_cast<Quadlet>(phyId & 0x3Fu) << AlphaPhyConfig::kRootIdShift) |
      0x003C'0000u;
  ```

* Returns `{ first, ~first }` in **host order**.

Notes:

* `0x003C0000` encodes the “extended resume” semantics Apple uses (`0x003c0000 | (phyId << 24)` pattern from IOFireWireFamily tracing).
* This mirrors observed AppleFWOHCI behavior in FireBug logs.

### 4.3. `EncodeBusOrder()`

**Contract:**

* Same complement relationship as config packets, but returned in **bus order** via `ToBusOrder()`.

---

## 5. Cross-Component Contracts

### 5.1. Between `PhyCommand` and PacketBuilder

* `PhyCommand::BuildHeader` assumes:

  * `PacketBuilder::BuildPhyPacket()`:

    * Accepts `PhyParams` representing either:

      * A `AlphaPhyConfigPacket` (for configuration / root / gap),
      * A `PhyGlobalResumePacket` (for resume),
      * Or future PHY packet flavors encoded similarly.
    * Writes a **complete 2-quadlet PHY packet** in **bus order**.
    * Returns the exact number of bytes written (must be ≤ 16).

* The async Tx engine must **not** append payload descriptors after this header.

### 5.2. Between `PhyCommand` and Completion Strategy

* Transactions created with `PhyCommand` must be treated as:

  * **Header-only**, tCode=0xE.
  * **Complete-on-AT**:

    * AT ack is the only completion signal.
    * AR packets with tCode=0xE are delivered to a *separate* PHY event listener, not mapped to this transaction.

* Any future change to use `CompletionStrategy::CompleteOnPHY` instead of `CompleteOnAT` requires:

  * Updating the async engine’s AT path to distinguish PHY completion explicitly.
  * Keeping the AR path behavior unchanged (still no AR for this transaction).

### 5.3. Between Alpha PHY types and higher-level bus logic

* **Bus Reset / Root Assignment logic** must:

  * Use `AlphaPhyConfig` / `AlphaPhyConfigPacket` to construct:

    * Force-root packets,
    * GAP optimization packets,
    * Extended config packets (if you add them) with `IsExtendedConfig()`.

* **Diagnostic / logging tools** should:

  * Use `DecodeHostOrder` + `FromBusOrder` to show:

    * `rootId`, `forceRoot`, T bit, and effective `gapCount`.
  * Use `IsConfigQuadletHostOrder()` to distinguish config vs other PHY packets.
