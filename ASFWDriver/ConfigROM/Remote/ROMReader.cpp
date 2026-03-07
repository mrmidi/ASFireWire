#include "../ROMReader.hpp"

#include "../../Async/Interfaces/IFireWireBus.hpp"
#include "../../Common/FWCommon.hpp" // For FW:: strong types
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>

#include <bit>
#include <cstring>
#include <deque>
#include <memory>
#include <utility>

namespace ASFW::Discovery {

namespace {

constexpr bool IsValidQuadletPayload(size_t payloadSizeBytes) noexcept {
    return payloadSizeBytes == ASFW::ConfigROM::kQuadletBytes;
}

constexpr uint16_t ClampHeaderFirstEntryCount(uint16_t entryCount) noexcept {
    if (entryCount > ASFW::ConfigROM::kHeaderFirstMaxEntries) {
        return static_cast<uint16_t>(ASFW::ConfigROM::kHeaderFirstMaxEntries);
    }
    return entryCount;
}

constexpr uint32_t BusNameQuadletHostOrder() noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(ASFW::FW::kBusNameQuadlet);
    }
    return ASFW::FW::kBusNameQuadlet;
}

[[nodiscard]] bool CanTreatAsEOF(ROMReader::QuadletReadPolicy policy, Async::AsyncStatus status,
                                 size_t payloadSizeBytes, uint32_t completedQuadlets) noexcept {
    if (policy != ROMReader::QuadletReadPolicy::AllowPartialEOF) {
        return false;
    }
    if (completedQuadlets == 0) {
        return false;
    }
    return status != Async::AsyncStatus::kSuccess || !IsValidQuadletPayload(payloadSizeBytes);
}

} // namespace

ROMReader::ROMReader(Async::IFireWireBus& bus, OSSharedPtr<IODispatchQueue> dispatchQueue)
    : bus_(bus), dispatchQueue_(std::move(dispatchQueue)) {}

void ROMReader::ReadQuadletsBE(uint8_t nodeId, Generation generation, FwSpeed speed,
                               uint32_t offsetBytes, uint32_t quadletCount,
                               CompletionCallback callback, QuadletReadPolicy policy) {
    ReadQuadletsBEImpl(bus_, dispatchQueue_, nodeId, generation, speed, offsetBytes, quadletCount,
                       std::move(callback), policy);
}

void ROMReader::ReadQuadletsBEImpl(Async::IFireWireBus& bus,
                                   OSSharedPtr<IODispatchQueue> dispatchQueue, uint8_t nodeId,
                                   Generation generation, FwSpeed speed, uint32_t offsetBytes,
                                   uint32_t quadletCount, CompletionCallback callback,
                                   QuadletReadPolicy policy) {
    if (FW::ConfigROMAddr::kAddressHi != 0xFFFF) {
        ASFW_LOG_V0(ConfigROM, "ERROR: Config ROM addressHigh changed from 0xFFFF to 0x%04x!",
                    FW::ConfigROMAddr::kAddressHi);
        if (callback) {
            ReadResult out{};
            out.success = false;
            out.nodeId = nodeId;
            out.generation = generation;
            out.address = FW::ConfigROMAddr::kAddressLo + offsetBytes;
            out.status = Async::AsyncStatus::kHardwareError;
            callback(std::move(out));
        }
        return;
    }

    if (!callback) {
        return;
    }

    if (quadletCount == 0) {
        ReadResult out{};
        out.success = true;
        out.nodeId = nodeId;
        out.generation = generation;
        out.address = FW::ConfigROMAddr::kAddressLo + offsetBytes;
        out.status = Async::AsyncStatus::kSuccess;
        callback(std::move(out));
        return;
    }

    auto ctx = std::make_shared<QuadletReadContext>();
    ctx->userCallback = std::move(callback);
    ctx->bus = &bus;
    ctx->dispatchQueue = std::move(dispatchQueue);
    ctx->nodeId = nodeId;
    ctx->generation = generation;
    ctx->speed = speed;
    ctx->baseAddress = FW::ConfigROMAddr::kAddressLo + offsetBytes;
    ctx->quadletCount = quadletCount;
    ctx->policy = policy;
    ctx->buffer.resize(quadletCount, 0);

    ScheduleQuadletReadStep(ctx);
}

void ROMReader::ScheduleQuadletReadStep(const std::shared_ptr<QuadletReadContext>& ctx) {
    if (ctx->quadletIndex >= ctx->quadletCount) {
        EmitQuadletReadResult(ctx,
                              /*success=*/true, Async::AsyncStatus::kSuccess, ctx->quadletCount);
        return;
    }

    const uint32_t addressLo =
        ctx->baseAddress + (ctx->quadletIndex * ASFW::ConfigROM::kQuadletBytes);
    Async::FWAddress addr{FW::ConfigROMAddr::kAddressHi, addressLo,
                          static_cast<uint16_t>(ctx->nodeId)};

    Async::InterfaceCompletionCallback completionHandler =
        [ctx](Async::AsyncStatus status, std::span<const uint8_t> payload) mutable {
            HandleQuadletReadComplete(ctx, status, payload);
        };

    if (ctx->bus == nullptr) {
        EmitQuadletReadResult(ctx, /*success=*/false, Async::AsyncStatus::kHardwareError, 0);
        return;
    }

    const auto handle = ctx->bus->ReadQuad(ctx->generation, FW::NodeId{ctx->nodeId}, addr,
                                           ctx->speed, std::move(completionHandler));
    if (!handle) {
        EmitQuadletReadResult(ctx, /*success=*/false, Async::AsyncStatus::kHardwareError, 0);
    }
}

void ROMReader::HandleQuadletReadComplete(const std::shared_ptr<QuadletReadContext>& ctx,
                                          Async::AsyncStatus status,
                                          std::span<const uint8_t> responsePayload) {
    if (CanTreatAsEOF(ctx->policy, status, responsePayload.size(), ctx->successCount)) {
        EmitQuadletReadResult(ctx, /*success=*/true, status, ctx->successCount);
        return;
    }

    if (status != Async::AsyncStatus::kSuccess || !IsValidQuadletPayload(responsePayload.size())) {
        EmitQuadletReadResult(ctx, /*success=*/false, status, 0);
        return;
    }

    uint32_t quadlet = 0;
    std::memcpy(&quadlet, responsePayload.data(), sizeof(quadlet));
    ctx->buffer[ctx->quadletIndex] = quadlet;
    ctx->successCount++;
    ctx->quadletIndex++;

    ScheduleNextQuadlet(ctx->dispatchQueue, [ctx]() { ScheduleQuadletReadStep(ctx); });
}

void ROMReader::EmitQuadletReadResult(const std::shared_ptr<QuadletReadContext>& ctx, bool success,
                                      Async::AsyncStatus status, uint32_t quadletsToReturn) {
    ReadResult out{};
    out.success = success;
    out.nodeId = ctx->nodeId;
    out.generation = ctx->generation;
    out.address = ctx->baseAddress;
    out.status = status;

    if (quadletsToReturn > 0) {
        if (quadletsToReturn < ctx->buffer.size()) {
            ctx->buffer.resize(quadletsToReturn);
        }
        out.quadletsBE = std::move(ctx->buffer);
    }

    if (ctx->userCallback) {
        ctx->userCallback(std::move(out));
    }
}

void ROMReader::ScheduleNextQuadlet(OSSharedPtr<IODispatchQueue> dispatchQueue,
                                    std::function<void()> task) {
    if (!task) {
        return;
    }

    if (!dispatchQueue) {
#ifdef ASFW_HOST_TEST
        // Trampoline to avoid unbounded recursion without spawning detached threads.
        struct HostTrampoline {
            std::deque<std::function<void()>> queue;
            bool draining{false};
        };

        thread_local HostTrampoline trampoline;
        trampoline.queue.push_back(std::move(task));
        if (trampoline.draining) {
            return;
        }
        trampoline.draining = true;
        while (!trampoline.queue.empty()) {
            auto next = std::move(trampoline.queue.front());
            trampoline.queue.pop_front();
            next();
        }
        trampoline.draining = false;
        return;
#else
        task();
        return;
#endif
    }

    auto queue = std::move(dispatchQueue);
    auto captured = std::make_shared<std::function<void()>>(std::move(task));
    queue->DispatchAsync(^{
      (*captured)();
    });
}

void ROMReader::ReadBIB(uint8_t nodeId, Generation generation, FwSpeed speed,
                        CompletionCallback callback) {
    if (!callback) {
        return;
    }

    auto* bus = &bus_;
    auto dispatchQueue = dispatchQueue_;
    auto completionHolder = std::make_shared<CompletionCallback>(std::move(callback));

    ReadQuadletsBEImpl(
        *bus, dispatchQueue, nodeId, generation, speed,
        /*offsetBytes=*/0,
        /*quadletCount=*/1,
        [bus, dispatchQueue, nodeId, generation, speed, completionHolder](ReadResult q0) mutable {
            if (!completionHolder || !*completionHolder) {
                return;
            }

            if (!q0.success || q0.quadletsBE.size() != 1) {
                ReadResult out{};
                out.success = false;
                out.nodeId = nodeId;
                out.generation = generation;
                out.address = FW::ConfigROMAddr::kAddressLo;
                out.status = q0.status;
                (*completionHolder)(std::move(out));
                return;
            }

            const uint32_t q1Host = BusNameQuadletHostOrder();

            ReadQuadletsBEImpl(
                *bus, dispatchQueue, nodeId, generation, speed,
                /*offsetBytes=*/8,
                /*quadletCount=*/3,
                [nodeId, generation, completionHolder, q0 = std::move(q0),
                 q1Host](ReadResult q2_4) mutable {
                    if (!completionHolder || !*completionHolder) {
                        return;
                    }

                    if (!q2_4.success || q2_4.quadletsBE.size() != 3) {
                        ReadResult out{};
                        out.success = false;
                        out.nodeId = nodeId;
                        out.generation = generation;
                        out.address = FW::ConfigROMAddr::kAddressLo;
                        out.status = q2_4.status;
                        (*completionHolder)(std::move(out));
                        return;
                    }

                    ReadResult out{};
                    out.success = true;
                    out.nodeId = nodeId;
                    out.generation = generation;
                    out.address = FW::ConfigROMAddr::kAddressLo;
                    out.status = Async::AsyncStatus::kSuccess;

                    out.quadletsBE.reserve(kBIBQuadlets);
                    out.quadletsBE.push_back(q0.quadletsBE[0]);
                    out.quadletsBE.push_back(q1Host);
                    out.quadletsBE.insert(out.quadletsBE.end(), q2_4.quadletsBE.begin(),
                                          q2_4.quadletsBE.end());
                    (*completionHolder)(std::move(out));
                },
                QuadletReadPolicy::AllOrNothing);
        },
        QuadletReadPolicy::AllOrNothing);
}

void ROMReader::ReadRootDirQuadlets(uint8_t nodeId, Generation generation, FwSpeed speed,
                                    uint32_t offsetBytes, uint32_t count,
                                    CompletionCallback callback) {
    if (!callback) {
        return;
    }

    auto* bus = &bus_;
    auto dispatchQueue = dispatchQueue_;

    if (count != 0) {
        ReadQuadletsBEImpl(*bus, dispatchQueue, nodeId, generation, speed, offsetBytes, count,
                           std::move(callback), QuadletReadPolicy::AllowPartialEOF);
        return;
    }

    auto completionHolder = std::make_shared<CompletionCallback>(std::move(callback));

    ReadQuadletsBEImpl(
        *bus, dispatchQueue, nodeId, generation, speed, offsetBytes,
        /*quadletCount=*/1,
        [bus, dispatchQueue, nodeId, generation, speed, offsetBytes,
         completionHolder](ReadResult header) mutable {
            if (!completionHolder || !*completionHolder) {
                return;
            }

            if (!header.success || header.quadletsBE.size() != 1) {
                ReadResult out{};
                out.success = false;
                out.nodeId = nodeId;
                out.generation = generation;
                out.address = FW::ConfigROMAddr::kAddressLo + offsetBytes;
                out.status = header.status;
                (*completionHolder)(std::move(out));
                return;
            }

            const uint32_t hdrBe = header.quadletsBE[0];
            const uint32_t hdr = OSSwapBigToHostInt32(hdrBe);
            auto entryCount = static_cast<uint16_t>((hdr >> 16) & 0xFFFFU);
            entryCount = ClampHeaderFirstEntryCount(entryCount);

            if (entryCount == 0) {
                ReadResult out{};
                out.success = true;
                out.nodeId = nodeId;
                out.generation = generation;
                out.address = FW::ConfigROMAddr::kAddressLo + offsetBytes;
                out.status = Async::AsyncStatus::kSuccess;
                out.quadletsBE = std::move(header.quadletsBE);
                (*completionHolder)(std::move(out));
                return;
            }

            ReadQuadletsBEImpl(
                *bus, dispatchQueue, nodeId, generation, speed,
                offsetBytes + ASFW::ConfigROM::kQuadletBytes, entryCount,
                [nodeId, generation, offsetBytes, hdrBe,
                 completionHolder](ReadResult entries) mutable {
                    if (!completionHolder || !*completionHolder) {
                        return;
                    }

                    ReadResult out{};
                    out.success = true;
                    out.nodeId = nodeId;
                    out.generation = generation;
                    out.address = FW::ConfigROMAddr::kAddressLo + offsetBytes;
                    out.status = entries.status;

                    out.quadletsBE.reserve(1 + entries.quadletsBE.size());
                    out.quadletsBE.push_back(hdrBe);
                    if (entries.success) {
                        out.quadletsBE.insert(out.quadletsBE.end(), entries.quadletsBE.begin(),
                                              entries.quadletsBE.end());
                    }

                    (*completionHolder)(std::move(out));
                },
                QuadletReadPolicy::AllowPartialEOF);
        },
        QuadletReadPolicy::AllOrNothing);
}

} // namespace ASFW::Discovery
