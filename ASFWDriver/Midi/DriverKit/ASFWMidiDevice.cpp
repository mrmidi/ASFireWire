//
// ASFWMidiDevice.cpp
// ASFWDriver
//

#include <cstdio>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <MIDIDriverKit/MIDIDriverKit.h>

#include <net.mrmidi.ASFW.ASFWDriver/ASFWMidiDevice.h>

#include "../../Logging/Logging.hpp"
#include "../Transport/MidiTransportBlock.hpp"
#include "../Ump/MidiByteStreamToUmp.hpp"
#include "../Ump/UmpToMidiByteStream.hpp"

struct ASFWMidiDevice_IVars {
    OSSharedPtr<IOUserMIDIDriver> driver;
    OSSharedPtr<IODispatchQueue> workQueue;
    uint32_t sourcePorts{0};
    uint32_t destinationPorts{0};

    // The seam. `block` points into memory the nub owns and the driver keeps
    // mapped; `bound` is what the I/O blocks check, because an entity's block
    // can still be invoked after Stop begins.
    ASFW::Midi::MidiTransportBlock* block{nullptr};
    uint64_t streamEpoch{0};
    std::atomic<bool> bound{false};

    // One converter per port per direction: both are stateful (running status,
    // SysEx accumulation) and must not be shared between ports.
    ASFW::Midi::Ump::UmpToMidiByteStream toWire[ASFW::Midi::kMidiPortsPerDirection]{};
    ASFW::Midi::Ump::MidiByteStreamToUmp fromWire[ASFW::Midi::kMidiPortsPerDirection]{};

    // Bounded scratch, reused on the work queue only. Sized so one drain pass
    // can carry a full ring without allocating on a path that must not.
    // Last observed discontinuity count per port, so a loss is acted on once.
    uint64_t seenGaps[ASFW::Midi::kMidiPortsPerDirection]{};

    uint8_t rxBytes[ASFW::Midi::kMidiRingCapacityBytes]{};
    ASFW::Midi::Ump::UmpWord rxWords[
        ASFW::Midi::kMidiRingCapacityBytes *
        ASFW::Midi::Ump::MidiByteStreamToUmp::kMaxWordsPerByte]{};

    /// Real-time thread. Converts one destination's UMP to MIDI 1.0 bytes and
    /// enqueues them for the wire. Allocation-free and lock-free by
    /// construction; every buffer it touches is a fixed member.
    kern_return_t WriteToWire(uint32_t port, const uint32_t* umpWords,
                              size_t numWords) noexcept {
        if (!bound.load(std::memory_order_acquire)) return kIOReturnNotReady;
        if (block == nullptr || port >= ASFW::Midi::kMidiPortsPerDirection) {
            return kIOReturnBadArgument;
        }
        if (!block->Usable(streamEpoch)) return kIOReturnNotReady;

        const auto pulled = toWire[port].Pull({umpWords, numWords}, txBytes);
        if (pulled.bytesWritten == 0) return kIOReturnSuccess;
        // All-or-nothing: a rejected message is better than a truncated one,
        // which would leave the device desynchronised until the next status
        // byte.
        if (!block->hostToDevice[port].TryWrite({txBytes, pulled.bytesWritten})) {
            return kIOReturnNoSpace;
        }
        return kIOReturnSuccess;
    }

    uint8_t txBytes[ASFW::Midi::Ump::UmpToMidiByteStream::kMaxBytesPerPacket * 64]{};
};

namespace {

/// "ASFW MIDI 1", "ASFW MIDI 2" ... one per physical jack pair.
OSSharedPtr<OSString> MakeEntityName(uint32_t oneBasedIndex) {
    char name[32];
    std::snprintf(name, sizeof(name), "MIDI %u", oneBasedIndex);
    return OSSharedPtr(OSString::withCString(name), OSNoRetain);
}

} // namespace

bool ASFWMidiDevice::init(IOUserMIDIDriver* driver,
                          OSString* deviceUID,
                          OSString* modelUID,
                          OSString* manufacturerUID,
                          uint32_t sourcePorts,
                          uint32_t destinationPorts) {
    if (!super::init(driver, deviceUID, modelUID, manufacturerUID)) {
        return false;
    }
    ivars = IONewZero(ASFWMidiDevice_IVars, 1);
    if (ivars == nullptr) return false;

    ivars->driver = OSSharedPtr(driver, OSRetain);
    ivars->workQueue = GetWorkQueue();
    ivars->sourcePorts = sourcePorts;
    ivars->destinationPorts = destinationPorts;

    // One entity per physical jack pair. A device with two inputs and one
    // output publishes two entities, the second with a source and no
    // destination -- not a phantom destination whose bytes would go nowhere.
    const uint32_t entities =
        sourcePorts > destinationPorts ? sourcePorts : destinationPorts;
    for (uint32_t i = 0; i < entities; ++i) {
        const uint32_t sources = (i < sourcePorts) ? 1u : 0u;
        const uint32_t destinations = (i < destinationPorts) ? 1u : 0u;
        auto name = MakeEntityName(i + 1);
        if (!name) {
            ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: entity %u name alloc failed", i);
            return false;
        }
        // MIDIProtocol_1_0, not the sample's 2_0: the wire carries a MIDI 1.0
        // byte stream, so 1.0 is the honest declaration of what reaches the
        // device. Translation to UMP happens at the seam, and claiming 2.0
        // would promise resolution the hardware cannot carry.
        auto entity = IOUserMIDIEntity::Create(
            ivars->driver.get(), this, name.get(),
            IOUserMIDIProtocolID::MIDIProtocol_1_0, sources, destinations);
        if (!entity) {
            ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: entity %u create failed", i);
            return false;
        }
        const kern_return_t ret = AddEntity(entity.get());
        if (ret != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: AddEntity %u failed 0x%x", i, ret);
            return false;
        }
    }

    // The device exists but no FireWire stream is running yet. StartIO clears
    // this, so CoreMIDI shows the endpoints greyed until bytes can actually
    // flow.
    auto offline = OSSharedPtr(OSNumber::withNumber(uint64_t{1}, 32), OSNoRetain);
    if (offline) SetProperty(IOUserMIDIProperty::Offline, offline.get());

    ASFW_LOG(Midi,
             "ASFWMidiDevice: init entities=%u sources=%u destinations=%u",
             entities, sourcePorts, destinationPorts);
    return true;
}

void ASFWMidiDevice::free() {
    if (ivars != nullptr) {
        ivars->driver.reset();
        ivars->workQueue.reset();
    }
    IOSafeDeleteNULL(ivars, ASFWMidiDevice_IVars, 1);
    super::free();
}

uint32_t ASFWMidiDevice::SourcePortCount() const {
    return ivars ? ivars->sourcePorts : 0;
}

uint32_t ASFWMidiDevice::DestinationPortCount() const {
    return ivars ? ivars->destinationPorts : 0;
}

void ASFWMidiDevice::BindTransport(void* block, uint64_t streamEpoch) {
    if (ivars == nullptr || block == nullptr) return;
    ivars->block = static_cast<ASFW::Midi::MidiTransportBlock*>(block);
    ivars->streamEpoch = streamEpoch;
    ivars->bound.store(true, std::memory_order_release);

    // Each destination gets its own port index, captured by value. The block
    // runs on MIDIDriverKit's real-time thread: no allocation, no logging, no
    // locks below this point.
    __block uint32_t portIndex = 0;
    auto entities = GetEntities();
    if (!entities) return;
    entities->iterateObjects(^bool(OSObject* object) {
        auto* entity = OSDynamicCast(IOUserMIDIEntity, object);
        if (entity == nullptr) return false;
        auto destination = entity->GetDestination(0);
        if (destination) {
            const uint32_t port = portIndex;
            auto* ivarsCopy = ivars;
            auto ioBlock = ^kern_return_t(IOUserMIDIUMPWord const* umpWords,
                                          size_t numWords) {
                return ivarsCopy->WriteToWire(port, umpWords, numWords);
            };
            (void)destination->SetIOBlock(ioBlock);
        }
        ++portIndex;
        return false;
    });
    ASFW_LOG(Midi, "ASFWMidiDevice: transport bound epoch=%llu", streamEpoch);
}

void ASFWMidiDevice::UnbindTransport() {
    if (ivars == nullptr) return;
    // Clear the flag first: an I/O block already running will see it and return
    // without touching the rings, and one that starts afterwards does nothing.
    ivars->bound.store(false, std::memory_order_release);
    ivars->block = nullptr;
    ASFW_LOG(Midi, "ASFWMidiDevice: transport unbound");
}

void ASFWMidiDevice::DrainReceiveRings() {
    if (ivars == nullptr) return;
    if (!ivars->bound.load(std::memory_order_acquire)) return;
    auto* block = ivars->block;
    if (block == nullptr || !block->Usable(ivars->streamEpoch)) return;

    auto entities = GetEntities();
    if (!entities) return;

    __block uint32_t portIndex = 0;
    entities->iterateObjects(^bool(OSObject* object) {
        auto* entity = OSDynamicCast(IOUserMIDIEntity, object);
        if (entity == nullptr) return false;
        const uint32_t port = portIndex++;
        if (port >= ASFW::Midi::kMidiPortsPerDirection) return true;

        auto source = entity->GetSource(0);
        if (!source) return false;
        auto& ring = block->deviceToHost[port];

        // A discontinuity means bytes were lost between what is queued and what
        // came before. Reset the parser at that point so the two sides cannot
        // join into a message nobody sent.
        const uint64_t gaps = ring.discontinuities.load(std::memory_order_relaxed);
        if (gaps != ivars->seenGaps[port]) {
            ivars->seenGaps[port] = gaps;
            ivars->fromWire[port].Reset();
        }

        const uint32_t count = ring.Peek(ivars->rxBytes);
        if (count == 0) return false;
        const auto pushed = ivars->fromWire[port].Push(
            {ivars->rxBytes, count}, ivars->rxWords);
        ring.Consume(pushed.bytesConsumed);
        if (pushed.wordsWritten > 0) {
            (void)source->Send(ivars->rxWords, pushed.wordsWritten);
        }
        return false;
    });
}

kern_return_t ASFWMidiDevice::StartIO() {
    __block kern_return_t error = kIOReturnSuccess;
    if (!ivars || !ivars->workQueue) return kIOReturnNotReady;

    ivars->workQueue->DispatchSync(^{
        error = super::StartIO();
        if (error != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: StartIO failed 0x%x", error);
            super::StopIO();
        }
    });

    if (error == kIOReturnSuccess) {
        // Set outside the DispatchSync above: the sample does the same, and a
        // property write is not part of the I/O state transition the queue is
        // serialising.
        auto online = OSSharedPtr(OSNumber::withNumber(uint64_t{0}, 32), OSNoRetain);
        if (online) SetProperty(IOUserMIDIProperty::Offline, online.get());
        ASFW_LOG(Midi, "ASFWMidiDevice: IO started");
    }
    return error;
}

kern_return_t ASFWMidiDevice::StopIO() {
    __block kern_return_t error = kIOReturnSuccess;
    if (!ivars || !ivars->workQueue) return kIOReturnNotReady;

    auto offline = OSSharedPtr(OSNumber::withNumber(uint64_t{1}, 32), OSNoRetain);
    if (offline) SetProperty(IOUserMIDIProperty::Offline, offline.get());

    ivars->workQueue->DispatchSync(^{
        error = super::StopIO();
    });
    if (error != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: StopIO failed 0x%x", error);
    } else {
        ASFW_LOG(Midi, "ASFWMidiDevice: IO stopped");
    }
    return error;
}
