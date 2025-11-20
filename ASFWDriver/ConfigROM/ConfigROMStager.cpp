#include "ConfigROMStager.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <cstring>

#include "../Hardware/HardwareInterface.hpp"
#include "../Logging/Logging.hpp"
#include "../Hardware/RegisterMap.hpp"

namespace {

constexpr uint64_t kRomAlignment = 1024; // OHCI §5.5.6 requires 1 KiB alignment

} // namespace

namespace ASFW::Driver {

ConfigROMStager::ConfigROMStager() = default;
ConfigROMStager::~ConfigROMStager() = default;

kern_return_t ConfigROMStager::Prepare(HardwareInterface& hw, size_t romBytes) {
    // NOTE: linkEnable is set in ControllerCore::InitialiseHardware() before this method
    // is called. Per OHCI §5.5.6, ConfigROMmap updates are valid when HCControl.linkEnable=1.
    // See ControllerCore::InitialiseHardware() for the linkEnable initialization sequence
    // (set linkEnable + BIBimageValid atomically per Linux ohci_enable line 2572-2574).
    if (prepared_) {
        return kIOReturnSuccess;
    }

    IOBufferMemoryDescriptor* rawBuffer = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, romBytes, kRomAlignment, &rawBuffer);
    if (kr != kIOReturnSuccess || rawBuffer == nullptr) {
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }
    buffer_ = OSSharedPtr(rawBuffer, OSNoRetain);
    buffer_->SetLength(romBytes);

    IOMemoryMap* rawMap = nullptr;
    kr = buffer_->CreateMapping(0, 0, 0, 0, 0, &rawMap);
    if (kr != kIOReturnSuccess || rawMap == nullptr) {
        buffer_.reset();
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }
    map_ = OSSharedPtr(rawMap, OSNoRetain);

    // CRITICAL: Touch memory BEFORE PrepareForDMA to force physical page allocation
    // macOS uses lazy allocation - pages aren't allocated until first access
    // If we call PrepareForDMA first, IOMMU might map to non-existent pages
    ZeroBuffer();
    ASFW_LOG(Hardware, "Physical pages allocated via ZeroBuffer before DMA mapping");

    dma_ = hw.CreateDMACommand();
    if (!dma_) {
        map_.reset();
        buffer_.reset();
        return kIOReturnNoResources;
    }

    uint32_t segCount = 1;
    IOAddressSegment segment{};
    uint64_t flags = 0;
    kr = dma_->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                             buffer_.get(),
                             0,
                             romBytes,
                             &flags,
                             &segCount,
                             &segment);
    if (kr != kIOReturnSuccess || segCount < 1) {
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma_.reset();
        map_.reset();
        buffer_.reset();
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoResources;
    }

    if ((segment.address & (kRomAlignment - 1)) != 0) {
        ASFW_LOG(Hardware, "Config ROM DMA address 0x%llx not 1KiB aligned", segment.address);
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma_.reset();
        map_.reset();
        buffer_.reset();
        return kIOReturnNotAligned;
    }

    if (segment.length < romBytes) {
        ASFW_LOG(Hardware,
                     "Config ROM DMA segment too small (len=%llu expected>=%zu)",
                     segment.length, romBytes);
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma_.reset();
        map_.reset();
        buffer_.reset();
        return kIOReturnNoResources;
    }

    segment_ = segment;
    dmaFlags_ = flags;
    prepared_ = true;
    // ZeroBuffer already called before PrepareForDMA to ensure physical page allocation
    return kIOReturnSuccess;
}

kern_return_t ConfigROMStager::StageImage(const ConfigROMBuilder& image, HardwareInterface& hw) {
    // Log PHY control state for diagnostics
    // Note: No simple "PHY present" bit exists - rdReg/wrReg bits indicate pending operations
    uint32_t phyControl = hw.Read(Register32::kPhyControl);
    ASFW_LOG(Hardware, "ConfigROM staging: PhyControl=0x%08x (rdDone=%d, wrReg=%d, rdReg=%d)",
             phyControl,
             (phyControl >> 31) & 1,  // bit 31: rdDone
             (phyControl >> 15) & 1,  // bit 15: wrReg
             (phyControl >> 14) & 1); // bit 14: rdReg

    kern_return_t kr = EnsurePrepared(hw);
    if (kr != kIOReturnSuccess) {
        return kr;
    }

    if (!map_) {
        return kIOReturnNotReady;
    }

    // CRITICAL: Use ImageNative() not ImageBE() for DMA buffer storage!
    // Per OHCI §5.5.6: Hardware reads ConfigROMheader/BusOptions from host memory
    // during bus reset and expects NATIVE byte order (little-endian on macOS).
    // The noByteSwapData flag does NOT apply to hardware DMA reads.
    auto romSpan = image.ImageNative();  // Host byte order for DMA buffer
    const size_t romBytes = romSpan.size() * sizeof(uint32_t);
    void* base = reinterpret_cast<void*>(map_->GetAddress());
    const size_t capacity = static_cast<size_t>(map_->GetLength());

    if (romBytes > capacity) {
        ASFW_LOG(Hardware,
                     "Config ROM image (%zu bytes) exceeds staging buffer (%zu bytes)",
                     romBytes, capacity);
        return kIOReturnNoSpace;
    }

    ZeroBuffer();
    if (romBytes > 0) {
        std::memcpy(base, romSpan.data(), romBytes);
        ASFW_LOG_CONFIG_ROM("Config ROM copied to DMA buffer in NATIVE byte order (little-endian)");

        // CRITICAL: Save and zero the header quadlet in DMA buffer (matches Linux ohci_enable line 2551)
        // Per Linux comment at lines 2515-2532: Some controllers DMA-read config ROM during
        // ConfigROMmap write. Setting header=0 marks ROM as "not ready yet".
        // ConfigROMStager::RestoreHeaderAfterBusReset() will restore it after bus reset.
        uint32_t* buffer = static_cast<uint32_t*>(base);
        savedHeader_ = buffer[0];              // Save header for restoration after bus reset
        savedBusOptions_ = image.BusInfoQuad(); // Save BusOptions for register restoration
        buffer[0] = 0;  // Zero header in DMA buffer (will be restored after bus reset)
        ASFW_LOG_CONFIG_ROM("DMA buffer header zeroed: 0x%08x → 0x00000000 (will restore after bus reset)", savedHeader_);
        ASFW_LOG_CONFIG_ROM("Saved expected values: header=0x%08x busOptions=0x%08x", savedHeader_, savedBusOptions_);

        // CRITICAL: Ensure cache-to-RAM flush completes before hardware DMA reads
        // Use atomic fence to prevent reordering, then explicit sync delay
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Force cache flush by reading back volatile (prevents optimization)
        volatile const uint32_t* sync = static_cast<volatile const uint32_t*>(base);
        for (size_t i = 0; i < romBytes / sizeof(uint32_t); i++) {
            (void)sync[i];  // Read forces cache coherency
        }

        // Additional barrier to ensure completion
        std::atomic_thread_fence(std::memory_order_seq_cst);
        ASFW_LOG_CONFIG_ROM("Cache flush sequence complete (fence + volatile read + fence)");

        // CRITICAL: Re-prepare DMA to refresh IOMMU mapping after data write
        // Original PrepareForDMA was called when buffer was zeroed
        // IOMMU might have cached zero pages - need to refresh mapping
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);

        uint32_t segCount = 1;
        IOAddressSegment segment{};
        uint64_t flags = 0;
        kr = dma_->PrepareForDMA(kIODMACommandPrepareForDMANoOptions,
                                 buffer_.get(),
                                 0,
                                 romBytes,
                                 &flags,
                                 &segCount,
                                 &segment);

        if (kr != kIOReturnSuccess || segCount < 1 || segment.address != segment_.address) {
            ASFW_LOG_CONFIG_ROM("DMA re-prepare failed: kr=0x%08x segCount=%u addr=0x%llx",
                                 kr, segCount, segment.address);
        } else {
            ASFW_LOG_CONFIG_ROM("DMA remapped after data write - IOMMU coherency refreshed");
        }

        // Verify DMA buffer contents after memcpy
        if (romBytes >= 3 * sizeof(uint32_t)) {
#if ASFW_DEBUG_CONFIG_ROM
            const uint32_t* verify = static_cast<const uint32_t*>(base);
            ASFW_LOG_CONFIG_ROM("DMA buffer verification (reading back from virtual address 0x%llx)",
                                reinterpret_cast<uint64_t>(base));
            ASFW_LOG_CONFIG_ROM("  verify[0] = 0x%08x (should be 0x%08x)", verify[0], image.HeaderQuad());
            ASFW_LOG_CONFIG_ROM("  verify[1] = 0x%08x (should be 0x31333934)", verify[1]);
            ASFW_LOG_CONFIG_ROM("  verify[2] = 0x%08x (should be 0x%08x)", verify[2], image.BusInfoQuad());
#endif
        }
    }

    if (segment_.address > 0xFFFFFFFFULL) {
        ASFW_LOG(Hardware,
               "Config ROM DMA address 0x%llx exceeds 32-bit range",
               segment_.address);
        return kIOReturnUnsupported;
    }

    // Per OHCI §5.5.6: ConfigROMheader and BusOptions registers "shall be reloaded
    // with updated values by Open HCI accesses to the host bus space" during bus reset.
    // We do NOT write them directly here - hardware will DMA-read from host memory.
    
    // However, GUID registers are special: §5.5.5 requires they be written ONCE after
    // power reset and then become read-only. We write them on first staging only.
    static bool guidWritten = false;
    if (!guidWritten) {
        hw.WriteAndFlush(Register32::kGUIDHi, image.GuidHiQuad());
        hw.WriteAndFlush(Register32::kGUIDLo, image.GuidLoQuad());
        ASFW_LOG(Hardware, "GUID registers initialized (write-once per OHCI §5.5.5)");
        guidWritten = true;
    }

    // CRITICAL: Write BusOptions and ConfigROMhdr registers BEFORE ConfigROMmap
    // Per OHCI §5.5.6: Hardware may DMA-read these registers during bus reset.
    // We write real values immediately so first bus reset sees valid Config ROM.
    // DMA buffer header is 0 initially (per Linux pattern), restored after bus reset.

    // Write BusOptions FIRST with real value (native byte order for register)
    hw.WriteAndFlush(Register32::kBusOptions, image.BusInfoQuad());
    ASFW_LOG(Hardware, "BusOptions written directly: 0x%08x (native byte order)", image.BusInfoQuad());

    // Write ConfigROMhdr with REAL VALUE immediately
    // CRITICAL: Don't set to 0! Other nodes read this during first bus reset.
    // If header=0, they see invalid ROM → triggers bus reset storm.
    // DMA buffer header is 0 initially (per Linux pattern), but register must be valid.
    hw.WriteAndFlush(Register32::kConfigROMHeader, image.HeaderQuad());
    ASFW_LOG(Hardware, "ConfigROMheader written: 0x%08x (ROM ready immediately)", image.HeaderQuad());

    // Write ConfigROMMap shadow register (ConfigROMmapNext per §5.5.6)
    // This will be atomically swapped to ConfigROMmap during next bus reset
    const uint32_t mapAddr = static_cast<uint32_t>(segment_.address);
    hw.WriteAndFlush(Register32::kConfigROMMap, mapAddr);
    ASFW_LOG(Hardware,
           "Config ROM shadow register updated (ConfigROMmapNext=0x%08x, bytes=%zu)",
           mapAddr, romBytes);

    // NOTE: BIBimageValid will be set atomically with linkEnable in InitialiseHardware()
    // per Linux ohci_enable() line 2572-2574. Do NOT set it here.

    // Log what hardware will reload during next bus reset
    ASFW_LOG_CONFIG_ROM("Config ROM staged in DMA buffer (IOVA=0x%08llx)", segment_.address);
    ASFW_LOG_CONFIG_ROM("  [0] Header    = 0x%08x (hardware will reload)", image.HeaderQuad());
    ASFW_LOG_CONFIG_ROM("  [1] BusName   = 0x31333934 (1394)");
    ASFW_LOG_CONFIG_ROM("  [2] BusOptions= 0x%08x (hardware will reload)", image.BusInfoQuad());
    ASFW_LOG_CONFIG_ROM("  [3] GUID Hi   = 0x%08x (already in register)", image.GuidHiQuad());
    ASFW_LOG_CONFIG_ROM("  [4] GUID Lo   = 0x%08x (already in register)", image.GuidLoQuad());
    ASFW_LOG(Hardware, "Shadow update pending - will activate on next bus reset (OHCI §5.5.6)");
    
    return kIOReturnSuccess;
}

void ConfigROMStager::Teardown(HardwareInterface& hw) {
    if (prepared_) {
        hw.ClearHCControlBits(HCControlBits::kBibImageValid);
        hw.WriteAndFlush(Register32::kConfigROMMap, 0);
    }

    if (dma_) {
        dma_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dma_.reset();
    }
    map_.reset();
    buffer_.reset();
    prepared_ = false;
    segment_ = {};
    dmaFlags_ = 0;
}

kern_return_t ConfigROMStager::EnsurePrepared(HardwareInterface& hw) {
    return prepared_ ? kIOReturnSuccess : Prepare(hw);
}

void ConfigROMStager::ZeroBuffer() {
    if (!map_) {
        return;
    }
    std::memset(reinterpret_cast<void*>(map_->GetAddress()), 0, static_cast<size_t>(map_->GetLength()));
}

void ConfigROMStager::RestoreHeaderAfterBusReset() {
    // Per Linux bus_reset_work (ohci.c lines 2178-2184):
    // After bus reset, hardware DMA-reads ConfigROMheader from host memory.
    // We zeroed the header during staging to mark ROM as "not ready".
    // Now restore the real header value so Config ROM reads work correctly.

    if (!map_ || savedHeader_ == 0) {
        ASFW_LOG(Hardware, "RestoreHeaderAfterBusReset: nothing to restore (map=%p saved=0x%08x)",
                 map_.get(), savedHeader_);
        return;
    }

    void* base = reinterpret_cast<void*>(map_->GetAddress());
    uint32_t* buffer = static_cast<uint32_t*>(base);

    const uint32_t currentHeader = buffer[0];
    buffer[0] = savedHeader_;

    // Cache flush to ensure write is visible to hardware DMA
    std::atomic_thread_fence(std::memory_order_seq_cst);
    volatile uint32_t sync = buffer[0];  // Force cache flush
    (void)sync;
    std::atomic_thread_fence(std::memory_order_seq_cst);

    ASFW_LOG(Hardware, "Config ROM header restored in DMA buffer: 0x%08x → 0x%08x",
             currentHeader, savedHeader_);
}

} // namespace ASFW::Driver
