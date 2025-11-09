#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>

#include "../../Core/HardwareInterface.hpp"
#include "../../Core/RegisterMap.hpp"

namespace ASFW::Async {

/**
 * \brief C++20 concept for OHCI DMA context role tags.
 *
 * Enforces compile-time contract: each context role must define register
 * offsets and a human-readable name for logging/diagnostics.
 *
 * \par Design Rationale
 * Using concepts instead of runtime polymorphism (virtual functions) ensures
 * zero overhead for context operations while maintaining type safety.
 *
 * \par Usage Example
 * \code
 * struct MyContextTag {
 *     static constexpr Driver::Register32 kControlSetReg = ...;
 *     static constexpr Driver::Register32 kControlClearReg = ...;
 *     static constexpr Driver::Register32 kCommandPtrReg = ...;
 *     static constexpr std::string_view kContextName = "My Context";
 * };
 * static_assert(ContextRole<MyContextTag>);
 * \endcode
 */
template<typename T>
concept ContextRole = requires {
    { T::kControlSetReg } -> std::convertible_to<Driver::Register32>;
    { T::kControlClearReg } -> std::convertible_to<Driver::Register32>;
    { T::kCommandPtrReg } -> std::convertible_to<Driver::Register32>;
    { T::kContextName } -> std::convertible_to<std::string_view>;
};

/**
 * \brief Register offset tag for AT Request context.
 *
 * \par OHCI Specification
 * - AsReqTrContextControlSet: 0x180 (§7.2.3 Table 7-6)
 * - AsReqTrContextControlClear: 0x184
 * - AsReqTrCommandPtr: 0x18C (§7.2.4)
 *
 * \par Apple Pattern
 * AppleFWOHCI_AsyncTransmitRequest uses these register offsets.
 */
struct ATRequestTag {
    static constexpr Driver::Register32 kControlSetReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsReqTrContextControlSet);
    static constexpr Driver::Register32 kControlClearReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsReqTrContextControlClear);
    static constexpr Driver::Register32 kCommandPtrReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsReqTrCommandPtr);
    static constexpr std::string_view kContextName = "AT Request";
};

/**
 * \brief Register offset tag for AT Response context.
 *
 * \par OHCI Specification
 * - AsRspTrContextControlSet: 0x1A0 (§7.2.3 Table 7-6)
 * - AsRspTrContextControlClear: 0x1A4
 * - AsRspTrCommandPtr: 0x1AC (§7.2.4)
 *
 * \par Apple Pattern
 * AppleFWOHCI_AsyncTransmitResponse uses these register offsets.
 */
struct ATResponseTag {
    static constexpr Driver::Register32 kControlSetReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsRspTrContextControlSet);
    static constexpr Driver::Register32 kControlClearReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsRspTrContextControlClear);
    static constexpr Driver::Register32 kCommandPtrReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsRspTrCommandPtr);
    static constexpr std::string_view kContextName = "AT Response";
};

/**
 * \brief Register offset tag for AR Request context.
 *
 * \par OHCI Specification
 * - AsReqRcvContextControlSet: 0x400 (§8.2 Table 8-2)
 * - AsReqRcvContextControlClear: 0x404
 * - AsReqRcvCommandPtr: 0x40C (§8.2)
 *
 * \par Apple Pattern
 * AppleFWOHCI_AsyncReceiveRequest uses these register offsets.
 *
 * \par Special Behavior
 * AR Request context receives PHY packets and synthetic bus-reset packets
 * when LinkControl.rcvPhyPkt=1 (OHCI §8.4.2.3, §C.3).
 */
struct ARRequestTag {
    static constexpr Driver::Register32 kControlSetReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsReqRcvContextControlSet);
    static constexpr Driver::Register32 kControlClearReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsReqRcvContextControlClear);
    static constexpr Driver::Register32 kCommandPtrReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsReqRcvCommandPtr);
    static constexpr std::string_view kContextName = "AR Request";
};

/**
 * \brief Register offset tag for AR Response context.
 *
 * \par OHCI Specification
 * - AsRspRcvContextControlSet: 0x420 (§8.2 Table 8-2)
 * - AsRspRcvContextControlClear: 0x424
 * - AsRspRcvCommandPtr: 0x42C (§8.2)
 *
 * \par Apple Pattern
 * AppleFWOHCI_AsyncReceiveResponse uses these register offsets.
 */
struct ARResponseTag {
    static constexpr Driver::Register32 kControlSetReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsRspRcvContextControlSet);
    static constexpr Driver::Register32 kControlClearReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsRspRcvContextControlClear);
    static constexpr Driver::Register32 kCommandPtrReg =
        static_cast<Driver::Register32>(DMAContextHelpers::AsRspRcvCommandPtr);
    static constexpr std::string_view kContextName = "AR Response";
};

// Compile-time validation
static_assert(ContextRole<ATRequestTag>);
static_assert(ContextRole<ATResponseTag>);
static_assert(ContextRole<ARRequestTag>);
static_assert(ContextRole<ARResponseTag>);

/**
 * \brief CRTP base class for OHCI DMA context operations.
 *
 * Provides common register access patterns for all context types (AT/AR).
 * Uses Curiously Recurring Template Pattern for zero-overhead polymorphism.
 *
 * \tparam Derived Concrete context class (e.g., ATRequestContext)
 * \tparam Tag Context role tag (e.g., ATRequestTag) defining register offsets
 *
 * \par Design Rationale
 * - **CRTP**: Compile-time polymorphism avoids vtable overhead
 * - **Concepts**: ContextRole ensures type safety without runtime checks
 * - **Constexpr**: Register offsets resolved at compile time
 *
 * \par OHCI Specification References
 * - §7.2.3: ContextControl register (run/wake/active/dead bits)
 * - §7.2.4: CommandPtr register (descriptor chain head)
 *
 * \par Linux Pattern
 * See drivers/firewire/ohci.c:
 * - reg_read() / reg_write() for register access
 * - context_run() / context_stop() for lifecycle
 */
template<typename Derived, ContextRole Tag>
class ContextBase {
public:
    ContextBase() = default;
    ~ContextBase() = default;

    /**
     * \brief Initialize context with hardware interface.
     *
     * \param hw Hardware register interface
     * \return kIOReturnSuccess or error code
     */
    [[nodiscard]] kern_return_t Initialize(Driver::HardwareInterface& hw) {
        if (hw_ != nullptr) {
            return kIOReturnExclusiveAccess;
        }
        hw_ = &hw;
        return kIOReturnSuccess;
    }

    /**
     * \brief Read ContextControl register.
     *
     * \return Current ContextControl value
     *
     * \par OHCI §7.2.3 / §8.2
     * ContextControl bits:
     * - [15] run: Context active when 1
     * - [13] active: Hardware processing descriptors
     * - [12] wake: Write 1 to signal new descriptors available
     * - [5] dead: Context encountered fatal error
     */
    [[nodiscard]] uint32_t ReadControl() const noexcept {
        return hw_->Read(Tag::kControlSetReg);
    }

    /**
     * \brief Write ContextControl.Set register.
     *
     * \param bits Bits to set (write-1-to-set semantics)
     *
     * \par Usage
     * - Set run bit: WriteControlSet(1 << 15)
     * - Set wake bit: WriteControlSet(1 << 12)
     */
    void WriteControlSet(uint32_t bits) noexcept {
        hw_->Write(Tag::kControlSetReg, bits);
    }

    /**
     * \brief Write ContextControl.Clear register.
     *
     * \param bits Bits to clear (write-1-to-clear semantics)
     *
     * \par Usage
     * - Clear run bit: WriteControlClear(1 << 15)
     */
    void WriteControlClear(uint32_t bits) noexcept {
        hw_->Write(Tag::kControlClearReg, bits);
    }

    /**
     * \brief Write CommandPtr register.
     *
     * \param commandPtr Physical address of first descriptor (16-byte aligned)
     *                    with Z field encoded in lower bits
     *
     * \par OHCI §7.2.4 / §8.2
     * CommandPtr format:
     * - AT contexts: [31:4] = physAddr[31:4], [3:0] = Z (block count)
     * - AR contexts: [31:4] = physAddr[31:4], [0] = Z (continue flag)
     *
     * \warning Must be written BEFORE setting ContextControl.run bit.
     */
    void WriteCommandPtr(uint32_t commandPtr) noexcept {
        hw_->Write(Tag::kCommandPtrReg, commandPtr);
    }

    /**
     * \brief Read CommandPtr register.
     *
     * @return Current CommandPtr value as last written by hardware or software.
     */
    [[nodiscard]] uint32_t ReadCommandPtr() const noexcept {
        return hw_->Read(Tag::kCommandPtrReg);
    }

    /**
     * \brief Check if context is currently active.
     *
     * \return true if ContextControl.active bit is set
     *
     * \par OHCI §7.2.3 / §8.2
     * active=1 indicates hardware is actively processing descriptors.
     * Used for polling during context stop sequence.
     */
    [[nodiscard]] bool IsActive() const noexcept {
        constexpr uint32_t kActiveBit = 1u << 13;
        return (ReadControl() & kActiveBit) != 0;
    }

    /**
     * \brief Check if context has run bit set.
     *
     * \return true if ContextControl.run bit is set
     */
    [[nodiscard]] bool IsRunning() const noexcept {
        constexpr uint32_t kRunBit = 1u << 15;
        return (ReadControl() & kRunBit) != 0;
    }

    /**
     * \brief Get context name for logging.
     *
     * \return Human-readable context name from role tag
     */
    [[nodiscard]] constexpr std::string_view ContextName() const noexcept {
        return Tag::kContextName;
    }

protected:
    /// CRTP accessor for derived class
    Derived& derived() noexcept { return static_cast<Derived&>(*this); }
    const Derived& derived() const noexcept { return static_cast<const Derived&>(*this); }

    Driver::HardwareInterface* hw_{nullptr};
};

} // namespace ASFW::Async
