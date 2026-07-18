//
// IsochTypes.hpp
// ASFWDriver
//
// Core Isochronous Type Definitions and OHCI context definitions.
//

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <TargetConditionals.h>

namespace ASFW::Isoch {

// A single OHCI drain obtains one controller-cycle/host-time pair. Every
// packet completed by that drain is correlated to this pair by its consumer;
// the transport deliberately does not interpret packet content.
struct IsochReceiveBatch final {
    uint32_t drainCycleTimer{0};
    uint64_t drainHostTicks{0};
};

// Payload-opaque result of one IR descriptor. `payload` is valid only for the
// duration of the synchronous ConsumePacket() call; consumers must copy any
// data they need after that point.
struct IsochReceivePacket final {
    uint32_t descriptorIndex{0};
    uint16_t transferStatus{0};
    uint16_t residualCount{0};
    std::span<const uint8_t> payload{};
};

// Content consumers live outside transport. The receive context invokes these
// methods synchronously on its polling path and never reads or writes
// consumer-owned state itself.
class IIsochReceiveConsumer {
  public:
    virtual ~IIsochReceiveConsumer() = default;

    // Called only after the context has armed its DMA ring, and only after the
    // context has quiesced it.  A consumer may release payload-derived views
    // from OnReceiveQuiesced(); it must not retain `payload` past ConsumePacket.
    virtual void OnReceiveActivated() noexcept {}
    virtual void OnReceiveQuiesced() noexcept {}

    virtual void BeginReceiveBatch(const IsochReceiveBatch& batch) noexcept = 0;
    virtual void ConsumePacket(const IsochReceiveBatch& batch,
                               const IsochReceivePacket& packet) noexcept = 0;
};

// Callback for received packets (Raw transport level)
// @param data: Span containing packet data (header + payload)
// @param status: Status bits from descriptor
// @param timestamp: Timestamp of reception
using IsochReceiveCallback = std::function<void(std::span<const uint8_t> data, uint32_t status, uint64_t timestamp)>;

} // namespace ASFW::Isoch
