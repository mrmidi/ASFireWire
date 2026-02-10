#pragma once

#include <optional>
#include <utility>  // for std::pair

#ifdef ASFW_HOST_TEST
#include "Testing/HostDriverKitStubs.hpp"
#include <vector>
#else
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSObject.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>
#endif

#include "../Common/BarrierUtils.hpp"
#include "../Controller/ControllerTypes.hpp"
#include "../Phy/PhyPackets.hpp"
#include "RegisterMap.hpp"

// Forward declare IOLock for PHY register serialization
struct IOLock;

namespace ASFW::Async {
class AsyncSubsystem;
} // namespace ASFW::Async

namespace ASFW::Driver {

class HardwareInterface {
public:
    HardwareInterface();
    ~HardwareInterface();

    kern_return_t Attach(IOService* owner, IOService* provider);
    void Detach();
    void SetAsyncSubsystem(ASFW::Async::AsyncSubsystem* subsystem) noexcept;

    [[nodiscard]] bool Attached() const noexcept { return static_cast<bool>(device_); }

    [[nodiscard]] uint32_t Read(Register32 reg) const noexcept;
    void Write(Register32 reg, uint32_t value) noexcept;
    void WriteAndFlush(Register32 reg, uint32_t value);

    void SetInterruptMask(uint32_t mask, bool enable);
    [[nodiscard]] InterruptSnapshot CaptureInterruptSnapshot(uint64_t timestamp) const noexcept;        void SetLinkControlBits(uint32_t bits);
        void ClearLinkControlBits(uint32_t bits);
        void ClearIntEvents(uint32_t mask);
        void ClearIsoXmitEvents(uint32_t mask);
        void ClearIsoRecvEvents(uint32_t mask);

        bool SendPhyConfig(std::optional<uint8_t> gapCount,
                        std::optional<uint8_t> forceRootPhyId,
                        std::string_view caller);
        bool SendPhyGlobalResume(uint8_t phyId);
        bool InitiateBusReset(bool shortReset);
        bool ReadIntEvent(uint32_t& value);
        void AckIntEvent(uint32_t bits);
        void IntMaskSet(uint32_t bits);

        void IntMaskClear(uint32_t bits);

        void SetContender(bool enable);

        void InitializePhyReg4Cache();

        void SetRootHoldOff(bool enable);

    [[nodiscard]] std::optional<uint8_t> ReadPhyRegister(uint8_t address);
    [[nodiscard]] bool WritePhyRegister(uint8_t address, uint8_t value);
    [[nodiscard]] bool UpdatePhyRegister(uint8_t address, uint8_t clearBits, uint8_t setBits);

        struct DMABuffer {
            OSSharedPtr<IOBufferMemoryDescriptor> descriptor;
            OSSharedPtr<IODMACommand> dmaCommand;
            uint64_t deviceAddress;
            size_t length;
        };

        [[nodiscard]] std::optional<DMABuffer> AllocateDMA(size_t length, uint64_t options, size_t alignment = 64);
        [[nodiscard]] OSSharedPtr<IODMACommand> CreateDMACommand();

        [[nodiscard]] uint32_t ReadHCControl() const noexcept;
        void SetHCControlBits(uint32_t bits) noexcept;
        void ClearHCControlBits(uint32_t bits) noexcept;

        [[nodiscard]] uint32_t ReadNodeID() const noexcept;
        
    [[nodiscard]] bool WaitHC(uint32_t mask, bool expectSet, uint32_t timeoutUsec, uint32_t pollIntervalUsec = 100) const;
    [[nodiscard]] bool WaitLink(uint32_t mask, bool expectSet, uint32_t timeoutUsec, uint32_t pollIntervalUsec = 100) const;
    [[nodiscard]] bool WaitNodeIdValid(uint32_t timeoutMs = 100) const;
    
    void FlushPostedWrites() const;
    
    [[nodiscard]] bool HasAgereQuirk() const noexcept { return quirk_agere_lsi_; }

    [[nodiscard]] uint32_t ReadIntEvent() const noexcept {
        return Read(Register32::kIntEvent);
    }

    [[nodiscard]] uint32_t ReadIntMask() const noexcept {
        return 0;
    }

    [[nodiscard]] uint32_t ReadLinkControl() const noexcept {
        return Read(Register32::kLinkControl);
    }

    // Cycle Timer access (OHCI §5.6, offset 0xF0)
    // Format: [seconds:7][cycles:13][offset:12] = 32 bits total
    // - seconds: 0-127 (wraps every 128 seconds, triggers cycle64Seconds interrupt)
    // - cycles: 0-7999 (8kHz isochronous cycle count)
    // - offset: 0-3071 (24.576 MHz sub-cycle ticks)
    [[nodiscard]] uint32_t ReadCycleTime() const noexcept {
        return Read(Register32::kCycleTimer);
    }

    // Atomically read cycle timer and host uptime for timestamp correlation
    [[nodiscard]] std::pair<uint32_t, uint64_t> ReadCycleTimeAndUpTime() const noexcept;

    private:
        OSSharedPtr<IOPCIDevice> device_;
        IOService* owner_{nullptr};
        uint8_t barIndex_{0};
        uint64_t barSize_{0};
        uint8_t barType_{0};
        ASFW::Async::AsyncSubsystem* asyncSubsystem_{nullptr};
        
        IOLock* phyLock_{nullptr};

        uint8_t phyReg4Cache_{0};

        bool quirk_agere_lsi_{false};
        
        std::optional<uint8_t> ReadPhyRegisterUnlocked(uint8_t address);
        bool WritePhyRegisterUnlocked(uint8_t address, uint8_t value);
    };

    } // namespace ASFW::Driver
