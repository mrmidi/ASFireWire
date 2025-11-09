#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <DriverKit/IOLib.h>

#include "../OHCI_HW_Specs.hpp"

namespace ASFW::Async {

/**
 * \brief Information about a filled AR buffer ready for packet extraction.
 *
 * Returned by BufferRing::Dequeue() when hardware has written data to a buffer.
 *
 * \par CRITICAL: AR DMA Stream Semantics (OHCI §3.3, §8.4.2)
 * AR DMA operates in bufferFill mode where MULTIPLE packets are concatenated
 * into a single buffer. Hardware raises an interrupt after EACH packet, but
 * continues filling the SAME buffer until it's nearly exhausted.
 *
 * Therefore, Dequeue() may return the SAME descriptorIndex multiple times
 * with increasing bytesFilled values. The startOffset field indicates where
 * to begin parsing NEW packets that weren't present in the previous call.
 *
 * Example sequence:
 *   Interrupt #1: {descIndex=0, startOffset=0,  bytesFilled=20}  ← First packet
 *   Interrupt #2: {descIndex=0, startOffset=20, bytesFilled=40}  ← Second packet appended
 *   Interrupt #3: {descIndex=0, startOffset=40, bytesFilled=60}  ← Third packet appended
 *   ... hardware fills buffer[0] until nearly full ...
 *   Interrupt #N: {descIndex=1, startOffset=0,  bytesFilled=20}  ← Hardware advanced to buffer[1]
 *
 * Caller must:
 * 1. Parse packets from [virtualAddress + startOffset, virtualAddress + bytesFilled)
 * 2. Process ONLY the new packets in this range (old packets were processed in previous calls)
 * 3. Call Recycle() ONLY when ready to release the ENTIRE buffer back to hardware
 */
struct FilledBufferInfo {
    void* virtualAddress;        ///< Virtual address of buffer START (NOT offset by startOffset)
    size_t startOffset;          ///< Offset within buffer where NEW data begins (parse from here)
    size_t bytesFilled;          ///< Total bytes in buffer (parse up to here)
    size_t descriptorIndex;      ///< Index of descriptor for recycling
};

/**
 * \brief Fixed-size ring buffer for OHCI AR (Asynchronous Receive) DMA.
 *
 * Manages AR descriptor rings with INPUT_MORE descriptors in buffer-fill mode.
 * Unlike AT contexts (which use DescriptorRing for chaining), AR contexts use
 * a simple circular buffer where each descriptor points to a fixed-size buffer.
 *
 * \par OHCI Specification References
 * - §8.4.2: AR DMA operation (buffer-fill mode)
 * - §8.1.1: Descriptor status word endianness (BIG-ENDIAN for AR!)
 * - Table 8-1: INPUT_MORE descriptor format
 *
 * \par CRITICAL Endianness Requirements
 * Per OHCI §8.4.2 Table 8-1, AR descriptor statusWord is BIG-ENDIAN:
 * - statusWord = [xferStatus:16][resCount:16] in network byte order
 * - reqCount field is HOST order (NOT swapped)
 * - MUST use AR_resCount()/AR_xferStatus()/AR_init_status() helpers
 *
 * \par Buffer-Fill Mode Operation
 * Per OHCI §8.4.2:
 * 1. Software allocates N fixed-size buffers
 * 2. Each INPUT_MORE descriptor points to one buffer
 * 3. Hardware fills buffers sequentially, wrapping at end
 * 4. Software checks resCount != reqCount to detect filled buffers
 * 5. Software recycles buffers by resetting statusWord
 *
 * \par Packet Streams
 * Each buffer may contain MULTIPLE packets (§8.4.2). Software must parse
 * buffer contents using ARPacketParser to extract individual packets.
 *
 * \par Apple Pattern
 * Similar to AppleFWOHCI_AsyncReceive::allocatePacketBuffer():
 * - Fixed-size buffers (typically 4KB each)
 * - INPUT_MORE descriptors with buffer-fill mode
 * - getPacket() extracts data, updateResCount() recycles
 *
 * \par Linux Pattern
 * See drivers/firewire/ohci.c ar_context_init():
 * - Circular buffer of descriptors
 * - Each descriptor points to page-sized buffer
 * - handle_ar_packet() processes filled buffers
 */
class BufferRing {
public:
    BufferRing() = default;
    ~BufferRing() = default;

    /**
     * \brief Initialize AR ring with descriptors and data buffers.
     *
     * Sets up INPUT_MORE descriptors in buffer-fill mode, with each descriptor
     * pointing to a fixed-size data buffer.
     *
     * \param descriptors Storage for AR descriptors (must be 16-byte aligned)
     * \param buffers Storage for data buffers (continuous memory)
     * \param bufferCount Number of buffers (must equal descriptors.size())
     * \param bufferSize Size of each buffer in bytes
     * \return true on success, false if parameters invalid
     *
     * \par Descriptor Setup
     * Each descriptor is initialized as INPUT_MORE (cmd=0x2) per OHCI Table 8-1:
     * - control: [cmd:4][key:3][s:1][i:2][b:2][reserved:4][reqCount:16]
     *   - cmd = 0x2 (INPUT_MORE)
     *   - s = 1 (store xferStatus)
     *   - i = 0b11 (always interrupt)
     *   - b = 0b11 (always branch)
     * - reqCount: bufferSize (HOST byte order, NOT swapped)
     * - dataAddress: physical address of buffer
     * - branchWord: MakeBranchWordAR(nextDescPhys, true) with Z=1 for continue
     * - statusWord: BIG-ENDIAN [xferStatus:16][resCount:16], initialized with resCount=reqCount
     *
     * \warning statusWord MUST be initialized with AR_init_status(), not direct assignment.
     */
    [[nodiscard]] bool Initialize(
        std::span<HW::OHCIDescriptor> descriptors,
        std::span<uint8_t> buffers,
        size_t bufferCount,
        size_t bufferSize) noexcept;

    /**
     * \brief Patch descriptor dataAddress/branchWord with real physical addresses.
     *
     * Must be called after Initialize() once the caller knows the physical bases of both the
     * descriptor array and buffer pool. Without this step the controller would DMA to bogus
     * offsets (the placeholders written during Initialize()).
     *
     * \param descriptorsPhysBase Physical base address of descriptor array (16-byte aligned)
     * \param buffersPhysBase Physical base address of backing buffer storage
     * \return true if successfully patched, false if parameters invalid
     */
    [[nodiscard]] bool Finalize(uint64_t descriptorsPhysBase,
                                uint64_t buffersPhysBase) noexcept;

    /**
     * \brief Dequeue next filled buffer from ring.
     *
     * Scans descriptors starting from head index to find buffers where
     * resCount != reqCount (indicating hardware wrote data).
     *
     * \return FilledBufferInfo if buffer ready, nullopt if none available
     *
     * \par Implementation
     * 1. Read descriptor at head index with acquire fence
     * 2. Extract resCount using AR_resCount() (big-endian aware)
     * 3. If resCount != reqCount, buffer is filled
     * 4. Calculate bytesFilled = reqCount - resCount
     * 5. Return buffer info without advancing head (caller must Recycle)
     *
     * \par OHCI §8.4.2
     * "resCount is decremented as bytes are written. When resCount reaches 0,
     * the buffer is full. Software detects filled buffers by checking resCount != reqCount."
     *
     * \par Thread Safety
     * Safe to call concurrently with hardware DMA writes. Uses atomic head index
     * and memory barriers to ensure visibility of hardware-written status.
     */
    [[nodiscard]] std::optional<FilledBufferInfo> Dequeue() noexcept;

    /**
     * \brief Recycle buffer descriptor for reuse by hardware.
     *
     * Resets descriptor statusWord to indicate buffer is empty and available
     * for hardware to write. Updates head index to next descriptor.
     *
     * \param index Descriptor index to recycle (from FilledBufferInfo)
     * \return kIOReturnSuccess on success, error code if index invalid
     *
     * \par Implementation
     * 1. Validate index is current head
     * 2. Reset statusWord using AR_init_status(descriptor, reqCount)
     * 3. Release fence to ensure status write visible to hardware
     * 4. Advance head index (wrapping at bufferCount)
     *
     * \par OHCI §8.4.2
     * "Software recycles buffers by resetting resCount to reqCount, indicating
     * the buffer is empty and ready for hardware to write."
     *
     * \warning Must call with index from most recently dequeued buffer.
     * Recycling out-of-order is not supported.
     */
    [[nodiscard]] kern_return_t Recycle(size_t index) noexcept;

    /**
     * \brief Get virtual address of buffer at specified index.
     *
     * \param index Buffer index (0 to bufferCount-1)
     * \return Virtual address of buffer start, or nullptr if index invalid
     *
     * \par Usage
     * Used internally by Dequeue() to calculate FilledBufferInfo.virtualAddress.
     */
    [[nodiscard]] void* GetBufferAddress(size_t index) const noexcept;

    /**
     * \brief Get current head index (next buffer to dequeue).
     *
     * \return Head index (atomic read)
     */
    [[nodiscard]] size_t Head() const noexcept {
        return head_;
    }

    /**
     * \brief Get number of buffers in ring.
     *
     * \return Total buffer count
     */
    [[nodiscard]] size_t BufferCount() const noexcept {
        return bufferCount_;
    }

    /**
     * \brief Get size of each buffer in bytes.
     *
     * \return Buffer size
     */
    [[nodiscard]] size_t BufferSize() const noexcept {
        return bufferSize_;
    }

    /**
     * rief Encoded AR command pointer word for programming controller.
     *
     * Returns an encoded branch/command word suitable for AR::Arm(). Requires
     * Finalize(...) to have been called so physical bases are known.
     */
    [[nodiscard]] uint32_t CommandPtrWord() const noexcept;

    /**
     * \brief Bind DMA manager to buffer ring for cache synchronization.
     *
     * Must be called after Finalize() to enable DMA sync operations.
     *
     * \param dma Pointer to DMA memory manager (must outlive this ring)
     */
    void BindDma(class DMAMemoryManager* dma) noexcept;

    /**
     * \brief Publish all descriptors to DMA (flush after Finalize).
     *
     * Flushes entire descriptor array to make it visible to hardware.
     * Should be called once after Finalize() and before Arm().
     */
    void PublishAllDescriptorsOnce() noexcept;

    // ========================================================================
    // LLDB Debugging Helpers
    // ========================================================================

    /**
     * \brief Get base virtual address of buffer storage for LLDB inspection.
     *
     * \return Pointer to start of buffer storage, or nullptr if not initialized
     *
     * \par Usage in LLDB
     * Check if a VA is within buffer span:
     * ```
     * expr -R -- ((void*)info.virtualAddress >= ring->BufferBaseVA() && \
     *             (void*)info.virtualAddress < ((uint8_t*)ring->BufferBaseVA() + ring->BufferSpanBytes()))
     * ```
     */
    [[nodiscard]] void* BufferBaseVA() const noexcept {
        return buffers_.empty() ? nullptr : buffers_.data();
    }

    /**
     * \brief Get total size of buffer storage in bytes.
     *
     * \return Total bytes allocated for all buffers
     */
    [[nodiscard]] size_t BufferSpanBytes() const noexcept {
        return buffers_.size();
    }

    /**
     * \brief Get base virtual address of descriptor storage for LLDB inspection.
     *
     * \return Pointer to start of descriptor array, or nullptr if not initialized
     */
    [[nodiscard]] void* DescriptorBaseVA() const noexcept {
        return descriptors_.empty() ? nullptr : descriptors_.data();
    }

    /**
     * \brief Get total size of descriptor storage in bytes.
     *
     * \return Total bytes allocated for all descriptors
     */
    [[nodiscard]] size_t DescriptorSpanBytes() const noexcept {
        return descriptors_.size_bytes();
    }

    BufferRing(const BufferRing&) = delete;
    BufferRing& operator=(const BufferRing&) = delete;

private:
    std::span<HW::OHCIDescriptor> descriptors_;  ///< AR descriptor storage
    std::span<uint8_t> buffers_;                 ///< Data buffer storage
    size_t bufferCount_{0};                      ///< Number of buffers
    size_t bufferSize_{0};                       ///< Size of each buffer
    size_t head_{0};                             ///< Index of current buffer being filled by hardware
    size_t last_dequeued_bytes_{0};              ///< How many bytes of head_ buffer have been returned to caller
    // Device-visible bases recorded at Finalize time (32-bit usable range)
    uint32_t descIOVABase_{0};
    uint32_t bufIOVABase_{0};
    class DMAMemoryManager* dma_{nullptr};       ///< DMA manager for cache synchronization
};

} // namespace ASFW::Async
