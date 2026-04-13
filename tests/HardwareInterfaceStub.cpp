#include "HardwareInterface.hpp"
#include "RegisterMap.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace ASFW::Driver {
namespace {

using RegisterKey = uint32_t;

struct HardwareTestState {
    std::unordered_map<RegisterKey, uint32_t> registers;
    std::unordered_map<uint8_t, uint8_t> phyRegisters;
    std::vector<HardwareInterface::TestOperation> operations;
    bool busResetIssued{false};
    bool lastBusResetWasShort{false};
    bool initiateBusResetSucceeds{true};
    bool lastBusResetSucceeded{true};
    bool phyConfigIssued{false};
    bool sendPhyConfigSucceeds{true};
    bool lastPhyConfigSucceeded{true};
    bool globalResumeSent{false};
    bool contenderEnabled{false};
    std::optional<uint8_t> lastGapCount;
    std::optional<uint8_t> lastForceRootNode;
};

std::mutex gHardwareStateLock;
std::unordered_map<const HardwareInterface*, HardwareTestState> gHardwareStates;

template <typename Fn>
auto WithState(const HardwareInterface* hw, Fn&& fn) {
    std::scoped_lock lock(gHardwareStateLock);
    return fn(gHardwareStates[hw]);
}

constexpr RegisterKey KeyFor(Register32 reg) noexcept {
    return static_cast<RegisterKey>(reg);
}

} // namespace

HardwareInterface::HardwareInterface() {
    std::scoped_lock lock(gHardwareStateLock);
    gHardwareStates.try_emplace(this);
}

HardwareInterface::~HardwareInterface() {
    std::scoped_lock lock(gHardwareStateLock);
    gHardwareStates.erase(this);
}

kern_return_t HardwareInterface::Attach(IOService*, IOService*) { return kIOReturnSuccess; }

void HardwareInterface::Detach() {}

void HardwareInterface::BindAsyncControllerPort(ASFW::Async::IAsyncControllerPort* controllerPort) noexcept {
    asyncControllerPort_ = controllerPort;
}

uint32_t HardwareInterface::Read(Register32 reg) const noexcept {
    return WithState(this, [reg](HardwareTestState& state) -> uint32_t {
        const auto it = state.registers.find(KeyFor(reg));
        return (it != state.registers.end()) ? it->second : 0U;
    });
}

void HardwareInterface::Write(Register32 reg, uint32_t value) noexcept {
    WithState(this, [reg, value](HardwareTestState& state) {
        state.registers[KeyFor(reg)] = value;
        state.operations.push_back(TestOperation::Write);

        if (reg == Register32::kIntMaskSet) {
            state.registers[KeyFor(Register32::kIntMaskSet)] |= value;
        } else if (reg == Register32::kIntMaskClear) {
            state.registers[KeyFor(Register32::kIntMaskSet)] &=
                ~value;
        } else if (reg == Register32::kLinkControlSet) {
            state.registers[KeyFor(Register32::kLinkControl)] |= value;
        } else if (reg == Register32::kLinkControlClear) {
            state.registers[KeyFor(Register32::kLinkControl)] &=
                ~value;
        }
    });
}

void HardwareInterface::WriteAndFlush(Register32 reg, uint32_t value) {
    WithState(this, [reg, value](HardwareTestState& state) {
        state.registers[KeyFor(reg)] = value;
        state.operations.push_back(TestOperation::WriteAndFlush);

        if (reg == Register32::kIntEventClear) {
            state.registers[KeyFor(Register32::kIntEvent)] &=
                ~value;
        } else if (reg == Register32::kIntMaskSet) {
            state.registers[KeyFor(Register32::kIntMaskSet)] |= value;
        } else if (reg == Register32::kIntMaskClear) {
            state.registers[KeyFor(Register32::kIntMaskSet)] &=
                ~value;
        } else if (reg == Register32::kLinkControlSet) {
            state.registers[KeyFor(Register32::kLinkControl)] |= value;
        } else if (reg == Register32::kLinkControlClear) {
            state.registers[KeyFor(Register32::kLinkControl)] &=
                ~value;
        }
    });
}

void HardwareInterface::SetInterruptMask(uint32_t mask, bool enable) {
    if (enable) {
        IntMaskSet(mask);
    } else {
        IntMaskClear(mask);
    }
}

InterruptSnapshot HardwareInterface::CaptureInterruptSnapshot(uint64_t timestamp) const noexcept {
    return InterruptSnapshot{Read(Register32::kIntEvent), Read(Register32::kIntMaskSet),
                             Read(Register32::kIsoXmitEvent), Read(Register32::kIsoRecvEvent),
                             timestamp};
}

void HardwareInterface::SetLinkControlBits(uint32_t bits) { WriteAndFlush(Register32::kLinkControlSet, bits); }

void HardwareInterface::ClearLinkControlBits(uint32_t bits) {
    WriteAndFlush(Register32::kLinkControlClear, bits);
}

void HardwareInterface::ClearIntEvents(uint32_t mask) {
    WithState(this, [mask](HardwareTestState& state) {
        state.registers[KeyFor(Register32::kIntEventClear)] = mask;
        state.registers[KeyFor(Register32::kIntEvent)] &= ~mask;
        state.operations.push_back(TestOperation::ClearIntEvents);
    });
}

void HardwareInterface::ClearIsoXmitEvents(uint32_t mask) {
    WithState(this, [mask](HardwareTestState& state) {
        state.registers[KeyFor(Register32::kIsoXmitIntEventClear)] = mask;
        state.registers[KeyFor(Register32::kIsoXmitEvent)] &= ~mask;
        state.operations.push_back(TestOperation::ClearIsoXmitEvents);
    });
}

void HardwareInterface::ClearIsoRecvEvents(uint32_t mask) {
    WithState(this, [mask](HardwareTestState& state) {
        state.registers[KeyFor(Register32::kIsoRecvIntEventClear)] = mask;
        state.registers[KeyFor(Register32::kIsoRecvEvent)] &= ~mask;
        state.operations.push_back(TestOperation::ClearIsoRecvEvents);
    });
}

bool HardwareInterface::SendPhyConfig(std::optional<uint8_t> gapCount,
                                      std::optional<uint8_t> forceRootPhyId,
                                      std::string_view) {
    return WithState(this, [gapCount, forceRootPhyId](HardwareTestState& state) {
        state.phyConfigIssued = true;
        state.lastPhyConfigSucceeded = state.sendPhyConfigSucceeds;
        state.lastGapCount = gapCount;
        state.lastForceRootNode = forceRootPhyId;
        state.operations.push_back(TestOperation::SendPhyConfig);
        return state.sendPhyConfigSucceeds;
    });
}

bool HardwareInterface::SendPhyGlobalResume(uint8_t) {
    return WithState(this, [](HardwareTestState& state) {
        state.globalResumeSent = true;
        state.operations.push_back(TestOperation::SendPhyGlobalResume);
        return true;
    });
}

bool HardwareInterface::InitiateBusReset(bool shortReset) {
    return WithState(this, [shortReset](HardwareTestState& state) {
        state.busResetIssued = true;
        state.lastBusResetWasShort = shortReset;
        state.lastBusResetSucceeded = state.initiateBusResetSucceeds;
        state.operations.push_back(TestOperation::InitiateBusReset);
        return state.initiateBusResetSucceeds;
    });
}

bool HardwareInterface::ReadIntEvent(uint32_t& value) {
    value = Read(Register32::kIntEvent);
    return true;
}

void HardwareInterface::AckIntEvent(uint32_t bits) { ClearIntEvents(bits); }

void HardwareInterface::IntMaskSet(uint32_t bits) {
    WithState(this, [bits](HardwareTestState& state) {
        state.registers[KeyFor(Register32::kIntMaskSet)] |= bits;
        state.operations.push_back(TestOperation::WriteAndFlush);
    });
}

void HardwareInterface::IntMaskClear(uint32_t bits) {
    WithState(this, [bits](HardwareTestState& state) {
        state.registers[KeyFor(Register32::kIntMaskSet)] &=
            ~bits;
        state.registers[KeyFor(Register32::kIntMaskClear)] = bits;
        state.operations.push_back(TestOperation::WriteAndFlush);
    });
}

void HardwareInterface::SetContender(bool enable) {
    WithState(this, [enable](HardwareTestState& state) {
        state.contenderEnabled = enable;
        state.operations.push_back(TestOperation::SetContender);
    });
}

void HardwareInterface::InitializePhyReg4Cache() {}

void HardwareInterface::SetRootHoldOff(bool) {}

std::optional<uint8_t> HardwareInterface::ReadPhyRegister(uint8_t address) {
    return WithState(this, [address](HardwareTestState& state) -> std::optional<uint8_t> {
        const auto it = state.phyRegisters.find(address);
        if (it == state.phyRegisters.end()) {
            return std::nullopt;
        }
        return it->second;
    });
}

bool HardwareInterface::WritePhyRegister(uint8_t address, uint8_t value) {
    return WithState(this, [address, value](HardwareTestState& state) {
        state.phyRegisters[address] = value;
        return true;
    });
}

bool HardwareInterface::UpdatePhyRegister(uint8_t address, uint8_t clearBits, uint8_t setBits) {
    const uint8_t current = ReadPhyRegister(address).value_or(0U);
    return WritePhyRegister(address, static_cast<uint8_t>((current & ~clearBits) | setBits));
}

std::optional<HardwareInterface::DMABuffer> HardwareInterface::AllocateDMA(size_t length,
                                                                           uint64_t options,
                                                                           size_t alignment) {
    IOBufferMemoryDescriptor* buffer = nullptr;
    if (IOBufferMemoryDescriptor::Create(options, length, alignment, &buffer) != kIOReturnSuccess) {
        return std::nullopt;
    }

    auto* command = new IODMACommand();
    IOAddressSegment segment{};
    buffer->GetAddressRange(&segment);

    DMABuffer result;
    result.descriptor = OSSharedPtr<IOBufferMemoryDescriptor>(buffer, OSNoRetain);
    result.dmaCommand = OSSharedPtr<IODMACommand>(command, OSNoRetain);
    result.length = length;

    static std::atomic<uint32_t> sMockIOVA{0x20000000};

    const auto alignUp32 = [](uint32_t value, uint32_t alignmentValue) -> uint32_t {
        return (alignmentValue == 0U) ? value
                                      : ((value + (alignmentValue - 1U)) & ~(alignmentValue - 1U));
    };

    uint32_t aligned = (alignment == 0U) ? 16U : static_cast<uint32_t>(alignment);
    if ((aligned & (aligned - 1U)) != 0U) {
        aligned = 16U;
    }

    uint32_t cursor = sMockIOVA.load(std::memory_order_relaxed);
    for (;;) {
        const uint32_t base = alignUp32(cursor, aligned);
        const uint32_t next = base + static_cast<uint32_t>(length) + 4096U;
        if (next < base) {
            return std::nullopt;
        }
        if (sMockIOVA.compare_exchange_weak(cursor, next, std::memory_order_acq_rel)) {
            result.deviceAddress = base;
            break;
        }
    }

    return result;
}

OSSharedPtr<IODMACommand> HardwareInterface::CreateDMACommand() {
    return OSSharedPtr<IODMACommand>(new IODMACommand(), OSNoRetain);
}

uint32_t HardwareInterface::ReadHCControl() const noexcept { return Read(Register32::kHCControl); }

void HardwareInterface::SetHCControlBits(uint32_t bits) noexcept {
    Write(Register32::kHCControlSet, Read(Register32::kHCControl) | bits);
}

void HardwareInterface::ClearHCControlBits(uint32_t bits) noexcept {
    Write(Register32::kHCControlClear, Read(Register32::kHCControl) & ~bits);
}

uint32_t HardwareInterface::ReadNodeID() const noexcept { return Read(Register32::kNodeID); }

bool HardwareInterface::WaitHC(uint32_t mask, bool expectSet, uint32_t, uint32_t) const {
    const bool isSet = (Read(Register32::kHCControl) & mask) != 0U;
    return expectSet ? isSet : !isSet;
}

bool HardwareInterface::WaitLink(uint32_t mask, bool expectSet, uint32_t, uint32_t) const {
    const bool isSet = (Read(Register32::kLinkControl) & mask) != 0U;
    return expectSet ? isSet : !isSet;
}

bool HardwareInterface::WaitNodeIdValid(uint32_t) const {
    return (Read(Register32::kNodeID) & 0x80000000U) != 0U;
}

void HardwareInterface::FlushPostedWrites() const {}

std::pair<uint32_t, uint64_t> HardwareInterface::ReadCycleTimeAndUpTime() const noexcept {
    return {Read(Register32::kCycleTimer), mach_absolute_time()};
}

void HardwareInterface::SetTestRegister(Register32 reg, uint32_t value) noexcept {
    WithState(this, [reg, value](HardwareTestState& state) { state.registers[KeyFor(reg)] = value; });
}

uint32_t HardwareInterface::GetTestRegister(Register32 reg) const noexcept {
    return Read(reg);
}

std::vector<HardwareInterface::TestOperation> HardwareInterface::CopyTestOperations() const {
    return WithState(this, [](HardwareTestState& state) { return state.operations; });
}

bool HardwareInterface::TestBusResetIssued() const noexcept {
    return WithState(this, [](HardwareTestState& state) { return state.busResetIssued; });
}

bool HardwareInterface::TestLastBusResetWasShort() const noexcept {
    return WithState(this, [](HardwareTestState& state) { return state.lastBusResetWasShort; });
}

bool HardwareInterface::TestPhyConfigIssued() const noexcept {
    return WithState(this, [](HardwareTestState& state) { return state.phyConfigIssued; });
}

bool HardwareInterface::TestLastPhyConfigSucceeded() const noexcept {
    return WithState(this, [](HardwareTestState& state) { return state.lastPhyConfigSucceeded; });
}

bool HardwareInterface::TestLastBusResetSucceeded() const noexcept {
    return WithState(this, [](HardwareTestState& state) { return state.lastBusResetSucceeded; });
}

std::optional<uint8_t> HardwareInterface::TestLastGapCount() const noexcept {
    return WithState(this, [](HardwareTestState& state) { return state.lastGapCount; });
}

std::optional<uint8_t> HardwareInterface::TestLastForceRootNode() const noexcept {
    return WithState(this, [](HardwareTestState& state) { return state.lastForceRootNode; });
}

void HardwareInterface::SetTestSendPhyConfigResult(const bool success) noexcept {
    WithState(this, [success](HardwareTestState& state) { state.sendPhyConfigSucceeds = success; });
}

void HardwareInterface::SetTestInitiateBusResetResult(const bool success) noexcept {
    WithState(this,
              [success](HardwareTestState& state) { state.initiateBusResetSucceeds = success; });
}

void HardwareInterface::ResetTestState() noexcept {
    WithState(this, [](HardwareTestState& state) {
        state = HardwareTestState{};
    });
}

} // namespace ASFW::Driver
