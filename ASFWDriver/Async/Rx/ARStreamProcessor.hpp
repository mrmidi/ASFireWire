#pragma once

#include "ARPacketParser.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"

#include <DriverKit/IOLib.h>

#include <array>
#include <cstdint>
#include <span>

namespace ASFW::Async::Rx {

struct ARStreamStats {
    uint32_t buffersProcessed = 0;
    uint32_t packetsFound = 0;
};

// Shared bufferFill stream walker for both AR rings (request and response).
//
// OHCI §8.4.2: an AR ring is one contiguous packet stream; a packet
// (header+payload+trailer) may straddle a buffer boundary when a buffer fills.
// Hardware advances resCount only per fully written packet, so a trailing
// fragment can appear only at a buffer-full straddle. The walker therefore:
//   - commits only parsed bytes (never past an unparsed fragment),
//   - stitches boundary fragments via CopyReadableBytes/ConsumeReadableBytes
//     once the tail bytes have arrived in the next buffer,
//   - drops the remainder of a buffer loudly on an unparseable head (garbage
//     can never re-frame; waiting on it would stall the ring head forever).
// Cross-validated with Linux ohci.c ar_context, which makes straddles
// impossible by vmapping wraparound pages; this walker restores the same
// no-packet-loss guarantee for our per-buffer rings.
//
// Ctx must provide Dequeue/Recycle/CommitConsumed/CopyReadableBytes/
// ConsumeReadableBytes (ARContextBase or a bare BufferRing wrapper in tests).
// dispatch(const ARPacketParser::PacketInfo&) is invoked synchronously for
// every complete packet; onBuffer(bufferNo, info) is a debug hook per dequeue.
template <typename Ctx, typename OnBufferFn, typename DispatchFn>
ARStreamStats ProcessARStream(Ctx& ctx,
                              const char* ctxLabel,
                              uint64_t* heartbeatCounter,
                              OnBufferFn&& onBuffer,
                              DispatchFn&& dispatch) {
    using ParseFailure = ARPacketParser::ParseFailure;

    alignas(4) std::array<uint8_t, ARPacketParser::kMaxPacketBytes> stitchedPacket{};
    ARStreamStats stats{};

    auto recycle = [&](size_t descriptorIndex) {
        const kern_return_t recycleKr = ctx.Recycle(descriptorIndex);
        if (recycleKr != kIOReturnSuccess) {
            ASFW_LOG(Async,
                     "[%{public}s] failed to recycle descriptor %zu (kr=0x%08x)",
                     ctxLabel, descriptorIndex, recycleKr);
        }
    };
    auto commitConsumed = [&](size_t descriptorIndex, size_t consumedBytes) {
        const kern_return_t kr = ctx.CommitConsumed(descriptorIndex, consumedBytes);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Async,
                     "[%{public}s] failed to commit %zu bytes for descriptor %zu (kr=0x%08x)",
                     ctxLabel, consumedBytes, descriptorIndex, kr);
        }
    };
    auto logGarbageHead = [&](const uint8_t* bufferStart, size_t offset, size_t bufferSize,
                              ParseFailure failure) {
        uint32_t q0 = 0;
        uint32_t q1 = 0;
        if (offset + 4 <= bufferSize) {
            __builtin_memcpy(&q0, bufferStart + offset, 4);
        }
        if (offset + 8 <= bufferSize) {
            __builtin_memcpy(&q1, bufferStart + offset + 4, 4);
        }
        const char* kind = "zero garbage";
        if (failure == ParseFailure::UnknownTCode) {
            kind = "unknown tCode";
        } else if (failure == ParseFailure::OversizedPayload) {
            kind = "oversized data_length";
        }
        ASFW_LOG(Async,
                 "[%{public}s] unparseable head (%{public}s) at offset %zu/%zu q0=0x%08x q1=0x%08x — dropping buffer remainder",
                 ctxLabel, kind, offset, bufferSize, q0, q1);
    };

    while (auto bufferInfo = ctx.Dequeue()) {
        const auto& info = *bufferInfo;
        ++stats.buffersProcessed;

        if (heartbeatCounter) {
            const uint64_t seen = ++(*heartbeatCounter);
            if ((seen & 63u) == 0u) {
                ASFW_LOG(Async, "[%{public}s] hb bufs=%llu lastOff=%zu",
                         ctxLabel,
                         static_cast<unsigned long long>(seen),
                         info.startOffset);
            }
        }

        if (!info.virtualAddress) {
            recycle(info.descriptorIndex);
            continue;
        }

        const uint8_t* bufferStart = info.virtualAddress;
        const size_t bufferSize = info.bytesFilled;
        const size_t startOffset = info.startOffset;
        if (bufferSize == 0 || bufferSize <= startOffset) {
            recycle(info.descriptorIndex);
            continue;
        }

        onBuffer(stats.buffersProcessed, info);

        size_t offset = startOffset;
        bool bufferHandled = false;   // commit already issued by a stitch/drop branch
        bool committedPartial = false;
        while (offset < bufferSize) {
            auto parsed =
                ARPacketParser::ParseNext(std::span<const uint8_t>(bufferStart, bufferSize), offset);
            if (parsed.has_value()) {
                ++stats.packetsFound;
                dispatch(*parsed);
                offset += parsed->totalLength;
                continue;
            }

            if (parsed.error() == ParseFailure::NeedMoreBytes) {
                // Boundary fragment. Commit what we parsed so the readable window
                // starts at the fragment, then try to stitch across the boundary.
                if (offset > startOffset) {
                    commitConsumed(info.descriptorIndex, offset);
                    committedPartial = true;
                }

                const size_t stitchedBytes = ctx.CopyReadableBytes(stitchedPacket);
                if (stitchedBytes > 0) {
                    auto stitched = ARPacketParser::ParseNext(
                        std::span<const uint8_t>(stitchedPacket.data(), stitchedBytes), 0);
                    if (stitched.has_value()) {
                        ++stats.packetsFound;
                        dispatch(*stitched);
                        const kern_return_t consumeKr =
                            ctx.ConsumeReadableBytes(stitched->totalLength);
                        if (consumeKr != kIOReturnSuccess) {
                            ASFW_LOG(Async,
                                     "[%{public}s] failed to consume stitched packet (%zu bytes, kr=0x%08x)",
                                     ctxLabel, stitched->totalLength, consumeKr);
                        }
                        // Anomaly-visible on purpose: stitches are rare (one per
                        // ~bufferSize of inbound traffic at most), and any stitch
                        // line near a fault is a lead.
                        ASFW_LOG(Async, "[%{public}s] stitched tCode=0x%x len=%zu across buffer boundary",
                                 ctxLabel, stitched->tCode, stitched->totalLength);
                        bufferHandled = true;
                    } else if (stitched.error() != ParseFailure::NeedMoreBytes) {
                        // The fragment head itself is garbage — it will never parse.
                        logGarbageHead(bufferStart, offset, bufferSize, stitched.error());
                        commitConsumed(info.descriptorIndex, bufferSize);
                        bufferHandled = true;
                    } else {
                        ASFW_LOG_V2(Async,
                                    "[%{public}s] fragment (%zu bytes) at buffer end awaiting tail",
                                    ctxLabel, bufferSize - offset);
                    }
                }
                break;
            }

            // Unparseable head mid-buffer: framing is lost. Drop the remainder
            // loudly and resume at the next buffer's first byte. Note this is a
            // best-effort heuristic, not a true resync — a DMA buffer boundary
            // is not a packet boundary in bufferFill mode. Linux aborts and
            // restarts the whole AR context here (ar_context_abort); with
            // stitching in place this path is reachable only on genuine
            // corruption, so any of these log lines is itself a finding.
            logGarbageHead(bufferStart, offset, bufferSize, parsed.error());
            commitConsumed(info.descriptorIndex, bufferSize);
            bufferHandled = true;
            break;
        }

        if (bufferHandled) {
            continue;
        }
        if (!committedPartial) {
            commitConsumed(info.descriptorIndex, offset);
        }
        if (offset == startOffset) {
            // No forward progress: the head of the readable window is an
            // unstitchable fragment whose tail has not been DMA'd yet. All
            // later traffic sits behind it in stream order, so end the pass;
            // the next interrupt retries the stitch (BufferRing::Dequeue
            // re-presents a full head once its successor shows data).
            break;
        }
    }

    return stats;
}

} // namespace ASFW::Async::Rx
