#pragma once
// ASFWDriver/Async/Engine/ContextManager.hpp

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <expected>   // C++23

#include "../../Logging/Logging.hpp"
#include "../Rings/DescriptorRing.hpp"
#include "../Rings/BufferRing.hpp"
#include "../Contexts/ContextBase.hpp"  // For ATRequestTag, ATResponseTag full definitions

// Forward declarations for light-weight accessors
namespace ASFW::Async {
    class DMAMemoryManager;
    class ATRequestContext;
    class ATResponseContext;
    class ARRequestContext;
    class ARResponseContext;
    class PayloadRegistry;
    class DescriptorBuilder;
}

// Correct namespace for your HW interface:
namespace ASFW::Driver { class HardwareInterface; }

namespace ASFW::Async::Engine {

// Forward declarations for FSM-based managers
template<typename ContextT, typename RingT, typename RoleTag> class ATManager;
// ATRequestTag and ATResponseTag are defined in ASFW::Async namespace

struct ProvisionSpec {
    size_t atReqDescCount = 256;
    size_t atRespDescCount = 64;
    size_t arReqBufCount  = 128, arReqBufSize  = 4160;
    size_t arRespBufCount = 256, arRespBufSize = 4160;
};

struct ContextManagerSnapshot {
    uint32_t magic{0x12345678};
    uint32_t contextState{0};
    uint32_t atReqRingHead{0};
    uint32_t atReqRingTail{0};
    uint32_t atRspRingHead{0};
    uint32_t atRspRingTail{0};
    uint32_t outstandingCount{0};
    uint32_t crc32{0};

    [[nodiscard]] uint32_t CalculateCRC32() const noexcept;
};

class ContextManager final {
public:
    using Snapshot = ContextManagerSnapshot;

    ContextManager() noexcept;
    ~ContextManager();

    kern_return_t provision(ASFW::Driver::HardwareInterface& hw,
                            const ProvisionSpec& spec) noexcept;

    void          teardown(bool disable_hw) noexcept;
    kern_return_t armAR() noexcept;
    kern_return_t stopAT() noexcept;
    kern_return_t stopAR() noexcept;
    void          flushAT() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

    // Payload registry wiring (non-owning)
    void SetPayloads(ASFW::Async::PayloadRegistry* p) noexcept;
    ASFW::Async::PayloadRegistry* Payloads() noexcept;

    // Lightweight accessors for incremental wiring (non-owning)
    // Return nullptr if not provisioned.
    DescriptorRing* AtRequestRing() noexcept;
    DescriptorRing* AtResponseRing() noexcept;
    BufferRing*     ArRequestRing() noexcept;
    BufferRing*     ArResponseRing() noexcept;
    ASFW::Async::DMAMemoryManager* DmaManager() noexcept;
    ASFW::Async::ATRequestContext*  GetAtRequestContext() noexcept;
    ASFW::Async::ATResponseContext* GetAtResponseContext() noexcept;
    ASFW::Async::ARRequestContext*  GetArRequestContext() noexcept;
    ASFW::Async::ARResponseContext* GetArResponseContext() noexcept;

    // New FSM-based AT manager accessors
    // These replace the old manual state tracking methods
    ATManager<ASFW::Async::ATRequestContext, DescriptorRing, ASFW::Async::ATRequestTag>* GetATRequestManager() noexcept;
    ATManager<ASFW::Async::ATResponseContext, DescriptorRing, ASFW::Async::ATResponseTag>* GetATResponseManager() noexcept;
    ASFW::Async::DescriptorBuilder* GetDescriptorBuilder() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ASFW::Async::Engine
