    #pragma once

    #include <cstddef>
    #include <cstdint>
    #include <optional>

    #ifdef ASFW_HOST_TEST
    #include "HostDriverKitStubs.hpp"
    #else
    #include <DriverKit/IOReturn.h>
    #include <DriverKit/OSObject.h>
    #include <DriverKit/OSSharedPtr.h>
    #include <DriverKit/IOBufferMemoryDescriptor.h>
    #include <DriverKit/IODMACommand.h>
    #include <PCIDriverKit/IOPCIDevice.h>
    #endif

    #include "BarrierUtils.hpp"
    #include "ControllerTypes.hpp"
    #include "RegisterMap.hpp"

    // Forward declare IOLock for PHY register serialization
    struct IOLock;

    // MEMORY SETUP VERIFICATION STATUS:
    // ✅ PCI Command Register: Sets bus master + memory space bits (matches Linux pci_set_master)
    // ✅ BAR Configuration: Uses BAR 0 for OHCI registers (matches Linux pcim_iomap_region)
    // ✅ Register Access: 32-bit quadlet access on boundaries (complies with OHCI §4.4)
    // ✅ DMA Setup: 32-bit address space, proper DriverKit APIs
    // ✅ BAR Validation: Enforces size (≥2048) and memory-space type requirements
    // ✅ DMA Alignment: Configurable alignment (default 64-byte), supports 16-byte for descriptors
    // ✅ PCI Write Verification: Reads back command register to confirm bus master + memory enable
    // ✅ BAR Index Validation: Confirms returned memory index matches requested BAR

    namespace ASFW::Driver {

    class HardwareInterface {
    public:
        HardwareInterface();
        ~HardwareInterface();

    kern_return_t Attach(IOService* owner, IOService* provider);
    void Detach();

    [[nodiscard]] bool Attached() const noexcept { return static_cast<bool>(device_); }

    [[nodiscard]] uint32_t Read(Register32 reg) const noexcept;
    void Write(Register32 reg, uint32_t value) noexcept;
    void WriteAndFlush(Register32 reg, uint32_t value);

    void SetInterruptMask(uint32_t mask, bool enable);
    // NOTE: InterruptSnapshot.intMask is ZEROED (IntMaskSet/Clear are write-only strobes per OHCI §5.7).
    // To get enabled interrupts, query InterruptManager::EnabledMask() shadow instead.
    [[nodiscard]] InterruptSnapshot CaptureInterruptSnapshot(uint64_t timestamp) const noexcept;        void SetLinkControlBits(uint32_t bits);
        void ClearLinkControlBits(uint32_t bits);
        void ClearIntEvents(uint32_t mask);
        void ClearIsoXmitEvents(uint32_t mask);
        void ClearIsoRecvEvents(uint32_t mask);

        // TODO(ASFW-BusReset-Design): These helpers currently provide tracing
        // hooks only. Once PHY register semantics are finalised we will plumb in
        // the real MMIO writes.
        bool SendPhyConfig(std::optional<uint8_t> gapCount,
                        std::optional<uint8_t> forceRootPhyId);
        bool InitiateBusReset(bool shortReset);
        bool ReadIntEvent(uint32_t& value);
        void AckIntEvent(uint32_t bits);
        void IntMaskSet(uint32_t bits);
        void IntMaskClear(uint32_t bits);

    // PHY register access (per OHCI §5.12)
    [[nodiscard]] std::optional<uint8_t> ReadPhyRegister(uint8_t address);
    [[nodiscard]] bool WritePhyRegister(uint8_t address, uint8_t value);
    [[nodiscard]] bool UpdatePhyRegister(uint8_t address, uint8_t clearBits, uint8_t setBits);

        struct DMABuffer {
            OSSharedPtr<IOBufferMemoryDescriptor> descriptor;
            OSSharedPtr<IODMACommand> dmaCommand;  // MUST keep alive to maintain IOMMU mapping
            uint64_t deviceAddress;                // Device-visible IOVA from IODMACommand
            size_t length;
        };

        [[nodiscard]] std::optional<DMABuffer> AllocateDMA(size_t length, uint64_t options, size_t alignment = 64);
        [[nodiscard]] OSSharedPtr<IODMACommand> CreateDMACommand();

        [[nodiscard]] uint32_t ReadHCControl() const noexcept;
        void SetHCControlBits(uint32_t bits) noexcept;
        void ClearHCControlBits(uint32_t bits) noexcept;

        [[nodiscard]] uint32_t ReadNodeID() const noexcept;
        
    // Generic wait-for-register helpers with device ejection detection
    // Thin intention-revealing wrappers for common register waits:
    [[nodiscard]] bool WaitHC(uint32_t mask, bool expectSet, uint32_t timeoutUsec, uint32_t pollIntervalUsec = 100) const;
    [[nodiscard]] bool WaitLink(uint32_t mask, bool expectSet, uint32_t timeoutUsec, uint32_t pollIntervalUsec = 100) const;
    [[nodiscard]] bool WaitNodeIdValid(uint32_t timeoutMs = 100) const;
    
    void FlushPostedWrites() const;
    
    // Hardware quirk detection
    [[nodiscard]] bool HasAgereQuirk() const noexcept { return quirk_agere_lsi_; }

    // ========================================================================
    // LLDB Debugging Helpers
    // ========================================================================

    /**
     * \brief Read IntEvent register (for LLDB snapshot convenience).
     *
     * \return Current IntEvent register value
     *
     * \par Usage in LLDB
     * ```
     * expr -R -- (uint32_t)this->hardware_->ReadIntEvent()
     * ```
     */
    [[nodiscard]] uint32_t ReadIntEvent() const noexcept {
        return Read(Register32::kIntEvent);
    }

    /**
     * \brief Read IntMask register shadow (use InterruptManager for real mask).
     *
     * \return Current IntMask register value (NOTE: read value is typically 0)
     *
     * \warning Per OHCI §5.7, IntMaskSet/Clear are write-only strobes, so
     * reading IntMask returns implementation-defined values (often 0).
     * Use InterruptManager::EnabledMask() for the actual enabled mask.
     */
    [[nodiscard]] uint32_t ReadIntMask() const noexcept {
        // OHCI only exposes IntMaskSet/IntMaskClear write strobes; there is no
        // distinct readable IntMask register. Return 0 to make the debug helper
        // safe while steering callers to the InterruptManager shadow.
        return 0;
    }

    /**
     * \brief Read LinkControl register (for LLDB snapshot convenience).
     *
     * \return Current LinkControl register value
     */
    [[nodiscard]] uint32_t ReadLinkControl() const noexcept {
        return Read(Register32::kLinkControl);
    }

    private:
        OSSharedPtr<IOPCIDevice> device_;
        IOService* owner_{nullptr};
        uint8_t barIndex_{0};
        uint64_t barSize_{0};
        uint8_t barType_{0};
        
        // Per Linux phy_reg_mutex (ohci.c): serialize all PHY register access via PhyControl
        // OHCI §5.12: Only one PHY transaction can be outstanding at a time
        IOLock* phyLock_{nullptr};
        
        // Hardware quirk detection: Agere/LSI FW643E reports invalid eventCode 0x10
        bool quirk_agere_lsi_{false};
        
        // Internal unlocked versions for use when lock is already held
        std::optional<uint8_t> ReadPhyRegisterUnlocked(uint8_t address);
        bool WritePhyRegisterUnlocked(uint8_t address, uint8_t value);
    };

    } // namespace ASFW::Driver
