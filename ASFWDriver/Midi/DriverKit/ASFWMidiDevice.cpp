//
// ASFWMidiDevice.cpp
// ASFWDriver
//

#include <atomic>
#include <cstdio>
#include <new>
#include <span>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <MIDIDriverKit/MIDIDriverKit.h>

#include "ASFWMidiDevice.h"

#include "../../Logging/Logging.hpp"
#include "../Transport/MidiRxDrain.hpp"
#include "../Transport/MidiTransportBlock.hpp"
#include "../Transport/MidiTxIngress.hpp"

struct ASFWMidiDevice_IVars {
    OSSharedPtr<IOUserMIDIDriver> driver;
    OSSharedPtr<IODispatchQueue> workQueue;
    uint32_t sourcePorts{0};
    uint32_t destinationPorts{0};

    // The seam. `block` points into memory the endpoint owns and the driver
    // keeps mapped; `bound` is what the I/O blocks check, because an entity's
    // block can still be invoked after Stop begins.
    std::atomic<ASFW::Midi::MidiTransportBlock*> block{nullptr};
    std::atomic<bool> bound{false};
    std::atomic<int32_t> activeWriters{0};
    // CoreMIDI has this device open. Bytes that arrive while it is not are
    // discarded rather than queued for whoever opens it next.
    std::atomic<bool> ioRunning{false};

    // Host -> device, real-time thread. Per-port state, bounded, lock-free.
    ASFW::Midi::MidiTxIngress ingress{};
    // Device -> host, work queue only.
    ASFW::Midi::MidiRxDrain drain{};

    /// Real-time thread. Converts one destination's UMP to MIDI 1.0 bytes and
    /// enqueues them for the wire.
    kern_return_t WriteToWire(uint32_t port, const uint32_t* umpWords,
                              size_t numWords) noexcept {
        if (!bound.load(std::memory_order_acquire)) return kIOReturnNotReady;
        activeWriters.fetch_add(1, std::memory_order_acquire);
        struct WriterGuard {
            std::atomic<int32_t>& count;
            ~WriterGuard() { count.fetch_sub(1, std::memory_order_release); }
        } guard{activeWriters};
        // Re-check under the guard: UnbindTransport waits for the count to
        // reach zero after clearing `bound`, so a writer that got past the
        // first test is either counted (and waited for) or sees it cleared.
        if (!bound.load(std::memory_order_acquire)) return kIOReturnNotReady;

        auto* localBlock = block.load(std::memory_order_acquire);
        if (localBlock == nullptr || port >= ASFW::Midi::kMidiPortsPerDirection) {
            return kIOReturnBadArgument;
        }
        switch (ingress.Submit(*localBlock, port,
                               std::span<const uint32_t>(umpWords, numWords),
                               mach_absolute_time())) {
            case ASFW::Midi::MidiTxStatus::kOk:        return kIOReturnSuccess;
            case ASFW::Midi::MidiTxStatus::kNotActive: return kIOReturnNotReady;
            case ASFW::Midi::MidiTxStatus::kRingFull:  return kIOReturnNoSpace;
            case ASFW::Midi::MidiTxStatus::kTruncated: return kIOReturnBadArgument;
            case ASFW::Midi::MidiTxStatus::kBadPort:   return kIOReturnBadArgument;
        }
        return kIOReturnError;
    }
};

namespace {

/// "MIDI 1", "MIDI 2" ... one per physical jack pair.
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

    // IONewZero zero-fills; the members with constructors (atomics, the
    // converters) are brought up explicitly.
    new (&ivars->driver) OSSharedPtr<IOUserMIDIDriver>();
    new (&ivars->workQueue) OSSharedPtr<IODispatchQueue>();
    new (&ivars->block) std::atomic<ASFW::Midi::MidiTransportBlock*>(nullptr);
    new (&ivars->bound) std::atomic<bool>(false);
    new (&ivars->activeWriters) std::atomic<int32_t>(0);
    new (&ivars->ioRunning) std::atomic<bool>(false);
    new (&ivars->ingress) ASFW::Midi::MidiTxIngress();
    new (&ivars->drain) ASFW::Midi::MidiRxDrain();

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
        // MIDIProtocol_1_0: the wire carries a MIDI 1.0 byte stream, so 1.0 is
        // the honest declaration of what reaches the device. Translation to UMP
        // happens at the seam, and claiming 2.0 would promise resolution the
        // hardware cannot carry.
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

    // Offline until CoreMIDI opens the device; StartIO clears it.
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

void ASFWMidiDevice::BindTransport(void* block) {
    if (ivars == nullptr || block == nullptr) return;
    ivars->block.store(static_cast<ASFW::Midi::MidiTransportBlock*>(block),
                       std::memory_order_release);
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
    ASFW_LOG(Midi, "ASFWMidiDevice: transport bound");
}

bool ASFWMidiDevice::UnbindTransport() {
    if (ivars == nullptr) return true;

    // 1. Refuse new writes: an I/O block that already started sees `bound`
    //    cleared at its re-check or is counted below.
    ivars->bound.store(false, std::memory_order_release);

    // 2. Swap each destination's I/O block for one that captures nothing, so a
    //    block CoreMIDI invokes later never reaches these ivars. SetIOBlock
    //    rejects nullptr (kIOReturnBadArgument, seen on hot-unplug), so an
    //    inert block is the only way to detach.
    auto entities = GetEntities();
    if (entities) {
        entities->iterateObjects(^bool(OSObject* object) {
            auto* entity = OSDynamicCast(IOUserMIDIEntity, object);
            if (entity != nullptr) {
                auto destination = entity->GetDestination(0);
                if (destination) {
                    const kern_return_t ret = destination->SetIOBlock(
                        ^kern_return_t(IOUserMIDIUMPWord const*, size_t) {
                            return kIOReturnNotReady;
                        });
                    if (ret != kIOReturnSuccess) {
                        ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: detaching I/O block failed 0x%x", ret);
                    }
                }
            }
            return false;
        });
    }

    // 3. Wait for every in-flight real-time writer. A timeout is NOT quiescence:
    //    the caller keeps the mapping and this keeps the block pointer, because
    //    the writer may still be inside it.
    bool quiesced = false;
    for (uint32_t i = 0; i < 5000; ++i) {
        if (ivars->activeWriters.load(std::memory_order_acquire) == 0) {
            quiesced = true;
            break;
        }
        IODelay(100);
    }
    if (!quiesced) {
        ASFW_LOG_ERROR(Midi,
                       "ASFWMidiDevice: unbind timed out with %d active writer(s); "
                       "retaining the transport mapping",
                       ivars->activeWriters.load(std::memory_order_acquire));
        return false;
    }

    // 4. No writer can be inside the block any more.
    ivars->block.store(nullptr, std::memory_order_release);
    ivars->drain.Reset();
    ASFW_LOG(Midi, "ASFWMidiDevice: transport unbound and quiesced");
    return true;
}

void ASFWMidiDevice::DrainReceiveRings() {
    if (ivars == nullptr) return;
    if (!ivars->bound.load(std::memory_order_acquire)) return;
    auto* block = ivars->block.load(std::memory_order_acquire);
    if (block == nullptr) return;

    // A new stream moves the epoch: drop the old one's backlog and parser state.
    ivars->drain.ObserveEpoch(*block);

    if (!ivars->ioRunning.load(std::memory_order_acquire)) {
        // Nobody has the device open. Discard instead of queueing for the next
        // client, so opening a port never replays old bytes.
        for (auto& ring : block->deviceToHost) ring.DiscardBacklog();
        return;
    }

    auto entities = GetEntities();
    if (!entities) return;

    __block uint32_t portIndex = 0;
    auto* drain = &ivars->drain;
    entities->iterateObjects(^bool(OSObject* object) {
        auto* entity = OSDynamicCast(IOUserMIDIEntity, object);
        if (entity == nullptr) return false;
        const uint32_t port = portIndex++;
        if (port >= ASFW::Midi::kMidiPortsPerDirection) return true;
        auto source = entity->GetSource(0);
        if (!source) return false;
        drain->DrainPort(*block, port, [&](std::span<const ASFW::Midi::Ump::UmpWord> words) {
            (void)source->Send(words.data(), words.size());
        });
        return false;
    });
}

kern_return_t ASFWMidiDevice::StartIO() {
    __block kern_return_t error = kIOReturnSuccess;
    if (!ivars || !ivars->workQueue) return kIOReturnNotReady;

    // No stream is requested here. If the audio streams are not running, MIDI
    // is simply not flowing yet; the rings are refused/discarded until they do.
    ivars->workQueue->DispatchSync(^{
        // On the queue DrainReceiveRings runs on, so the parser state it owns is
        // never touched from two threads.
        ivars->drain.Reset();
        error = super::StartIO();
        if (error != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: StartIO failed 0x%x", error);
            super::StopIO();
        }
    });

    if (error == kIOReturnSuccess) {
        ivars->ioRunning.store(true, std::memory_order_release);
        auto online = OSSharedPtr(OSNumber::withNumber(uint64_t{0}, 32), OSNoRetain);
        if (online) SetProperty(IOUserMIDIProperty::Offline, online.get());
        ASFW_LOG(Midi, "ASFWMidiDevice: IO started");
    }
    return error;
}

kern_return_t ASFWMidiDevice::StopIO() {
    __block kern_return_t error = kIOReturnSuccess;
    if (!ivars || !ivars->workQueue) return kIOReturnNotReady;

    // Idempotent. Teardown reaches here twice: ASFWMIDIDriver::Stop stops the
    // device itself, then the framework's StopIO does it again, and a second
    // super::StopIO on a stopped device fails with kIOReturnError.
    if (!ivars->ioRunning.exchange(false, std::memory_order_acq_rel)) {
        return kIOReturnSuccess;
    }
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
