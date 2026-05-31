#pragma once

#include <optional>
#include <utility> // for std::pair


#ifdef ASFW_HOST_TEST
#include "Testing/HostDriverKitStubs.hpp"
#include <vector>
#else
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSObject.h>
#include <DriverKit/OSSharedPtr.h>
#include <PCIDriverKit/IOPCIDevice.h>
#endif

#include "../Common/BarrierUtils.hpp"
#include "../Controller/ControllerTypes.hpp"
#include "../Phy/PhyPackets.hpp"
#include "RegisterMap.hpp"

// Forward declare IOLock for PHY register serialization
struct IOLock;

namespace ASFW::Async {
class IAsyncControllerPort;
} // namespace ASFW::Async

namespace ASFW::Driver {

/// Result of a local autonomous IRM CSR compare-swap operation (OHCI §5.5).
struct LocalCSRLockResult {
    enum class Status : uint8_t {
        Success,             ///< Hardware completed the operation.
        Timeout,             ///< CSRControl done bit was never set.
        HardwareUnavailable, ///< No hardware interface (device_ is null).
    };
    Status status{Status::HardwareUnavailable};
    uint32_t oldValue{0};    ///< Previous register value (valid only on Success).
    bool compareMatched{false}; ///< True if oldValue == compareValue (swap occurred).
};

struct LocalCSRReadResult {
    LocalCSRLockResult::Status status{LocalCSRLockResult::Status::HardwareUnavailable};
    uint32_t value{0};
};

struct LocalCSRWriteResult {
    LocalCSRLockResult::Status status{LocalCSRLockResult::Status::HardwareUnavailable};
};


class HardwareInterface {
  public:
    HardwareInterface();
    ~HardwareInterface();

    kern_return_t Attach(IOService* owner, IOService* provider);
    void Detach();
    void BindAsyncControllerPort(ASFW::Async::IAsyncControllerPort* controllerPort) noexcept;

    [[nodiscard]] bool Attached() const noexcept { return static_cast<bool>(device_); }

    [[nodiscard]] uint32_t Read(Register32 reg) const noexcept;
    void Write(Register32 reg, uint32_t value) noexcept;
    void WriteAndFlush(Register32 reg, uint32_t value);

    void SetInterruptMask(uint32_t mask, bool enable);
    [[nodiscard]] InterruptSnapshot CaptureInterruptSnapshot(uint64_t timestamp) const noexcept;
    void SetLinkControlBits(uint32_t bits);
    void ClearLinkControlBits(uint32_t bits);
    void ClearIntEvents(uint32_t mask);
    void ClearIsoXmitEvents(uint32_t mask);
    void ClearIsoRecvEvents(uint32_t mask);

    bool SendPhyConfig(std::optional<uint8_t> gapCount, std::optional<uint8_t> forceRootPhyId,
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

    [[nodiscard]] std::optional<DMABuffer> AllocateDMA(size_t length, uint64_t options,
                                                       size_t alignment = 64);
    [[nodiscard]] OSSharedPtr<IODMACommand> CreateDMACommand();

    [[nodiscard]] uint32_t ReadHCControl() const noexcept;
    void SetHCControlBits(uint32_t bits) noexcept;
    void ClearHCControlBits(uint32_t bits) noexcept;

    [[nodiscard]] uint32_t ReadNodeID() const noexcept;

    [[nodiscard]] bool InitialIRMRegistersProgrammed() const noexcept {
        return initialIRMRegistersProgrammed_;
    }

    /**
     * @brief Forced state override for unit tests.
     */
    void SetInitialIRMRegistersProgrammed(bool programmed) noexcept {
        initialIRMRegistersProgrammed_ = programmed;
    }

    [[nodiscard]] bool WaitHC(uint32_t mask, bool expectSet, uint32_t timeoutUsec,
                              uint32_t pollIntervalUsec = 100) const;
    [[nodiscard]] bool WaitLink(uint32_t mask, bool expectSet, uint32_t timeoutUsec,
                                uint32_t pollIntervalUsec = 100) const;
    [[nodiscard]] bool WaitNodeIdValid(uint32_t timeoutMs = 100) const;

    void FlushPostedWrites() const;

    [[nodiscard]] bool HasAgereQuirk() const noexcept { return quirk_agere_lsi_; }

    [[nodiscard]] uint32_t ReadIntEvent() const noexcept { return Read(Register32::kIntEvent); }

    [[nodiscard]] uint32_t ReadIntMask() const noexcept { return 0; }

    [[nodiscard]] uint32_t ReadLinkControl() const noexcept {
        return Read(Register32::kLinkControl);
    }

    /**
     * @brief Checks if the local OHCI cycleMaster bit is currently set in LinkControl.
     */
    [[nodiscard]] bool IsLocalCycleMasterEnabled() const noexcept;

    /**
     * @brief Sets or clears the local OHCI cycleMaster bit via LinkControlSet/Clear.
     * Per OHCI §5.3.3: This node generates cycle-start packets only when it is the bus root.
     * Returns true if the hardware readback matches the requested state.
     */
    bool SetLocalCycleMasterEnabled(bool enable) noexcept;

    // Cycle Timer access (OHCI §5.6, offset 0xF0)
    // Format: [seconds:7][cycles:13][offset:12] = 32 bits total
    // - seconds: 0-127 (wraps every 128 seconds, triggers cycle64Seconds interrupt)
    // - cycles: 0-7999 (8kHz isochronous cycle count)
    // - offset: 0-3071 (24.576 MHz sub-cycle ticks)
    [[nodiscard]] uint32_t ReadCycleTime() const noexcept { return Read(Register32::kCycleTimer); }

    // Atomically read cycle timer and host uptime for timestamp correlation
    [[nodiscard]] std::pair<uint32_t, uint64_t> ReadCycleTimeAndUpTime() const noexcept;

    // Local autonomous IRM CSR helpers (OHCI §5.5)
    [[nodiscard]] LocalCSRWriteResult WriteLocalIRMResource(uint32_t selectCode, uint32_t value) noexcept;
    [[nodiscard]] LocalCSRReadResult ReadLocalIRMResource(uint32_t selectCode) noexcept;
    [[nodiscard]] LocalCSRLockResult CompareSwapLocalIRMResource(
        uint32_t selectCode, uint32_t compareValue, uint32_t newValue) noexcept;

    /**
     * @brief Writes canonical initial values to OHCI registers 0x0B0, 0x0B4, and 0x0B8.
     * OHCI 1.1 §5.5: These registers provide the default values for the autonomous CSRs
     * after a bus reset.
     */
    kern_return_t ProgramInitialIRMResourceRegisters() noexcept;

#ifdef ASFW_HOST_TEST
    enum class TestOperation : uint8_t {
        Write,
        WriteAndFlush,
        ClearIntEvents,
        ClearIsoXmitEvents,
        ClearIsoRecvEvents,
        SendPhyConfig,
        InitiateBusReset,
        SendPhyGlobalResume,
        SetContender,
    };

    void SetTestRegister(Register32 reg, uint32_t value) noexcept;
    [[nodiscard]] uint32_t GetTestRegister(Register32 reg) const noexcept;
    [[nodiscard]] std::vector<TestOperation> CopyTestOperations() const;
    [[nodiscard]] bool TestBusResetIssued() const noexcept;
    [[nodiscard]] bool TestLastBusResetWasShort() const noexcept;
    [[nodiscard]] bool TestPhyConfigIssued() const noexcept;
    [[nodiscard]] bool TestLastPhyConfigSucceeded() const noexcept;
    [[nodiscard]] bool TestLastBusResetSucceeded() const noexcept;
    [[nodiscard]] std::optional<uint8_t> TestLastGapCount() const noexcept;
    [[nodiscard]] std::optional<uint8_t> TestLastForceRootNode() const noexcept;
    void SetTestSendPhyConfigResult(bool success) noexcept;
    void SetTestInitiateBusResetResult(bool success) noexcept;
    void ResetTestState() noexcept;
#endif

  private:
    OSSharedPtr<IOPCIDevice> device_;
    IOService* owner_{nullptr};
    uint8_t barIndex_{0};
    uint64_t barSize_{0};
    uint8_t barType_{0};
    ASFW::Async::IAsyncControllerPort* asyncControllerPort_{nullptr};

    IOLock* phyLock_{nullptr};

    uint8_t phyReg4Cache_{0};

    bool quirk_agere_lsi_{false};
    bool initialIRMRegistersProgrammed_{false};

    std::optional<uint8_t> ReadPhyRegisterUnlocked(uint8_t address);
    bool WritePhyRegisterUnlocked(uint8_t address, uint8_t value);
};

} // namespace ASFW::Driver
