// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeStoppedPrefix.hpp"
#include "SustainedSession.hpp"
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <chrono>
#include <time.h>
#include <array>
#include <algorithm>
#ifdef PRO40_DMA_SOURCE_QUEUE
#include "DispatchBoundary.hpp"
#endif
#ifdef PRO40_DMA_TX_RETURN
#include "NativeTransmitVisits.hpp"
#include "EarlyTransmitGate.hpp"
#include "TransmitRefillArchive.hpp"
#if defined(PRO40_DMA_TX_LONG_REFILL) && !defined(PRO40_DMA_TX_REFILL)
#error Longer refill requires the measured refill profile.
#endif
#if defined(PRO40_DMA_TX_REFILL) && (!defined(PRO40_DMA_TX_FULL_HEAD) || !defined(PRO40_DMA_TX_MEASURED) || !defined(PRO40_DMA_TX_HANDOFFS))
#error Refill requires measured full-head handoffs.
#endif
#if defined(PRO40_DMA_TX_FULL_HEAD) && (!defined(PRO40_DMA_TX_MEASURED) || (defined(PRO40_DMA_TX_HANDOFFS) && !defined(PRO40_DMA_TX_REFILL)))
#error Full-head requires the measured single-return or refill profile.
#endif
#include "ReceiveBankHandoffs.hpp"
#if defined(PRO40_DMA_TX_HANDOFFS) && defined(PRO40_DMA_TX_REFRESH) && !defined(PRO40_DMA_TX_MEASURED)
#error Refreshed handoffs require persistent measured timing.
#endif
#include "NativePacketBuffer.hpp"
#include <optional>
#if !defined(PRO40_DMA_TX_PRIME) || defined(PRO40_DMA_DUPLEX_REPLAY) || (defined(PRO40_DMA_RX_REUSE) && !defined(PRO40_DMA_BIDIRECTIONAL_REUSE))
#error Transmit return requires the short dynamic transmit profile.
#endif
#endif
#if defined(PRO40_DMA_RX_REUSE) && (!defined(PRO40_DMA_RX_BRANCH_EXTEND) || !defined(PRO40_DMA_RX_BRANCH_STOP))
#error Receive reuse requires the finite branch-extension profile.
#endif
#if defined(PRO40_DMA_RX_DESCRIPTOR_REUSE) && ((!defined(PRO40_DMA_DUPLEX_REPLAY) && !defined(PRO40_DMA_BIDIRECTIONAL_REUSE)) || !defined(PRO40_DMA_RX_REUSE))
#error Descriptor reuse requires the long finite payload-reuse profile.
#endif
#if defined(PRO40_DMA_RX_HANDOFFS) && !defined(PRO40_DMA_RX_DESCRIPTOR_REUSE)
#error Repeated handoffs require the descriptor-reuse profile.
#endif
#if defined(PRO40_DMA_SOURCE_QUEUE) && !defined(PRO40_DMA_ONE_SECOND)
#error Source queue experiment requires the one-second compact profile.
#endif
#ifdef PRO40_DMA_SOURCE_QUEUE
#include "NativeSourceQueue.hpp"
#include "ReceiveProgress.hpp"
#endif
#if defined(PRO40_DMA_ONE_SECOND) && !defined(PRO40_DMA_COMPACT_NATIVE)
#error One-second budget requires compact native allocation.
#endif
#if defined(PRO40_DMA_COMPACT_NATIVE) && !defined(PRO40_DMA_EXTENDED_REUSE)
#error Compact native allocation requires the extended combined profile.
#endif
#if defined(PRO40_DMA_EXTENDED_REUSE) && !defined(PRO40_DMA_BIDIRECTIONAL_REUSE)
#error Extended budget requires the combined reuse profile.
#endif
#if defined(PRO40_DMA_BIDIRECTIONAL_REUSE) && (!defined(PRO40_DMA_RX_HANDOFFS) || !defined(PRO40_DMA_TX_LOCK))
#error Combined reuse requires receive handoffs and the bounded transmit lock profile.
#endif
#if defined(PRO40_DMA_DUPLEX_REPLAY) || defined(PRO40_DMA_RX_DESCRIPTOR_REUSE)
#include "NativeReceiveSequence.hpp"
#endif
#ifdef PRO40_DMA_RX_REUSE
#include "NativeReceiveArchive.hpp"
#include "ReceiveBankHandoffs.hpp"
#include <memory>
#include <new>
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
#include "NativePacketBuffer.hpp"
#include "NativeReceiveJournal.hpp"
#include <memory>
#include <new>
#endif
#ifdef PRO40_DMA_TX_TIMING
#include "Pro40Packets.hpp"
#endif

// Native transport harness; preparation build never starts. Lifecycle build
// uses empty transmit cycles and idle listeners. TX_TIMING sends bounded CIP
// no-data headers at a shared scheduled cycle. DUPLEX_CAPTURE/REPLAY are composed
// with an owned, enabled device; other profiles keep it idle. Sources:
// Apple IOFireWireLibIsochChannel.cpp:147-244; IOFWIsochChannel.cpp:892-930;
// IOFireWireLibIsochPort.cpp:953-1018; IOFWLocalIsochPort.cpp:50-83.
// The remote port is a host-side constraint adapter. Its callbacks make no
// device requests and do not claim to configure a real remote endpoint.
class DmaPreparation {
#ifdef PRO40_DMA_TEARDOWN_TEST
    friend struct DmaPreparationTest;
#endif
public:
#ifdef PRO40_DMA_TX_RETURN
#ifdef PRO40_DMA_SOURCE_QUEUE
    // Two256-packet TX banks leave24ms above the unchanged64-cycle cut guard.
    // The former128-packet banks left8ms; session-ykw3w7zx measured a10.417ms
    // generation query. Apple userNotify applies index/range lists independently
    // of bank size (IOFWUserIsochPort.cpp:1168-1245); Notify batches remain<=64.
    static constexpr unsigned transmitBankPackets=256;
#else
    static constexpr unsigned transmitBankPackets=128;
#endif
    static constexpr unsigned transmitSlots=2*transmitBankPackets;
#ifdef PRO40_DMA_SUSTAINED
#ifndef PRO40_DMA_SOURCE_QUEUE
#error Sustained mode requires fixed native storage and source queue.
#endif
    static constexpr unsigned transmitReturnCount=Pro40::sustainedCounterLimit;
#elif defined(PRO40_DMA_CYCLE_BUDGET)
#ifndef PRO40_DMA_SOURCE_QUEUE
#error Custom cycle budget requires the bounded source-queue profile.
#endif
    static constexpr unsigned transmitReturnCount=PRO40_DMA_CYCLE_BUDGET;
    static_assert(transmitReturnCount==32768 || transmitReturnCount==65536 || transmitReturnCount==262144, "Only bounded long experiment geometries are enabled");
#elif defined(PRO40_DMA_ONE_SECOND)
    static constexpr unsigned transmitReturnCount=8192;
#elif defined(PRO40_DMA_EXTENDED_REUSE)
    static constexpr unsigned transmitReturnCount=4096;
#elif defined(PRO40_DMA_TX_LONG_REFILL)
    static constexpr unsigned transmitReturnCount=1024;
#elif defined(PRO40_DMA_TX_REFILL)
    static constexpr unsigned transmitReturnCount=640;
#elif defined(PRO40_DMA_TX_HANDOFFS)
    static constexpr unsigned transmitReturnCount=512;
#else
    static constexpr unsigned transmitReturnCount=384;
#endif
#ifdef PRO40_DMA_TX_FULL_HEAD
    static constexpr unsigned firstRefreshCycle=transmitSlots;
#else
    static constexpr unsigned firstRefreshCycle=transmitSlots+1;
#endif
    // Reuse the payload-opaque cut-before-link policy already tested for RX.
    using TransmitBankPolicy=NativePackets::ReceiveBankHandoffs<transmitBankPackets,transmitReturnCount>;
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
    static constexpr unsigned descriptorCount = 9216; // Maximum finite RX program.
    static constexpr unsigned replayTransmitCount = 8192;
    static_assert(descriptorCount%64==0 && replayTransmitCount%64==0);
#elif defined(PRO40_DMA_TX_LONG_REFILL)
    static constexpr unsigned descriptorCount=transmitReturnCount;
#elif defined(PRO40_DMA_TX_FLOW_UPDATE) || defined(PRO40_DMA_DUPLEX_CAPTURE)
    static constexpr unsigned descriptorCount = 512;
#else
    static constexpr unsigned descriptorCount = 8;
#endif
#ifdef PRO40_DMA_RX_BRANCH_STOP
#ifdef PRO40_DMA_DUPLEX_REPLAY
#ifdef PRO40_DMA_RX_HANDOFFS
    static constexpr unsigned receiveGate=2048,receiveCopyCount=1024;
#else
    static constexpr unsigned receiveGate=6144,receiveCopyCount=3072;
#endif
#elif defined(PRO40_DMA_SOURCE_QUEUE)
    // Copy callbacks still publish every64 packets; TX consumes one whole bank.
    // Larger RX banks only extend the deadline for relinking owned storage.
    // The 128-packet bank left 8ms above the 64-cycle guard: live-playback001
    // exhausted it after a 5ms timer query plus status/dispatch work.
    // Apple IOFireWireLibIsochPort.cpp:1524-1568 exports branch pairs by DCL
    // index, independently of bank size. Keep cut-before-link and lead guards.
    static constexpr unsigned receiveGate=512,receiveCopyCount=256;
#elif defined(PRO40_DMA_BIDIRECTIONAL_REUSE)
    static constexpr unsigned receiveGate=256,receiveCopyCount=128;
#else
    static constexpr unsigned receiveGate=256,receiveCopyCount=64;
#endif
    static_assert(receiveCopyCount<receiveGate && descriptorCount-receiveCopyCount>=receiveGate);
#ifdef PRO40_DMA_RX_HANDOFFS
    using ReceiveBankPolicy=NativePackets::ReceiveBankHandoffs<receiveCopyCount,descriptorCount>;
#ifdef PRO40_DMA_COMPACT_NATIVE
    static_assert(receiveGate==2*receiveCopyCount && receiveGate>=transmitSlots && firstRefreshCycle==transmitSlots,
        "Compact native allocation must contain both complete RX/TX banks");
#endif
#endif
#endif
private:
    static constexpr NativePackets::CompletionLayout completionLayout(bool talking) {
#ifdef PRO40_DUPLEX_WORKER
        // Apple IOFWDCL.cpp:366-399 imports arbitrary explicit update targets;
        // IOFWUserIsochPort.cpp:950-954 sends one async message per callback.
        // Preserve bank+1 readiness and terminal callbacks while reducing TX
        // notifications from256 to5 per bank. Every packet is still validated.
        if(talking)return {64,transmitBankPackets};
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
        (void)talking;
        return {64};
#elif defined(PRO40_DMA_RX_DESCRIPTOR_REUSE)
        return {talking?1u:64u};
#else
        (void)talking;
        return {1};
#endif
    }
#ifdef PRO40_DMA_COMPACT_NATIVE
    static constexpr unsigned storageCount = receiveGate;
#else
    static constexpr unsigned storageCount = descriptorCount;
#endif
    static constexpr size_t metadataOffset = storageCount*512;
    static constexpr size_t replacementOffset = (metadataOffset+storageCount*8+4095)/4096*4096;
    IOFireWireLibNuDCLPoolRef pool_ = nullptr;
    IOFireWireLibLocalIsochPortRef local_ = nullptr;
    IOFireWireLibRemoteIsochPortRef constraint_ = nullptr;
    IOFireWireLibIsochChannelRef channel_ = nullptr;
    void *buffer_ = nullptr;
    size_t bufferBytes_ = 0;
    FILE *log_ = nullptr;
    unsigned index_ = 0, busChannel_ = 0;
    unsigned programCount_ = descriptorCount;
#ifdef PRO40_DMA_RX_BRANCH_STOP
    NuDCLRef receiveStopDcl_=nullptr;
#ifdef PRO40_DMA_RX_BRANCH_EXTEND
    NuDCLRef receiveNextDcl_=nullptr;
#ifndef PRO40_DMA_RX_HANDOFFS
    bool receiveExtensionAttempted_=false;
#endif
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
    NuDCLRef receiveReturnStopDcl_=nullptr;
#ifdef PRO40_DMA_RX_HANDOFFS
    std::array<NuDCLRef,2> receiveHeads_{},receiveTails_{};
    ReceiveBankPolicy handoffs_;
#endif
#endif
#endif
#endif
    bool allocationAttempted_ = false;
    bool callbackStorageExposed_ = false;
#ifdef PRO40_DMA_TX_TIMING
    uint32_t transmitStart_ = 0;
#endif
#ifdef PRO40_DMA_TX_PRIME
    std::array<NuDCLRef,storageCount> transmitDcls_{};
    uint8_t sender_ = 0;
#endif
#ifdef PRO40_DMA_LIFECYCLE
    bool started_ = false, finalized_ = false;
    bool needsFinalize_ = false;
    bool callbacksClosed_ = false;
    unsigned lateCallbacks_ = 0;
#ifdef PRO40_DMA_DUPLEX_CAPTURE
#if defined(PRO40_DMA_RX_REUSE) || defined(PRO40_DMA_TX_RETURN)
    bool retainHistory_=true;
#endif
    FILE *capture_ = nullptr; // Caller retains the file until release finishes.
#endif
    unsigned callbacks_ = 0;
    unsigned nativeCallbacks_ = 0;
#ifdef PRO40_DMA_SOURCE_QUEUE
    NativePackets::DispatchBoundary dispatchBoundary_;
#endif
#ifdef PRO40_DMA_TX_RETURN
    std::optional<NativePackets::TransmitProgress> transmitProgress_;
    std::unique_ptr<NativePackets::TransmitVisits> transmitVisits_; // Diagnostic only.
    TransmitBankPolicy transmitHandoffs_;
#ifdef PRO40_DMA_TX_REFILL
    // Finite diagnostic history, copied from the mapped bytes after Notify.
    std::unique_ptr<NativePackets::TransmitRefillArchive<transmitReturnCount-transmitSlots>> refillArchive_;
#endif
#endif
#ifdef PRO40_DMA_RX_REUSE
    std::unique_ptr<NativePackets::ReceiveArchive<descriptorCount,receiveCopyCount>> receiveArchive_;
    FILE *finalCapture_=nullptr;
#ifdef PRO40_DMA_SOURCE_QUEUE
    std::unique_ptr<NativePackets::SourceQueue> sourceQueue_;
    std::optional<NativePackets::ReceiveProgress> receiveProgress_;
#endif
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
    std::unique_ptr<NativePackets::ReceiveJournal<32>> receiveJournal_;
#endif
#if defined(PRO40_DMA_DUPLEX_REPLAY) || defined(PRO40_DMA_RX_DESCRIPTOR_REUSE)
    uint32_t captureGeneration_=0;
    std::optional<NativePackets::ReceiveSequence> receiveSequence_;
#endif
    struct Metadata {
        DmaPreparation *owner = nullptr;
        UInt32 *timestamp = nullptr, *status = nullptr;
        uint32_t observedTimestamp = 0, observedStatus = 0;
        unsigned completions = 0;
        unsigned index=0,groupCount=1;
    };
    std::array<Metadata, storageCount> metadata_{};
    static void completed(void *refcon, NuDCLRef) {
        // IOFWUserIsochPort.cpp:950-954 sends no DCL argument through IOKit.
        // Identify the descriptor using its own stable refcon, not argument 2.
        auto &entry = *static_cast<Metadata *>(refcon);
        auto &owner=*entry.owner;
        // The finalize token is the documented end of this port's callbacks
        // (Apple IOFireWireLibIsoch.h:386-398). The 2026-10-10 timeout trace
        // nevertheless delivered a TX completion after that boundary. Keep
        // this refcon alive until the native client closes, reject the stale
        // completion before touching DMA memory, and report a cleanup fault.
        if(owner.callbacksClosed_) {
            if(owner.lateCallbacks_!=UINT32_MAX)++owner.lateCallbacks_;
            return;
        }
        ++owner.nativeCallbacks_;
#if defined(PRO40_DMA_DUPLEX_REPLAY) || defined(PRO40_DMA_RX_DESCRIPTOR_REUSE)
        std::optional<uint64_t> receiveFirst;
        if(owner.receiveSequence_) {
            receiveFirst=owner.receiveSequence_->accept(entry.index+1-entry.groupCount,owner.captureGeneration_);
            if(!receiveFirst)return;
        }
#endif
        // The endpoint's explicit native update list has refreshed every slot
        // before this callback. Count observations, not assumed successful I/O:
        // each stored status/timestamp is still validated by capture and replay.
        for(unsigned i=entry.index+1-entry.groupCount;i<=entry.index;++i) {
            auto &slot=owner.metadata_[i];
            ++owner.callbacks_;++slot.completions;
            slot.observedTimestamp=*slot.timestamp;
            slot.observedStatus=*slot.status;
#ifdef PRO40_DMA_TX_RETURN
            if(owner.transmitProgress_)
                (void)owner.transmitProgress_->accept(i,slot.observedStatus,slot.observedTimestamp);
            if(owner.transmitVisits_)
                (void)owner.transmitVisits_->accept(i,slot.observedStatus,slot.observedTimestamp);
#endif
#ifdef PRO40_DMA_RX_REUSE
            if(owner.receiveArchive_) {
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
                const auto logical=unsigned(*receiveFirst)+i-(entry.index+1-entry.groupCount);
#else
                const auto logical=i;
#endif
                (void)owner.receiveArchive_->append(logical,{
                    std::span(static_cast<const uint8_t *>(owner.buffer_)+NativePackets::reusedSlot(i,descriptorCount,receiveCopyCount)*512,448),
                    slot.observedStatus,slot.observedTimestamp});
            }
#endif
        }
#ifdef PRO40_DMA_SOURCE_QUEUE
        if(owner.sourceQueue_) {
            const auto stableSource=[&](uint64_t n) {
                const auto physical=unsigned(n)%receiveGate;
                const auto &slot=owner.metadata_[physical];
                return NativePackets::CompletedBuffer{
                    std::span(static_cast<const uint8_t *>(owner.buffer_)+physical*512,448),
                    *slot.status,*slot.timestamp};
            };
            if(owner.sourceQueue_->append(*receiveFirst,owner.captureGeneration_,stableSource))
                (void)owner.receiveProgress_->publish(*receiveFirst,owner.captureGeneration_,stableSource);
        }
        // Apple IOFWUserIsochPort.cpp:940-954 delivers native async callbacks.
        // Finish copying/validating this callback before yielding to bank work.
        // Yield TX only at the existing readiness boundary+1. Its independent
        // timer may still be early; EarlyTransmitGate keeps all native guards.
        if(owner.index_<2)owner.dispatchBoundary_.completed(owner.callbacks_);
        else owner.dispatchBoundary_.transmitCompleted(owner.callbacks_,transmitBankPackets);
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
        if(owner.receiveJournal_) {
            (void)owner.receiveJournal_->append(*receiveFirst,owner.captureGeneration_,
                [&](uint64_t n) {
#ifdef PRO40_DMA_RX_REUSE
#ifdef PRO40_DMA_RX_HANDOFFS
                    const auto physical=unsigned(n)%receiveGate;
#else
                    const auto physical=NativePackets::reusedSlot(unsigned(n),descriptorCount,receiveCopyCount);
#endif
#else
                    const auto physical=n%owner.programCount_;
#endif
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
                    const auto &slot=owner.metadata_[physical];
#else
                    const auto &slot=owner.metadata_[size_t(n)];
#endif
                    return NativePackets::CompletedBuffer{
                        std::span(static_cast<const uint8_t *>(owner.buffer_)+physical*512,448),
                        *slot.status,*slot.timestamp};
                });
        }
#endif
    }
    static IOReturn finalized(void *refcon) {
        auto &owner=*static_cast<DmaPreparation *>(refcon);
        owner.callbacksClosed_ = owner.finalized_ = true;
        return 0;
    }
#endif
    void event(const char *name, unsigned result = 0) {
        std::fprintf(log_, "dma %u %s: 0x%08x\n", index_, name, result);
        std::fflush(log_); fsync(fileno(log_));
    }
    static IOReturn supported(IOFireWireLibIsochPortRef port, IOFWSpeed *speed, UInt64 *mask) {
        auto *self = static_cast<DmaPreparation *>((*port)->GetRefCon(port));
        *speed = kFWSpeed400MBit;
        *mask = 1ULL << (63 - self->busChannel_);
        return 0;
    }
    static IOReturn allocated(IOFireWireLibIsochPortRef port, IOFWSpeed speed, UInt32 channel) {
        auto *self = static_cast<DmaPreparation *>((*port)->GetRefCon(port));
        return speed == kFWSpeed400MBit && channel == self->busChannel_ ? 0 : kIOReturnError;
    }
public:
    DmaPreparation() = default;
    DmaPreparation(const DmaPreparation &) = delete;
    DmaPreparation &operator=(const DmaPreparation &) = delete;
    ~DmaPreparation() {
        // All callers must close the native client before retiring exposed
        // callback storage. Never silently unmap it during an early unwind.
        if(callbackStorageExposed_)std::_Exit(3);
        (void)release();
        (void)reclaimAfterNativeClose();
    }
    IOReturn prepare(IOFireWireLibDeviceRef device, FILE *log, unsigned index, unsigned busChannel,
                    uint32_t transmitStart = 0, uint8_t sender = 0,uint32_t generation=0) {
        log_ = log; index_ = index; busChannel_ = busChannel;
#ifdef PRO40_DMA_SOURCE_QUEUE
        if(retainHistory_ && transmitReturnCount>8192)return kIOReturnUnsupported;
#endif
        (void)generation;
#ifdef PRO40_DMA_TX_TIMING
        transmitStart_ = transmitStart;
#ifdef PRO40_DMA_TX_PRIME
        sender_ = sender;
#endif
#else
        (void)transmitStart; (void)sender;
#endif
        const bool talking = index >= 2;
#ifdef PRO40_DMA_COMPACT_NATIVE
        programCount_=talking?transmitSlots:receiveGate; // Independent RX/TX geometry.
        std::fprintf(log_,"dma %u native-program descriptors=%u records=%u\n",index_,programCount_,descriptorCount);
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
        // Receive for 1152 ms, covering the 1024 ms finite transmission
        // and its aftermath. Descriptor metadata stays distinct; the optional
        // reuse profile aliases payload ranges only behind an initially closed gate.
        programCount_ = talking ? replayTransmitCount : descriptorCount;
#endif
#if defined(PRO40_DMA_DUPLEX_REPLAY) || defined(PRO40_DMA_RX_DESCRIPTOR_REUSE)
        captureGeneration_=generation;
        if(!talking) {
            // Count logical records separately from the optional physical return.
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
            receiveSequence_.emplace(receiveGate,(descriptorCount+receiveGate-1)/receiveGate,generation,descriptorCount);
#else
            receiveSequence_.emplace(programCount_,1,generation);
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
            receiveJournal_.reset(new(std::nothrow) NativePackets::ReceiveJournal<32>(generation));
            if(!receiveJournal_)return kIOReturnNoMemory;
#endif
        }
#endif
#ifdef PRO40_DMA_RX_REUSE
        if(!talking) {
            if(retainHistory_) {
                receiveArchive_.reset(new(std::nothrow) NativePackets::ReceiveArchive<descriptorCount,receiveCopyCount>);
                if(!receiveArchive_)return kIOReturnNoMemory;
            }
#ifdef PRO40_DMA_SOURCE_QUEUE
            sourceQueue_.reset(new(std::nothrow) NativePackets::SourceQueue(generation));
            if(!sourceQueue_)return kIOReturnNoMemory;
            receiveProgress_.emplace(generation);
#endif
        }
#endif
#ifdef PRO40_DMA_TX_RETURN
        if(talking) {
            transmitProgress_.emplace(transmitStart,transmitReturnCount,transmitSlots);
            if(retainHistory_) {
                transmitVisits_.reset(new(std::nothrow) NativePackets::TransmitVisits(transmitStart,transmitReturnCount,transmitSlots));
                if(!transmitVisits_)return kIOReturnNoMemory;
#ifdef PRO40_DMA_TX_REFILL
                refillArchive_.reset(new(std::nothrow) NativePackets::TransmitRefillArchive<transmitReturnCount-transmitSlots>);
                if(!refillArchive_)return kIOReturnNoMemory;
#endif
            }
        }
#endif
#ifdef PRO40_DMA_SOURCE_QUEUE
        const size_t historyBytes=(receiveArchive_?sizeof(*receiveArchive_):0)+
            (transmitVisits_?sizeof(*transmitVisits_):0)+(refillArchive_?sizeof(*refillArchive_):0);
        std::fprintf(log_,"dma %u diagnostic-history enabled=%u bytes=%zu\n",index_,unsigned(retainHistory_),historyBytes);
#endif
        // Packet banks and completion metadata share one exported mmap. Both
        // banks stay alive until finalization, including for larger finite runs.
#if defined(PRO40_DMA_TX_ARMED_UPDATE) || defined(PRO40_DMA_TX_FLOW_UPDATE) || defined(PRO40_DMA_DUPLEX_REPLAY) || defined(PRO40_DMA_TX_RETURN)
        constexpr size_t required = replacementOffset+storageCount*512;
#else
        constexpr size_t required = metadataOffset+storageCount*8;
#endif
#ifdef PRO40_DMA_TX_RETURN
        static_assert(required>=replacementOffset+transmitSlots*512,
            "Transmit return must export storage for every replacement slot");
#endif
        const size_t page = size_t(getpagesize());
        const size_t bytes = (required+page-1)/page*page;
        if(programCount_>storageCount)return kIOReturnNoSpace;
#ifdef PRO40_DMA_COMPACT_NATIVE
        // Apple IOFWUserIsochPort.cpp:1193-1219 imports updated ranges into
        // this exported mapping. Include both packet banks and all metadata.
        std::fprintf(log_,"dma %u native-storage slots=%u metadata-offset=%zu replacement-offset=%zu required=%zu mapped=%zu page=%zu\n",
            index_,storageCount,metadataOffset,replacementOffset,required,bytes,page);
#endif
        // IOFireWireLibIsoch.h local-port contract requires vm_allocate or mmap,
        // not merely an aligned malloc allocation, even though preparation passed.
        buffer_ = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (buffer_ == MAP_FAILED) { buffer_ = nullptr; return kIOReturnNoMemory; }
        bufferBytes_ = bytes;
        std::memset(buffer_, 0, bytes);
        event("enter-pool");
        pool_ = (*device)->CreateNuDCLPool(device, programCount_, CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
        event("leave-pool", pool_ ? 0 : unsigned(kIOReturnNoMemory));
        if (!pool_) return kIOReturnNoMemory;
#ifdef PRO40_DMA_LIFECYCLE
        std::array<NuDCLRef,64> completionGroup{};
        const auto completions=completionLayout(talking);
        if(!completions.valid() || !completions.endingAt(programCount_-1))return kIOReturnBadArgument;
#endif
        for (unsigned n = 0; n < programCount_; ++n) {
            // Fixed bounded ranges; TX_TIMING narrows outgoing ranges to the
            // exact eight-byte no-data packet built below.
            IOVirtualRange range{reinterpret_cast<IOVirtualAddress>(buffer_) + n * 512, 448};
#ifdef PRO40_DMA_RX_REUSE
            if(!talking)range.address=reinterpret_cast<IOVirtualAddress>(buffer_)+NativePackets::reusedSlot(n,descriptorCount,receiveCopyCount)*512;
#endif
#ifdef PRO40_DMA_TX_TIMING
            if (talking) {
                Pro40Packets::Am824Stream content(Pro40Packets::playback[index-2],sender);
                const auto size = content.make(std::span(static_cast<uint8_t *>(buffer_)+n*512,448),0,0xffff);
                if (!size) return kIOReturnError;
                range.length = *size;
            }
            auto dcl = talking ? (*pool_)->AllocateSendPacket(pool_, nullptr, 1, &range)
                : (*pool_)->AllocateReceivePacket(pool_, nullptr, 8, 1, &range);
#elif defined(PRO40_DMA_LIFECYCLE)
            auto dcl = talking ? (*pool_)->AllocateSkipCycle(pool_)
                : (*pool_)->AllocateReceivePacket(pool_, nullptr, 8, 1, &range);
#else
            auto dcl = talking ? (*pool_)->AllocateSendPacket(pool_, nullptr, 1, &range)
                : (*pool_)->AllocateReceivePacket(pool_, nullptr, 8, 1, &range);
#endif
            if (!dcl) return kIOReturnNoMemory;
#ifdef PRO40_DMA_RX_BRANCH_STOP
#ifdef PRO40_DMA_RX_HANDOFFS
            if(!talking && n<receiveGate) {
                if(n%receiveCopyCount==0)receiveHeads_[n/receiveCopyCount]=dcl;
                if(n%receiveCopyCount==receiveCopyCount-1) {
                    receiveTails_[n/receiveCopyCount]=dcl;
                    (*pool_)->SetDCLFlags(dcl,(*pool_)->GetDCLFlags(dcl)|kNuDCLDynamic);
                }
            }
#endif
#ifdef PRO40_DMA_RX_BRANCH_EXTEND
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
            if(!talking && n==0)receiveNextDcl_=dcl;
            if(!talking && n==receiveCopyCount-1) {
                receiveReturnStopDcl_=dcl;
                (*pool_)->SetDCLFlags(dcl,(*pool_)->GetDCLFlags(dcl)|kNuDCLDynamic);
            }
#else
            if(!talking && n==receiveGate)receiveNextDcl_=dcl;
#endif
#endif
            if(!talking && n==receiveGate-1) {
                receiveStopDcl_=dcl;
                (*pool_)->SetDCLFlags(dcl,(*pool_)->GetDCLFlags(dcl)|kNuDCLDynamic);
            }
#endif
#ifdef PRO40_DMA_TX_PRIME
            if (talking) {
                transmitDcls_[n] = dcl;
                // IOFireWireFamilyCommon.h:1073: dynamic send descriptors permit
                // modification. Installed OHCI notify rejects static SendDCLs
                // at 0xfffffe000aecc33c-344 (kIOReturnNotPermitted).
                (*pool_)->SetDCLFlags(dcl,(*pool_)->GetDCLFlags(dcl)|kNuDCLDynamic);
            }
#endif
#ifdef PRO40_DMA_TX_TIMING
            if (talking) {
                auto tagStatus = (*pool_)->SetDCLTagBits(dcl,1);
                if (!tagStatus) tagStatus = (*pool_)->SetDCLSyncBits(dcl,0);
                if (tagStatus) return tagStatus;
            }
#endif
#ifdef PRO40_DMA_LIFECYCLE
            auto &entry = metadata_[n];
            entry.owner = this;entry.index=n;
            entry.timestamp = reinterpret_cast<UInt32 *>(static_cast<uint8_t *>(buffer_) + metadataOffset + n * 8);
            entry.status = entry.timestamp + 1;
            *entry.timestamp = *entry.status = 0xdeadbeef;
            auto status = (*pool_)->SetDCLTimeStampPtr(dcl, entry.timestamp);
            if (!status) status = (*pool_)->SetDCLStatusPtr(dcl, entry.status);
            // The flag orders the update list; it does not add this DCL to it.
            // IOFWDCL.cpp:366-399 imports the explicit list of update targets.
            // Include only this notification's slots, including its own DCL.
            // The first TX group is1+63, preserving bank+1 readiness. Reused
            // metadata/payloads remain owned until bank completion and archival.
            completionGroup[n%64]=dcl;
            entry.groupCount=completions.endingAt(n);
            const bool notifyCompletion=entry.groupCount!=0;
            if(notifyCompletion) {
                for(unsigned i=n%64+1-entry.groupCount;i<=n%64;++i)
                    if(!status)status=(*pool_)->AppendDCLUpdateList(dcl,completionGroup[i]);
                (*pool_)->SetDCLFlags(dcl, (*pool_)->GetDCLFlags(dcl) | kNuDCLUpdateBeforeCallback);
                (*pool_)->SetDCLRefcon(dcl, &entry);
                if (!status) status = (*pool_)->SetDCLCallback(dcl, completed);
            }
            if (!status && n+1 == programCount_) status = (*pool_)->SetDCLBranch(dcl, nullptr);
            if (status) return status;
#endif
        }
        IOVirtualRange range{reinterpret_cast<IOVirtualAddress>(buffer_), bytes};
        event("enter-local-port");
#ifdef PRO40_DMA_TX_TIMING
        // IOFireWireBus.h:228-240: transmit cycle event uses the raw cycle
        // timer and mask. Match low two seconds bits plus cycle count.
        const UInt32 startEvent = talking ? kFWDCLCycleEvent : kFWDCLImmediateEvent;
        const UInt32 startState = talking ? transmitStart : 0;
        const UInt32 startMask = talking ? 0x07fff000 : 0;
#else
        const UInt32 startEvent = 0, startState = 0, startMask = 0;
#endif
#ifdef PRO40_DMA_TX_PRIME
        const auto portInterface = kIOFireWireLocalIsochPortInterfaceID_v5;
#else
        const auto portInterface = kIOFireWireLocalIsochPortInterfaceID_v4;
#endif
        local_ = (*device)->CreateLocalIsochPort(device, talking, (*pool_)->GetProgram(pool_),
            startEvent, startState, startMask, nullptr, 0, &range, 1, CFUUIDGetUUIDBytes(portInterface));
        event("leave-local-port", local_ ? 0 : unsigned(kIOReturnError));
        if (!local_) return kIOReturnError;
        callbackStorageExposed_ = true;
#ifdef PRO40_DMA_LIFECYCLE
        auto callbackPort = reinterpret_cast<IOFireWireLibIsochPortRef>(local_);
        (*callbackPort)->SetRefCon(callbackPort, this);
        const auto callbackStatus = (*local_)->SetFinalizeCallback(local_, finalized);
        if (callbackStatus) return callbackStatus;
#endif
        constraint_ = (*device)->CreateRemoteIsochPort(device, !talking,
            CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
        if (!constraint_) return kIOReturnNoMemory;
        auto constraintPort = reinterpret_cast<IOFireWireLibIsochPortRef>(constraint_);
        (*constraintPort)->SetRefCon(constraintPort, this);
        (*constraint_)->SetGetSupportedHandler(constraint_, supported);
        (*constraint_)->SetAllocatePortHandler(constraint_, allocated);
        event("enter-channel-object");
        // IRM already reserved by GenerationReservation. The constraint limits
        // channel selection to that reservation; no second IRM owner is created.
        channel_ = (*device)->CreateIsochChannel(device, false, 448, kFWSpeed400MBit,
            CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
        event("leave-channel-object", channel_ ? 0 : unsigned(kIOReturnNoMemory));
        if (!channel_) return kIOReturnNoMemory;
        auto localPort = reinterpret_cast<IOFireWireLibIsochPortRef>(local_);
        auto status = (*channel_)->SetTalker(channel_, talking ? localPort : constraintPort);
        if (!status) status = (*channel_)->AddListener(channel_, talking ? constraintPort : localPort);
        if (status) return status;
        event("enter-allocate-channel");
        allocationAttempted_ = true; // Release even after a partially failed allocation.
        status = (*channel_)->AllocateChannel(channel_);
        event("leave-allocate-channel", unsigned(status));
#ifdef PRO40_DMA_TX_RETURN
        if(!status && talking) {
            status=(*pool_)->SetDCLBranch(transmitDcls_[transmitSlots-1],nullptr);
            if(!status) {
                void *changed[]={transmitDcls_[transmitSlots-1]};
                status=(*local_)->Notify(local_,kFWNuDCLModifyJumpNotification,changed,1);
            }
            char label[64];std::snprintf(label,sizeof(label),"transmit-branch-stop-%u",transmitSlots);
            event(label,unsigned(status));
        }
#endif
#ifdef PRO40_DMA_RX_BRANCH_STOP
        if(!status && !talking) {
            // Apple IOFireWireLibIsochPort.cpp:1515-1575 exports a list of
            // modified DCLs as index/target pairs. IOFWUserIsochPort.cpp:
            // 1235-1305 preserves an explicit null target for ModifyJump.
            // Do this after native allocation and before Start: initial null
            // fields must not be assumed to stop an interior descriptor.
            status=(*pool_)->SetDCLBranch(receiveStopDcl_,nullptr);
            if(!status) {
                void *changed[]={receiveStopDcl_};
                status=(*local_)->Notify(local_,kFWNuDCLModifyJumpNotification,changed,1);
            }
            char label[64];std::snprintf(label,sizeof(label),"receive-branch-stop-%u",receiveGate);
            event(label,unsigned(status));
        }
#endif
        return status;
    }
#ifdef PRO40_DMA_LIFECYCLE
#ifdef PRO40_DMA_TX_RETURN
    // Copy owned history when RX is reused; otherwise copy a completed unique
    // DMA slot. Never lend live mapped storage to content code.
    IOReturn copyCompletedReceive(unsigned n,NativePackets::PreparedBuffer &copy,
                                  uint32_t &status,uint32_t &timestamp) {
        if(index_>=2 || !started_ || n>=transmitReturnCount-firstRefreshCycle)
            return kIOReturnNotReady;
#ifdef PRO40_DMA_RX_REUSE
        if(reuseFault())return kIOReturnError;
#ifdef PRO40_DMA_SOURCE_QUEUE
        return sourceQueue_->copy(n,captureGeneration_,copy,status,timestamp)?0:kIOReturnError;
#else
        const auto record=receiveArchive_->view(n);
        if(record.bytes.size()!=448)return kIOReturnNotReady;
        std::copy(record.bytes.begin(),record.bytes.end(),copy.bytes.begin());
        copy.length=448;status=record.status;timestamp=record.timestamp;
#endif
#else
        if(metadata_[n].completions!=1)return kIOReturnNotReady;
        std::memcpy(copy.bytes.data(),static_cast<const uint8_t *>(buffer_)+n*512,448);
        copy.length=448;status=metadata_[n].observedStatus;
        timestamp=metadata_[n].observedTimestamp;
#endif
        return 0;
    }
#ifdef PRO40_DMA_SOURCE_QUEUE
    IOReturn enableSourceObserver() {
        if(index_>=2 || started_ || !sourceQueue_)return kIOReturnNotPermitted;
        return sourceQueue_->enableObserver()?0:kIOReturnError;
    }
    uint64_t availableSourcePackets() const {return sourceQueue_?sourceQueue_->next():0;}
    IOReturn observeCompletedReceive(uint64_t n,NativePackets::PreparedBuffer &copy,
                                    uint32_t &status,uint32_t &timestamp) {
        if(index_>=2 || !started_ || !sourceQueue_ || reuseFault())return kIOReturnNotReady;
        return sourceQueue_->observe(n,captureGeneration_,copy,status,timestamp)?0:kIOReturnError;
    }
    IOReturn acknowledgeSource(unsigned first) {
        if(index_>=2 || !sourceQueue_)return kIOReturnNotPermitted;
        return sourceQueue_->acknowledge(first,captureGeneration_,transmitBankPackets)?0:kIOReturnError;
    }
#endif
    struct TransmitReturn {
        unsigned callbacks=0,before=0,cutLead=0,after=0,earlyReads=0;
        NativePackets::EarlyTransmitGate::Result earlyResult=NativePackets::EarlyTransmitGate::Result::unexamined;
        bool earlyRevalidated=false;
        uint64_t earlyMicros=0;
        unsigned refreshed=0;IOReturn refreshResult=kIOReturnNotReady;
        uint64_t micros=0;
        IOReturn cutResult=kIOReturnNotReady,result=kIOReturnNotReady;
        // Scalar timing only, emitted after Stop. Separates user-space work,
        // synchronous native Modify/Jump, and timer calls; no hot-path IO.
        std::array<uint64_t,10> phaseMicros{},phaseCpuNS{};
        std::array<unsigned,10> phaseCalls{};
    };
    void logRefreshedPackets(std::span<const NativePackets::PreparedBuffer> packets) const {
#ifdef PRO40_DMA_TX_REFILL
        (void)packets;
        const auto emit=[&](const char *kind,unsigned n,const uint8_t *bytes,unsigned length) {
            std::fprintf(log_,"%s port=%u descriptor=%u bytes=%u hex=",kind,index_,n,length);
            for(unsigned j=0;j<length;++j)std::fprintf(log_,"%02x",bytes[j]);
            std::fputc('\n',log_);
        };
        if(!refillArchive_)return;
        const auto records=refillArchive_->records();
        const auto refillRecords_=unsigned(records.size());
        for(unsigned n=0;n<refillRecords_;++n)
            emit("transmit-refreshed",n,records[n].bytes.data(),records[n].length);
        for(unsigned n=0;n<transmitSlots;++n) {
            if(n>=refillRecords_)continue;
            const unsigned last=n+((refillRecords_-1-n)/transmitSlots)*transmitSlots;
            if(last<refillRecords_)emit("transmit-final-range",n,
                static_cast<const uint8_t *>(buffer_)+replacementOffset+n*512,records[last].length);
        }
#else
        constexpr unsigned offset=firstRefreshCycle-transmitSlots;
        if(packets.size()>transmitSlots-offset || replacementOffset+transmitSlots*512>bufferBytes_)return;
        for(unsigned i=0;i<packets.size();++i) {
            const auto *bytes=static_cast<const uint8_t *>(buffer_)+replacementOffset+(i+offset)*512;
            std::fprintf(log_,"transmit-refreshed port=%u descriptor=%u bytes=%u hex=",index_,i+offset,packets[i].length);
            for(unsigned j=0;j<packets[i].length;++j)std::fprintf(log_,"%02x",bytes[j]);
            std::fputc('\n',log_);
        }
#endif
    }
    bool transmitVisitFault() const {return transmitProgress_ && transmitProgress_->fault();}
    template<class Guard>
    IOReturn returnTransmitBank(IOFireWireLibDeviceRef device,TransmitReturn &sample,Guard guard,
        std::span<const NativePackets::PreparedBuffer> packets={}) {
        if(index_<2 || !started_ || !local_ || transmitHandoffs_.fault() || !transmitProgress_)
            return sample.result=kIOReturnNotPermitted;
        sample.callbacks=callbacks_;
        if(transmitProgress_->fault())return sample.result=kIOReturnError;
        const auto plan=transmitHandoffs_.begin(unsigned(transmitProgress_->next()));
        if(!plan)return sample.result=kIOReturnNotReady;
        const auto reject=[&](IOReturn error) {
            (void)transmitHandoffs_.finish(false);return sample.result=error;
        };
        const auto start=((transmitStart_>>25)&3)*8000+((transmitStart_>>12)&8191);
        const auto lead=[&](unsigned &value) {
            UInt32 timer=0;auto status=(*device)->GetCycleTime(device,&timer);
            if(status)return status;
            if(((timer>>12)&8191)>=8000)return IOReturn(kIOReturnBadArgument);
            const auto now=((timer>>25)&3)*8000+((timer>>12)&8191);
            value=(start+plan->stopOffset+32000-now)%32000;return IOReturn(0);
        };
        if(!packets.empty()) {
            if(plan->ordinal>=TransmitBankPolicy::count || packets.size()!=(plan->ordinal?transmitBankPackets:3*transmitBankPackets-firstRefreshCycle))return reject(kIOReturnBadArgument);
            if(replacementOffset+transmitSlots*512>bufferBytes_)return reject(kIOReturnNoSpace);
            for(const auto &packet:packets)
                if(packet.length<8 || packet.length>448 || packet.length%4)
                    return reject(kIOReturnBadArgument);
        }
        const auto measure=[&](unsigned phase,auto operation) {
            const auto began=std::chrono::steady_clock::now();
            const auto cpu=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
            const auto result=operation();
            sample.phaseCpuNS[phase]+=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)-cpu;
            sample.phaseMicros[phase]+=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-began).count());
            ++sample.phaseCalls[phase];return result;
        };
        auto result=measure(0,[&]{return lead(sample.before);});
        // Apple IOFWUserIsochPort.cpp:940-954 delivers async completion;
        // IOFireWireLibDevice.cpp:992-1001 reads the timer independently.
        // Hardware capture worker-endurance-2026-09-24 proves boundary+1 can
        // still be early. startup-cadence-001 also captured lead129.
        // Wait ONLY before range/branch mutation; admission remains64..(bank-1).
        if(!result && (sample.before==transmitBankPackets || sample.before==transmitBankPackets+1)) {
            const auto earlyStart=std::chrono::steady_clock::now();
            auto elapsed=[&] {return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-earlyStart).count());};
            NativePackets::EarlyTransmitGate gate;
            gate.wait(sample.before,guard,lead,elapsed,[] {(void)usleep(125);},transmitBankPackets-1);
            sample.earlyReads=gate.reads;sample.earlyMicros=elapsed();
            sample.earlyResult=gate.result;sample.earlyRevalidated=gate.revalidated;
            if(!gate.ready)result=gate.nativeStatus?gate.nativeStatus:IOReturn(kIOReturnTimeout);
            // A host Stop during this read-only gate is not a failed native
            // publication. No payload/range/branch has been touched yet.
            // Preserve the existing null tail for NativeStopDrain; do not
            // poison the graph via finish(false), or permit another handoff.
            // Apple IOFWUserIsochPort.cpp:1235-1287 changes native branches
            // only through ModifyJump; none has been submitted on this path.
            if(gate.result==NativePackets::EarlyTransmitGate::Result::guardFailure &&
               result==kIOReturnAborted) {
                if(!transmitHandoffs_.stopBeforePublication())result=kIOReturnError;
                return sample.result=result;
            }
        }
        if(!result && (sample.before<64 || sample.before>=transmitBankPackets))result=kIOReturnTimeout;
        const auto began=std::chrono::steady_clock::now();
        // The completed bank is unreachable from the active bank until the
        // return link below. Preserve original payloads. FULL_HEAD alone also
        // recompiles descriptor 0, only AFTER its observed first completion.
        // Apple IOFWUserIsochPort.cpp:1187-1232 imports ranges; 1216-1220
        // restores null interior branches. Install the terminal cut AFTER Modify.
        for(unsigned first=0;!result && first<packets.size();first+=64) {
            std::array<void *,64> changed{};
            const unsigned count=unsigned(std::min(size_t(64),packets.size()-first));
            result=measure(1,[&] {
              for(unsigned i=0;!result && i<count;++i) {
                const auto n=plan->cutBank*transmitBankPackets+(plan->ordinal?0:firstRefreshCycle-transmitSlots)+first+i;
                auto *bytes=static_cast<uint8_t *>(buffer_)+replacementOffset+n*512;
                std::memcpy(bytes,packets[first+i].bytes.data(),packets[first+i].length);
                IOVirtualRange range{reinterpret_cast<IOVirtualAddress>(bytes),packets[first+i].length};
                result=(*pool_)->SetDCLRanges(transmitDcls_[n],1,&range);
                changed[i]=transmitDcls_[n];
              }
              return result;
            });
            if(!result)result=measure(2,[&]{return (*local_)->Notify(local_,kFWNuDCLModifyNotification,changed.data(),count);});
            if(!result)sample.refreshed+=count;
#ifdef PRO40_DMA_TX_REFILL
            if(!result && refillArchive_) {
                // Diagnostic failure must not change the already published
                // native update or decide whether the live bank is linked.
                for(unsigned i=0;i<count;++i) {
                    const auto n=plan->cutBank*transmitBankPackets+first+i;
                    refillArchive_->append(uint64_t(plan->ordinal)*transmitBankPackets+first+i,
                        std::span(static_cast<const uint8_t *>(buffer_)+replacementOffset+n*512,packets[first+i].length));
                }
            }
#endif
        }
        if(!packets.empty())sample.refreshResult=result;
        // A synchronous Modify may be slow: recheck before any branch mutation.
        if(!result)result=measure(3,[&]{return lead(sample.cutLead);});
        const auto refreshElapsed=std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-began).count();
        if(!result && (sample.cutLead<64 || sample.cutLead>sample.before ||
            uint64_t(refreshElapsed)>uint64_t(sample.before-64)*125))result=kIOReturnTimeout;
        if(!result)result=measure(4,[&]{return (*pool_)->SetDCLBranch(transmitDcls_[plan->cutBank*transmitBankPackets+transmitBankPackets-1],nullptr);});
        if(!result) {
            void *changed[]={transmitDcls_[plan->cutBank*transmitBankPackets+transmitBankPackets-1]};
            result=measure(5,[&]{return (*local_)->Notify(local_,kFWNuDCLModifyJumpNotification,changed,1);});
        }
        sample.cutResult=result;
        if(!result)result=measure(6,[&]{return lead(sample.cutLead);});
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-began).count();
        if(!result && (sample.cutLead<64 || sample.cutLead>sample.before ||
            uint64_t(elapsed)>uint64_t(sample.before-64)*125))result=kIOReturnTimeout;
        if(!result)result=measure(7,[&]{return (*pool_)->SetDCLBranch(transmitDcls_[plan->liveBank*transmitBankPackets+transmitBankPackets-1],transmitDcls_[plan->cutBank*transmitBankPackets]);});
        if(!result) {
            void *changed[]={transmitDcls_[plan->liveBank*transmitBankPackets+transmitBankPackets-1]};
            result=measure(8,[&]{return (*local_)->Notify(local_,kFWNuDCLModifyJumpNotification,changed,1);});
        }
        if(!result)result=measure(9,[&]{return lead(sample.after);});
        sample.micros=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-began).count());
        if(!result && (sample.after<32 || sample.after>sample.cutLead ||
            sample.micros>uint64_t(sample.before-32)*125))result=kIOReturnTimeout;
        (void)transmitHandoffs_.finish(!result);
        return sample.result=result;
    }
#endif
#ifdef PRO40_DMA_RX_BRANCH_EXTEND
    struct ReceiveExtension {
        unsigned callbacks=0,before=0,after=0;
        uint64_t micros=0;
        // Fixed diagnostic storage, emitted after Stop. Bit mask distinguishes
        // skipped calls from calls shorter than one microsecond.
        std::array<uint64_t,7> phaseMicros{};
        unsigned phaseMask=0;
        IOReturn result=kIOReturnNotReady;
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
        unsigned returnLead=0;
        IOReturn returnResult=kIOReturnNotReady;
#endif
    };
    IOReturn extendReceiveBranch(IOFireWireLibDeviceRef device,ReceiveExtension &sample) {
        // Preserve the actual watermark even when begin() rejects a late bank.
        // Previously these failures misleadingly logged callbacks=0.
        sample.callbacks=callbacks_;
#ifdef PRO40_DMA_RX_HANDOFFS
        if(index_>=2 || !started_ || !local_ || reuseFault())return sample.result=kIOReturnNotPermitted;
#ifdef PRO40_DMA_SOURCE_QUEUE
        if(receiveProgress_->next()>UINT32_MAX)return sample.result=kIOReturnOverrun;
        const auto plan=handoffs_.begin(unsigned(receiveProgress_->next()));
#else
        const auto plan=handoffs_.begin(receiveArchive_->next());
#endif
        if(!plan)return sample.result=kIOReturnNotReady;
        auto stopDcl=receiveTails_[plan->liveBank];
        auto returnStopDcl=receiveTails_[plan->cutBank];
        auto nextDcl=receiveHeads_[plan->cutBank];
        const auto stopOffset=plan->stopOffset;
#else
        if(index_>=2 || !started_ || !local_ || receiveExtensionAttempted_)
            return sample.result=kIOReturnNotPermitted;
        receiveExtensionAttempted_=true; // Never retry an uncertain notification.
        auto stopDcl=receiveStopDcl_;
        auto nextDcl=receiveNextDcl_;
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
        auto returnStopDcl=receiveReturnStopDcl_;
#endif
        const auto stopOffset=receiveGate-1;
#endif
        sample.callbacks=callbacks_;
#ifdef PRO40_DMA_SOURCE_QUEUE
        if(!receiveProgress_->ready(receiveCopyCount))return sample.result=kIOReturnNotReady;
#elif defined(PRO40_DMA_RX_REUSE)
        if(!receiveArchive_ || !receiveArchive_->canExtend())return sample.result=kIOReturnNotReady;
#endif
        if(callbacks_<receiveCopyCount || callbacks_>=stopOffset+1-64 || !metadata_[0].completions)
            return sample.result=kIOReturnNotReady;
#ifdef PRO40_DMA_SOURCE_QUEUE
        const auto stamp=receiveProgress_->firstTimestamp();
#elif defined(PRO40_DMA_RX_HANDOFFS)
        const auto stamp=receiveArchive_->view(0).timestamp;
#else
        const auto stamp=metadata_[0].observedTimestamp;
#endif
        if((stamp&0xf0000fff) || ((stamp>>12)&8191)>=8000)
            return sample.result=kIOReturnBadArgument;
        const auto target=(((stamp>>25)&7)*8000+((stamp>>12)&8191)+stopOffset)%64000;
        const auto lead=[&](unsigned &value) {
            UInt32 timer=0;
            const auto status=(*device)->GetCycleTime(device,&timer);
            if(status)return status;
            if(((timer>>12)&8191)>=8000)return IOReturn(kIOReturnBadArgument);
            const auto now=((timer>>25)&7)*8000+((timer>>12)&8191);
            value=(target+64000-now)%64000;
            return IOReturn(0);
        };
        const auto measure=[&](unsigned phase,auto work) {
            const auto begin=std::chrono::steady_clock::now();
            const auto result=work();
            sample.phaseMicros[phase]=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-begin).count());
            sample.phaseMask|=1u<<phase;
            return result;
        };
        auto result=measure(0,[&]{return lead(sample.before);});
        if(!result && (sample.before<64 || sample.before>receiveGate-1))result=kIOReturnTimeout;
        const auto began=std::chrono::steady_clock::now();
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
        // First make the copied bank terminal, then link the active bank to it.
        // No intermediate state deliberately creates a circular program.
        if(!result)result=measure(1,[&]{return (*pool_)->SetDCLBranch(returnStopDcl,nullptr);});
        if(!result) {
            void *changed[]={returnStopDcl};
            result=measure(2,[&]{return (*local_)->Notify(local_,kFWNuDCLModifyJumpNotification,changed,1);});
        }
        sample.returnResult=result;
        if(!result)result=measure(3,[&]{return lead(sample.returnLead);});
        const auto spent=std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-began).count();
        if(!result && (sample.returnLead<64 || sample.returnLead>sample.before ||
            uint64_t(spent)>uint64_t(sample.before-64)*125))result=kIOReturnTimeout;
#endif
        if(!result)result=measure(4,[&]{return (*pool_)->SetDCLBranch(stopDcl,nextDcl);});
        if(!result) {
            void *changed[]={stopDcl};
            result=measure(5,[&]{return (*local_)->Notify(local_,kFWNuDCLModifyJumpNotification,changed,1);});
        }
        if(!result)result=measure(6,[&]{return lead(sample.after);});
        sample.micros=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-began).count());
        if(!result && (sample.after<32 || sample.after>sample.before ||
            sample.micros>uint64_t(sample.before-32)*125))result=kIOReturnTimeout;
#ifdef PRO40_DMA_RX_HANDOFFS
        if(!handoffs_.finish(!result) && !result)result=kIOReturnError;
#endif
        return sample.result=result;
    }
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
    NativePackets::CompletedBuffer capturedPacket(unsigned n) const {
        if(!receiveJournal_ || captureFault())return {};
        return receiveJournal_->view(n);
    }
    bool captureFault() const {
        return index_<2 && (!receiveJournal_ || receiveJournal_->error()!=NativePackets::JournalError::none ||
            !receiveSequence_ || receiveSequence_->error()!=NativePackets::SequenceError::none);
    }
    IOReturn consumeCapturedBatch(uint64_t first) {
        if(!receiveJournal_)return kIOReturnNotReady;
        if(captureFault())return kIOReturnError;
        return receiveJournal_->consume(first)==NativePackets::JournalError::none?0:kIOReturnError;
    }
    struct UpdateTiming {uint64_t rangesMicros=0,notifyMicros=0;IOReturn notifyResult=kIOReturnNotReady;};
    IOReturn replaceFutureBatch(unsigned first,std::span<const NativePackets::PreparedBuffer> packets,
                               UpdateTiming &timing) {
        timing={};
        if(index_<2 || !started_ || !local_ || first<384 || first%64 || first>programCount_-64 || packets.size()!=64)
            return kIOReturnNotPermitted;
        for(unsigned i=0;i<64;++i)
            if(metadata_[first+i].completions || packets[i].length<8 || packets[i].length>448 || packets[i].length%4)
                return kIOReturnBadArgument;
        // Same native dynamic Notify contract as the validated flow-update
        // probe, but content is supplied by the composition layer. Never touch
        // the published head or overwrite the original packet bank.
        const auto began=std::chrono::steady_clock::now();
        std::array<void *,64> changed{};
        for(unsigned i=0;i<64;++i) {
            auto *bytes=static_cast<uint8_t *>(buffer_)+replacementOffset+(first+i)*512;
            std::memcpy(bytes,packets[i].bytes.data(),packets[i].length);
            IOVirtualRange range{reinterpret_cast<IOVirtualAddress>(bytes),packets[i].length};
            const auto result=(*pool_)->SetDCLRanges(transmitDcls_[first+i],1,&range);
            if(result)return result;
            changed[i]=transmitDcls_[first+i];
        }
        const auto rangesDone=std::chrono::steady_clock::now();
        timing.rangesMicros=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(rangesDone-began).count());
        timing.notifyResult=(*local_)->Notify(local_,kFWNuDCLModifyNotification,changed.data(),64);
        timing.notifyMicros=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-rangesDone).count());
        return timing.notifyResult;
    }
#endif
#ifdef PRO40_DMA_TX_FLOW_UPDATE
    IOReturn updateFutureBatch(unsigned first) {
        // Fixed experiment: only the last 128 of 512 descriptors, in batches
        // of 64 (IOFWUserIsochPort.cpp:1175 rejects larger Notify lists).
        if (index_<2 || !started_ || !local_ || (first!=384 && first!=448) || callbacks_<32)
            return kIOReturnNotPermitted;
        Pro40Packets::Am824Stream content(Pro40Packets::playback[index_-2],sender_);
        content.reset(uint8_t((first-384)*6));
        std::array<void *,64> changed{};
        const auto firstCycle=(transmitStart_>>12)&0x1fff;
        for (unsigned n=first;n<first+64;++n) {
            if (metadata_[n].completions) return kIOReturnNotPermitted;
            auto *bytes=static_cast<uint8_t *>(buffer_)+replacementOffset+n*512;
            const uint8_t blocks=n%4?8:0;
            const auto syt=blocks?*Pro40Packets::replaySyt(0,(firstCycle+n)%8000):0xffff;
            const auto length=content.make(std::span(bytes,448),blocks,syt);
            if (!length) return kIOReturnError;
            IOVirtualRange range{reinterpret_cast<IOVirtualAddress>(bytes),*length};
            const auto status=(*pool_)->SetDCLRanges(transmitDcls_[n],1,&range);
            if (status) return status;
            changed[n-first]=transmitDcls_[n];
        }
        return (*local_)->Notify(local_,kFWNuDCLModifyNotification,changed.data(),64);
    }
#endif
#if defined(PRO40_DMA_TX_PRIME) && !defined(PRO40_DMA_TX_FLOW_UPDATE) && !defined(PRO40_DMA_DUPLEX_REPLAY)
    IOReturn primeTimedPackets() {
#ifdef PRO40_DMA_TX_ARMED_UPDATE
        if (index_ < 2 || !started_ || !local_ || callbacks_) return kIOReturnNotPermitted;
        constexpr size_t bankOffset = replacementOffset;
#else
        if (index_ < 2 || started_ || !local_) return kIOReturnNotPermitted;
        constexpr size_t bankOffset = 0;
#endif
        // The priming variant changes a never-started program. ARMED_UPDATE
        // uses a separate bank after Start and preserves the published head.
        // The entire mmap was exported, including all replacement ranges.
        // Apple IOFireWireLibIsoch.h:839-851 / IsochPort.cpp:1475-1514;
        // IOFWUserIsochPort.cpp:1187-1232 reimports changes before native notify.
        Pro40Packets::Am824Stream content(Pro40Packets::playback[index_-2],sender_);
        const auto firstCycle = (transmitStart_ >> 12) & 0x1fff;
        for (unsigned n=0; n<programCount_; ++n) {
            auto *bytes = static_cast<uint8_t *>(buffer_)+bankOffset+n*512;
            const uint8_t blocks = n%4 ? 8 : 0;
            const auto syt = blocks ? *Pro40Packets::replaySyt(0,(firstCycle+n)%8000) : 0xffff;
            const auto length = content.make(std::span(bytes,448),blocks,syt);
            if (!length) return kIOReturnError;
            IOVirtualRange range{reinterpret_cast<IOVirtualAddress>(bytes),*length};
#ifdef PRO40_DMA_TX_ARMED_UPDATE
            // Start has already published the head descriptor to hardware.
            // Recompiling it lost its completion in armed-update-001, despite
            // ample lead. Preserve that descriptor and its original no-data bank.
            const auto result = n==0 ? kIOReturnSuccess : (*pool_)->SetDCLRanges(transmitDcls_[n],1,&range);
#else
            const auto result = (*pool_)->SetDCLRanges(transmitDcls_[n],1,&range);
#endif
            if (result) return result;
            std::fprintf(log_,"tx-prime port=%u descriptor=%u bytes=%zu blocks=%u dbc=%u syt=%04x\n",
                index_,n,*length,blocks,bytes[3],unsigned(syt));
        }
        // Notify is a synchronous export/recompile, not a DMA completion fence.
        // ARMED_UPDATE switches to a separate bank before the scheduled cycle;
        // it never overwrites or recycles the old bank. Continuous reuse is untested.
        std::array<void *,8> changed{};
#ifdef PRO40_DMA_TX_ARMED_UPDATE
        constexpr unsigned firstChanged=1;
#else
        constexpr unsigned firstChanged=0;
#endif
        for (unsigned n=firstChanged;n<changed.size();++n) changed[n-firstChanged]=transmitDcls_[n];
        const auto result = (*local_)->Notify(local_,kFWNuDCLModifyNotification,
            changed.data(),UInt32(changed.size()-firstChanged));
#ifdef PRO40_DMA_TX_ARMED_UPDATE
        event("armed-bank-modify-notify",unsigned(result));
#else
        event("prime-modify-notify",unsigned(result));
#endif
        return result;
    }
#endif
    IOReturn start() {
#ifdef PRO40_DMA_TX_TIMING
        event("enter-start-scheduled-no-data-test");
#else
        event("enter-start-empty-cycle-test");
#endif
        const auto status = (*channel_)->Start(channel_);
        started_ = !status;
        needsFinalize_ = started_;
        event("leave-start", unsigned(status));
        return status;
    }
    unsigned callbacks() const { return callbacks_; }
#if defined(PRO40_DMA_SOURCE_QUEUE) && defined(PRO40_DMA_BIDIRECTIONAL_REUSE) && defined(PRO40_DMA_TX_RETURN)
    std::optional<unsigned> terminalProgress() const {
        if(!started_ || callbacksClosed_)return {};
        if(index_<2) {
            if(reuseFault() || !receiveProgress_ || receiveProgress_->next()!=callbacks_)return {};
            return handoffs_.terminalProgress();
        }
        if(!transmitProgress_ || transmitProgress_->fault() || transmitProgress_->next()!=callbacks_)return {};
        return transmitHandoffs_.terminalProgress();
    }
#endif
#ifdef PRO40_DMA_SOURCE_QUEUE
    void setDispatchActive(bool active) {dispatchBoundary_.setActive(active);}
    unsigned dispatchYields() const {return dispatchBoundary_.yields();}
    bool dispatchActive() const {return dispatchBoundary_.active();}
#endif
    unsigned expectedCallbacks() const {
#ifdef PRO40_DMA_TX_RETURN
        if(index_>=2)return transmitReturnCount;
#endif
#if defined(PRO40_DMA_RX_BRANCH_STOP) && !defined(PRO40_DMA_RX_BRANCH_EXTEND)
        if(index_<2)return receiveGate;
#endif
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
        if(index_<2)return descriptorCount;
#endif
        return programCount_;
    }
    IOReturn stop() {
        if (!started_) return 0;
        event("enter-stop");
        const auto result = (*channel_)->Stop(channel_);
        event("leave-stop", unsigned(result));
        if (!result) started_ = false;
        return result;
    }
#ifdef PRO40_DMA_DUPLEX_CAPTURE
#ifdef PRO40_DMA_SOURCE_QUEUE
    void setHistoryEnabled(bool enabled) {retainHistory_=enabled;}
#endif
    void setCapture(FILE *file) { capture_ = file; }
#ifdef PRO40_DMA_RX_REUSE
    void setFinalCapture(FILE *file) {finalCapture_=file;}
    bool reuseFault() const {return index_<2 && (
#ifdef PRO40_DMA_SOURCE_QUEUE
        !receiveProgress_ || receiveProgress_->fault() || !sourceQueue_ || sourceQueue_->fault()
#else
        !receiveArchive_ || receiveArchive_->fault()
#endif
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
        || !receiveSequence_ || receiveSequence_->error()!=NativePackets::SequenceError::none
#endif
        );}
#endif
#endif
#endif
    struct ReleaseResult {IOReturn native=0,evidence=0;};
    ReleaseResult releaseDetailed([[maybe_unused]] bool stoppedPrefix=false) {
        IOReturn status = 0, evidence = 0;
#ifdef PRO40_DMA_LIFECYCLE
        bool needsFinalize = needsFinalize_;
        status = stop();
#endif
        if (allocationAttempted_) {
            event("enter-release-channel");
            const auto released = (*channel_)->ReleaseChannel(channel_);
            event("leave-release-channel", unsigned(released));
            if (!status) status = released;
            allocationAttempted_ = false;
        }
        if (channel_) {
            event("enter-release-channel-object");
            (*channel_)->Release(channel_); channel_ = nullptr;
            event("leave-release-channel-object");
        }
        if (constraint_) { (*constraint_)->Release(constraint_); constraint_ = nullptr; }
        if (local_) {
            event("enter-release-local-port");
            const auto refs = (*local_)->Release(local_); local_ = nullptr;
            event("leave-release-local-port", unsigned(refs));
            // Apple IOFireWireLibIUnknown.cpp:54-72 returns the PRE-release
            // count: 1 means final destruction, not one remaining reference.
            // This inference applies only to our never-started port (no deferred
            // stop tokens). A running transport must use the finalize callback.
#ifdef PRO40_DMA_LIFECYCLE
            // Start can fail after a local port was reached; Apple's channel
            // Start then calls Stop (IOFireWireLibIsochChannel.cpp:271-272).
            // An outstanding release token still requires our refcon to live.
            if (refs != 1) needsFinalize = true;
#else
            if (refs != 1)
            { pool_ = nullptr; buffer_ = nullptr; return {kIOReturnBusy,0}; }
#endif
        }
#ifdef PRO40_DMA_LIFECYCLE
        if (needsFinalize) {
            // Keep this object, its DCL pool, refcons and mmap alive while the
            // library drains callbacks and executes deferred port releases.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!finalized_ && std::chrono::steady_clock::now() < deadline)
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
            event("finalize-callback", finalized_ ? 0 : unsigned(kIOReturnTimeout));
            event("DCL-callback-count", callbacks_);
            event("DCL-native-callback-count", nativeCallbacks_);
            bool prefixChecked=false;
#if defined(PRO40_DMA_SOURCE_QUEUE) && defined(PRO40_DMA_BIDIRECTIONAL_REUSE) && defined(PRO40_DMA_TX_RETURN)
            if(stoppedPrefix) {
                prefixChecked=true;
                const bool receive=index_<2;
                const auto progress=receive?(receiveProgress_?receiveProgress_->next():0):
                    (transmitProgress_?transmitProgress_->next():0);
                const bool fault=receive?reuseFault():(!transmitProgress_ || transmitProgress_->fault());
                const auto handoffCount=receive?handoffs_.completed():transmitHandoffs_.completed();
                const bool handoffFault=receive?handoffs_.fault():transmitHandoffs_.fault();
                const NativePackets::StoppedPrefix prefix{progress,callbacks_,nativeCallbacks_,
                    receive?descriptorCount:transmitReturnCount,completionLayout(!receive),
                    handoffCount,fault || handoffFault || retainHistory_,receive?receiveCopyCount:transmitBankPackets};
                bool valid=prefix.valid();
                if(receive)valid=valid && receiveSequence_ && receiveSequence_->next()==progress &&
                    sourceQueue_ && NativePackets::stoppedSourceQueueValid(sourceQueue_->first(),
                        sourceQueue_->next(),sourceQueue_->copied(),progress,
                        unsigned(sourceQueue_->queued()),sourceQueue_->fault(),transmitBankPackets) && sourceQueue_->observerValid();
                if(receive && sourceQueue_ && sourceQueue_->observing())
                    std::fprintf(log_,"dma %u source-observer observed=%llu copied=%llu next=%llu max-lead=%llu valid=%u\n",
                        index_,(unsigned long long)sourceQueue_->observed(),(unsigned long long)sourceQueue_->copied(),
                        (unsigned long long)sourceQueue_->next(),(unsigned long long)sourceQueue_->maximumObserverLead(),
                        unsigned(sourceQueue_->observerValid()));
                for(unsigned n=0;n<programCount_;++n) {
                    const auto &entry=metadata_[n];
                    if(entry.completions!=prefix.completions(n) ||
                       (entry.completions && (entry.observedStatus&31)!=17))valid=false;
                }
                std::fprintf(log_,"dma %u stopped-prefix valid=%u progress=%llu callbacks=%u native=%u handoffs=%u\n",
                    index_,unsigned(valid),static_cast<unsigned long long>(progress),callbacks_,nativeCallbacks_,handoffCount);
                if(!valid)evidence=kIOReturnError;
            }
#endif
            if(stoppedPrefix && !prefixChecked)evidence=kIOReturnUnsupported;
            if(!prefixChecked) {
#ifdef PRO40_DUPLEX_WORKER
            const auto expectedNative=completionLayout(index_>=2).notifications(expectedCallbacks());
            if(!expectedNative || nativeCallbacks_!=*expectedNative)evidence=kIOReturnError;
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
            if(nativeCallbacks_!=programCount_/64)evidence = kIOReturnError;
            if(receiveJournal_) {
                std::fprintf(log_,"dma %u receive-sequence next=%llu error=%u\n",index_,
                    static_cast<unsigned long long>(receiveSequence_->next()),unsigned(receiveSequence_->error()));
                std::fprintf(log_,"dma %u journal first=%llu next=%llu queued=%zu high-water=%zu error=%u\n",
                    index_,static_cast<unsigned long long>(receiveJournal_->first()),
                    static_cast<unsigned long long>(receiveJournal_->next()),receiveJournal_->queuedGroups(),
                    receiveJournal_->highWaterGroups(),unsigned(receiveJournal_->error()));
                if(captureFault())evidence = kIOReturnError;
            }
#endif
#ifdef PRO40_DMA_TX_RETURN
            if(index_>=2) {
#ifdef PRO40_DMA_TX_REFILL
                if(refillArchive_) {
                    std::fprintf(log_,"dma %u refill-archive records=%zu fault=%u runtime-independent=1\n",
                        index_,refillArchive_->size(),unsigned(refillArchive_->fault()));
                    if(refillArchive_->fault() || refillArchive_->size()!=transmitReturnCount-transmitSlots)evidence = kIOReturnError;
                }
#endif
                std::fprintf(log_,"dma %u transmit-progress next=%llu fault=%u archive-independent=1\n",index_,
                    static_cast<unsigned long long>(transmitProgress_->next()),unsigned(transmitProgress_->fault()));
                if(transmitProgress_->fault() || transmitProgress_->next()!=transmitReturnCount ||
                   (transmitVisits_ && (transmitVisits_->fault() || transmitProgress_->next()!=transmitVisits_->next())))evidence = kIOReturnError;
            }
            if(index_>=2 && transmitVisits_)for(unsigned n=0;n<transmitReturnCount;++n) {
                const auto record=transmitVisits_->view(n);
                std::fprintf(log_,"dma %u descriptor=%u completions=%u timestamp=%08x status=%08x\n",
                    index_,n,unsigned(n<transmitVisits_->next()),record.timestamp,record.status);
            }
#endif
#ifdef PRO40_DMA_BIDIRECTIONAL_REUSE
            if(index_<2) {
#ifdef PRO40_DMA_SOURCE_QUEUE
                std::fprintf(log_,"dma %u receive-progress next=%llu first-stamp=%08x fault=%u archive-independent=1\n",
                    index_,static_cast<unsigned long long>(receiveProgress_->next()),receiveProgress_->firstTimestamp(),unsigned(receiveProgress_->fault()));
                if(receiveProgress_->next()!=descriptorCount || receiveProgress_->fault())evidence = kIOReturnError;
                std::fprintf(log_,"dma %u source-queue first=%llu next=%llu copied=%llu queued=%zu high-water=%zu capacity=16 fault=%u\n",
                    index_,static_cast<unsigned long long>(sourceQueue_->first()),static_cast<unsigned long long>(sourceQueue_->next()),
                    static_cast<unsigned long long>(sourceQueue_->copied()),sourceQueue_->queued(),sourceQueue_->highWater(),unsigned(sourceQueue_->fault()));
                if(sourceQueue_->first()!=transmitReturnCount-transmitSlots || sourceQueue_->next()!=descriptorCount ||
                   sourceQueue_->copied()!=sourceQueue_->first() || sourceQueue_->queued()!=transmitSlots/64 || sourceQueue_->fault())evidence = kIOReturnError;
#endif
                std::fprintf(log_,"dma %u receive-sequence next=%llu error=%u\n",index_,
                    static_cast<unsigned long long>(receiveSequence_->next()),unsigned(receiveSequence_->error()));
                if(reuseFault() || nativeCallbacks_!=descriptorCount/64 || handoffs_.fault() ||
                   handoffs_.completed()!=ReceiveBankPolicy::count)evidence = kIOReturnError;
            }
#endif
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
            if(index_<2 && receiveArchive_)for(unsigned n=0;n<descriptorCount;++n) {
                const auto record=receiveArchive_->view(n);
                std::fprintf(log_,"dma %u descriptor=%u completions=%u timestamp=%08x status=%08x\n",
                    index_,n,unsigned(!record.bytes.empty()),record.timestamp,record.status);
            }
#endif
            for (unsigned n = 0; n < programCount_; ++n) {
                const auto &entry = metadata_[n];
#ifdef PRO40_DMA_TX_RETURN
                if(index_>=2) {
                    std::fprintf(log_,"dma %u physical-descriptor=%u completions=%u timestamp=%08x status=%08x\n",
                        index_,n,entry.completions,entry.observedTimestamp,entry.observedStatus);
                    const auto expected=n<transmitSlots?(transmitReturnCount-1-n)/transmitSlots+1:0u;
                    if(entry.completions!=expected || transmitProgress_->fault() || transmitProgress_->next()!=transmitReturnCount ||
                        transmitHandoffs_.fault() || transmitHandoffs_.completed()!=TransmitBankPolicy::count)
                        evidence = kIOReturnError;
                    continue;
                }
#endif
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
                if(index_<2) {
                    std::fprintf(log_,"dma %u physical-descriptor=%u completions=%u timestamp=%08x status=%08x\n",
                        index_,n,entry.completions,entry.observedTimestamp,entry.observedStatus);

                    const auto expected=n<receiveGate?(descriptorCount-1-n)/receiveGate+1:0;
                    if(entry.completions!=expected)evidence = kIOReturnError;
                    if(entry.completions && (entry.observedStatus&31)!=17)evidence = kIOReturnError;
                    continue;
                }
#endif
                std::fprintf(log_, "dma %u descriptor=%u completions=%u timestamp=%08x status=%08x\n",
                    index_, n, entry.completions, entry.observedTimestamp, entry.observedStatus);
                if (entry.completions && (entry.observedTimestamp == 0xdeadbeef || entry.observedStatus == 0xdeadbeef))
                    evidence = kIOReturnError;
#ifdef PRO40_DMA_DUPLEX_CAPTURE
                if (entry.completions != (n<expectedCallbacks()?1u:0u))
#else
                if (entry.completions != (index_ >= 2 ? 1u : 0u))
#endif
                    evidence = kIOReturnError;
                // IOFireWireLibIsoch.h SetDCLStatusPtr documents a five-bit
                // event code, with 0x11 = success. Preserve other bits in logs.
                if (entry.completions && (entry.observedStatus & 0x1f) != 0x11)
                    evidence = kIOReturnError;
#ifdef PRO40_DMA_TX_TIMING
                if (index_ >= 2 && entry.completions) {
                    const auto expected = ((((transmitStart_ >> 25) & 3)*8000 +
                        ((transmitStart_ >> 12) & 0x1fff)) + n) % 32000;
                    const auto observed = ((entry.observedTimestamp >> 25) & 3)*8000 +
                        ((entry.observedTimestamp >> 12) & 0x1fff);
                    if ((entry.observedTimestamp & 0xf0000fff) || observed != expected)
                        evidence = kIOReturnError;
                }
#endif
            }
            }
            std::fflush(log_); fsync(fileno(log_));
            if (!finalized_) {
                // Never unwind into dangling callback state or unmap active
                // backing memory. Let process teardown close the native client.
                event("abort-with-buffers-retained", unsigned(kIOReturnTimeout));
                std::_Exit(3);
            }
            needsFinalize_ = false;
#ifdef PRO40_DMA_DUPLEX_CAPTURE
            // Export only after DMA/callbacks quiesce. The reuse profile exports
            // owned history separately from the final mapping. Other finite
            // profiles still export their distinct, never-reused mapped slots.
            if (index_ < 2 && capture_) {
#ifdef PRO40_DMA_RX_REUSE
                const auto captureCount=descriptorCount;
#else
                const auto captureCount=programCount_;
#endif
                for (unsigned n = 0; n < captureCount; ++n) {
#ifdef PRO40_DMA_RX_REUSE
                    const auto record=receiveArchive_->view(n);
                    if(record.bytes.size()!=448 || std::fwrite(record.bytes.data(),1,record.bytes.size(),capture_)!=448)
                        evidence = kIOReturnIOError;
                    if(n<storageCount && (!finalCapture_ || std::fwrite(static_cast<uint8_t *>(buffer_)+n*512,1,448,finalCapture_)!=448))
                        evidence = kIOReturnIOError;
#else
                    if (std::fwrite(static_cast<uint8_t *>(buffer_)+n*512, 1, 448, capture_) != 448)
                        evidence = kIOReturnIOError;
#endif
                }
#ifdef PRO40_DMA_RX_REUSE
#ifdef PRO40_DMA_RX_HANDOFFS
                std::fprintf(log_,"dma %u bank-archive records=%u fault=%u bank-size=%u banks=2\n",
                    index_,receiveArchive_->next(),unsigned(receiveArchive_->fault()),receiveCopyCount);
#else
                std::fprintf(log_,"dma %u reuse-archive records=%u fault=%u aliased-first=%u aliased-count=%u\n",
                    index_,receiveArchive_->next(),unsigned(receiveArchive_->fault()),descriptorCount-receiveCopyCount,receiveCopyCount);
#endif
                if(receiveArchive_->next()!=descriptorCount || receiveArchive_->fault())evidence = kIOReturnError;
                if(finalCapture_ && std::fflush(finalCapture_))evidence = kIOReturnIOError;
#endif
                if (std::fflush(capture_)) evidence = kIOReturnIOError;
            }
#endif
        }
#endif
        // Finalization releases native ports, not our callback storage. The
        // shared dispatcher can run another port's queued completion while
        // releasing its siblings. Retain all pools/mappings/refcons through
        // native client close, including across an internal recovery attempt.
#ifdef PRO40_DMA_LIFECYCLE
        callbacksClosed_ = true;
        if(lateCallbacks_ && !status)status=kIOReturnError;
#endif
        return {status,evidence};
    }
    IOReturn callbackStatus() const {
#ifdef PRO40_DMA_LIFECYCLE
        return lateCallbacks_?kIOReturnError:0;
#else
        return 0;
#endif
    }
    // Caller barrier: remove dispatchers, release all native objects, then
    // pro40_close(). Apple IOFireWireLibDevice.cpp:74-118 destroys the callback
    // Mach ports and user-client connection. Only then may storage be retired.
    IOReturn reclaimAfterNativeClose() {
        if(local_ || channel_ || constraint_ || allocationAttempted_
#ifdef PRO40_DMA_LIFECYCLE
            || started_ || needsFinalize_
#endif
        )return kIOReturnBusy;
        auto status=callbackStatus();
#ifdef PRO40_DMA_LIFECYCLE
        if(lateCallbacks_ && log_)event("late-callbacks-after-finalize",lateCallbacks_);
#endif
        if(pool_) {(*pool_)->Release(pool_);pool_=nullptr;}
        if(buffer_ && munmap(buffer_,bufferBytes_))return kIOReturnError;
        buffer_=nullptr;bufferBytes_=0;callbackStorageExposed_=false;log_=nullptr;
        return status;
    }
    IOReturn release() {
        const auto result=releaseDetailed();
        return result.native?result.native:result.evidence;
    }
};
