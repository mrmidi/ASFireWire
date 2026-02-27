#include "ROMReader.hpp"
#include "ConfigROMPolicies.hpp"
#include "../Discovery/DiscoveryValues.hpp"  // For ConfigROM address constants
#include "../Async/Interfaces/IFireWireBus.hpp"
#include "../Common/FWCommon.hpp"  // For FW:: strong types
#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"
#include <DriverKit/IOLib.h>
#include <cstring>
#include <memory>
#include <vector>
#include <functional>
#include <utility>

namespace ASFW::Discovery {

ROMReader::ROMReader(Async::IFireWireBus& bus,
                     OSSharedPtr<IODispatchQueue> dispatchQueue)
    : bus_(bus)
    , dispatchQueue_(dispatchQueue) {
}

void ROMReader::ReadBIB(uint8_t nodeId,
                        Generation generation,
                        FwSpeed speed,
                        CompletionCallback callback) {
    (void)speed;

    if (ConfigROMAddr::kAddressHi != 0xFFFF) {
        ASFW_LOG_V0(ConfigROM, "ERROR: Config ROM addressHigh changed from 0xFFFF to 0x%04x!", ConfigROMAddr::kAddressHi);
        return;
    }

    ASFW_LOG_V3(ConfigROM, "ReadBIB: node=%u gen=%u addr=0x%04x:%08x",
             nodeId, generation,
             ConfigROMAddr::kAddressHi, ConfigROMAddr::kAddressLo);

    auto ctx = std::make_shared<BIBReadContext>();
    ctx->userCallback = std::move(callback);
    ctx->nodeId = nodeId;
    ctx->generation = generation;
    ctx->buffer.resize(kBIBQuadlets, 0);

    ScheduleBIBStep(ctx);
}

void ROMReader::ScheduleBIBStep(const std::shared_ptr<BIBReadContext>& ctx) {
    if (ctx->quadletIndex >= kBIBQuadlets) {
        const bool success = (ctx->successCount == kBIBQuadlets);
        EmitBIBResult(ctx, success);
        return;
    }

    if (ctx->quadletIndex == 1) {
        static constexpr uint32_t kFWBIBBusName =
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            0x34393331u;
#else
            0x31333934u;
#endif
        ASFW_LOG_V3(ConfigROM, "Skipping Q1, prefilling with '1394'");
        ctx->buffer[1] = kFWBIBBusName;
        ctx->successCount++;
        ctx->quadletIndex = 2;
        ScheduleNextQuadlet([this, ctx]() {
            ScheduleBIBStep(ctx);
        });
        return;
    }

    Async::FWAddress addr{
        ConfigROMAddr::kAddressHi,
        ConfigROMAddr::kAddressLo + (ctx->quadletIndex * 4),
        static_cast<uint16_t>(ctx->nodeId)
    };

    ASFW_LOG_V3(ConfigROM, "BIB Q%u: node=%u addr=%04x:%08x",
             ctx->quadletIndex, ctx->nodeId, addr.addressHi, addr.addressLo);

    Async::InterfaceCompletionCallback completionHandler =
        [this, ctx](Async::AsyncStatus status,
                    std::span<const uint8_t> responsePayload) {
            HandleBIBReadComplete(ctx, status, responsePayload);
        };

    auto handle = bus_.ReadQuad(
        FW::Generation{static_cast<uint32_t>(ctx->generation)},
        FW::NodeId{ctx->nodeId},
        addr,
        FW::FwSpeed::S100,
        completionHandler
    );

    if (!handle) {
        ASFW_LOG_V0(ConfigROM, "BIB Q%u submission failed (node=%u)",
                 ctx->quadletIndex, ctx->nodeId);
        EmitBIBResult(ctx, false);
    }
}

void ROMReader::HandleBIBReadComplete(const std::shared_ptr<BIBReadContext>& ctx,
                                      Async::AsyncStatus status,
                                      std::span<const uint8_t> responsePayload) {
    ASFW_LOG_V3(ConfigROM, "BIB Q%u done: status=%u respLen=%zu",
             ctx->quadletIndex, static_cast<uint32_t>(status), responsePayload.size());

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG_V0(ConfigROM, "BIB Q%u failed with status=%u, aborting",
                 ctx->quadletIndex, static_cast<uint32_t>(status));
        EmitBIBResult(ctx, false);
        return;
    }

    if (responsePayload.size() != 4) {
        ASFW_LOG_V0(ConfigROM, "BIB Q%u invalid length=%zu, aborting",
                 ctx->quadletIndex, responsePayload.size());
        EmitBIBResult(ctx, false);
        return;
    }

    uint32_t quadlet = 0;
    std::memcpy(&quadlet, responsePayload.data(), sizeof(quadlet));
    ctx->buffer[ctx->quadletIndex] = quadlet;
    ctx->successCount++;
    ctx->quadletIndex++;

    ScheduleNextQuadlet([this, ctx]() {
        ScheduleBIBStep(ctx);
    });
}

void ROMReader::EmitBIBResult(const std::shared_ptr<BIBReadContext>& ctx,
                              bool success) const {
    ReadResult result{};
    result.success = success;
    result.nodeId = ctx->nodeId;
    result.generation = ctx->generation;
    result.address = ConfigROMAddr::kAddressLo;
    result.data = ctx->buffer.data();
    result.dataLength = kBIBLength;

    if (result.success) {
        ASFW_LOG_V2(ConfigROM, "ReadBIB complete: node=%u gen=%u len=%u bytes",
                 ctx->nodeId, ctx->generation, result.dataLength);
    } else {
        ASFW_LOG_V0(ConfigROM, "ReadBIB FAILED: node=%u gen=%u success=%u/%u",
                 ctx->nodeId, ctx->generation, ctx->successCount, kBIBQuadlets);
    }

    if (ctx->userCallback) {
        ctx->userCallback(result);
    }
}

void ROMReader::ReadRootDirQuadlets(uint8_t nodeId,
                                    Generation generation,
                                    FwSpeed speed,
                                    uint32_t offsetBytes,
                                    uint32_t count,
                                    CompletionCallback callback) {
    (void)speed;

    if (ConfigROMAddr::kAddressHi != 0xFFFF) {
        ASFW_LOG_V0(ConfigROM, "ERROR: Config ROM addressHigh changed from 0xFFFF to 0x%04x!", ConfigROMAddr::kAddressHi);
        return;
    }

    ASFW_LOG_V3(ConfigROM, "ReadRootDir: node=%u gen=%u offset=%u count=%u",
             nodeId, generation, offsetBytes, count);

    auto ctx = std::make_shared<RootDirReadContext>();
    ctx->userCallback = std::move(callback);
    ctx->nodeId = nodeId;
    ctx->generation = generation;
    ctx->baseAddress = ConfigROMAddr::kAddressLo + offsetBytes;
    ctx->quadletCount = count;
    if (count == 0) {
        ctx->headerFirstMode = true;
        ctx->quadletCount = 1;
    }
    ctx->buffer.resize(ctx->quadletCount, 0);

    ScheduleRootDirStep(ctx);
}

void ROMReader::ScheduleRootDirStep(const std::shared_ptr<RootDirReadContext>& ctx) {
    if (ctx->quadletIndex >= ctx->quadletCount) {
        const bool success = (ctx->successCount == ctx->quadletCount);
        EmitRootDirResult(ctx, success, ctx->quadletCount);
        return;
    }

    Async::FWAddress addr{
        ConfigROMAddr::kAddressHi,
        ctx->baseAddress + (ctx->quadletIndex * 4),
        static_cast<uint16_t>(ctx->nodeId)
    };

    ASFW_LOG_V3(ConfigROM, "RootDir Q%u: node=%u addr=%04x:%08x",
             ctx->quadletIndex, ctx->nodeId, addr.addressHi, addr.addressLo);

    Async::InterfaceCompletionCallback completionHandler =
        [this, ctx](Async::AsyncStatus status,
                    std::span<const uint8_t> responsePayload) {
            HandleRootDirReadComplete(ctx, status, responsePayload);
        };

    auto handle = bus_.ReadQuad(
        FW::Generation{static_cast<uint32_t>(ctx->generation)},
        FW::NodeId{ctx->nodeId},
        addr,
        FW::FwSpeed::S100,
        completionHandler
    );

    if (!handle) {
        ASFW_LOG_V0(ConfigROM, "RootDir Q%u submission failed (node=%u)",
                 ctx->quadletIndex, ctx->nodeId);
        EmitRootDirFailure(ctx);
    }
}

void ROMReader::HandleRootDirReadComplete(const std::shared_ptr<RootDirReadContext>& ctx,
                                          Async::AsyncStatus status,
                                          std::span<const uint8_t> responsePayload) {
    ASFW_LOG_V3(ConfigROM, "RootDir Q%u done: status=%u respLen=%zu",
             ctx->quadletIndex, static_cast<uint32_t>(status), responsePayload.size());

    if (ShortReadResolutionPolicy::ShouldTreatAsEOF(status,
                                                    responsePayload.size(),
                                                    ctx->successCount)) {
        const uint32_t validQuadlets = ctx->successCount;
        ASFW_LOG_V2(ConfigROM,
                 "RootDir Q%u short read/end-of-data (status=%u len=%zu), keeping %u valid quadlets",
                 ctx->quadletIndex,
                 static_cast<uint32_t>(status),
                 responsePayload.size(),
                 validQuadlets);
        EmitRootDirResult(ctx, true, validQuadlets);
        return;
    }

    if (ShortReadResolutionPolicy::IsReadFailure(status,
                                                 responsePayload.size(),
                                                 ctx->successCount)) {
        if (status != Async::AsyncStatus::kSuccess) {
            ASFW_LOG_V0(ConfigROM, "RootDir Q%u failed with status=%u, aborting",
                     ctx->quadletIndex, static_cast<uint32_t>(status));
        } else {
            ASFW_LOG_V0(ConfigROM, "RootDir Q%u invalid length=%zu, aborting",
                     ctx->quadletIndex, responsePayload.size());
        }
        EmitRootDirFailure(ctx);
        return;
    }

    uint32_t quadlet = 0;
    std::memcpy(&quadlet, responsePayload.data(), sizeof(quadlet));
    ctx->buffer[ctx->quadletIndex] = quadlet;
    ctx->successCount++;
    ctx->quadletIndex++;

    if (ctx->headerFirstMode && ctx->quadletIndex == 1) {
        const uint32_t hdr_be = ctx->buffer[0];
        const uint32_t hdr = OSSwapBigToHostInt32(hdr_be);
        auto entryCount = static_cast<uint16_t>((hdr >> 16) & 0xFFFFu);
        ASFW_LOG_V3(ConfigROM, "RootDir header parsed: entries=%u (hdr=0x%08x)", entryCount, hdr);

        if (entryCount > 0) {
            const auto clampedEntries = ShortReadResolutionPolicy::ClampHeaderFirstEntryCount(entryCount);
            if (clampedEntries != entryCount) {
                ASFW_LOG_V2(ConfigROM, "RootDir headerFirst cap: entries=%u -> %u", entryCount, clampedEntries);
                entryCount = clampedEntries;
            }

            const uint32_t total = 1u + static_cast<uint32_t>(entryCount);
            ctx->buffer.resize(total, 0);
            ctx->quadletCount = total;
        }
    }

    ScheduleNextQuadlet([this, ctx]() {
        ScheduleRootDirStep(ctx);
    });
}

void ROMReader::EmitRootDirFailure(const std::shared_ptr<RootDirReadContext>& ctx) const {
    ReadResult result{};
    result.success = false;
    result.generation = ctx->generation;
    result.nodeId = ctx->nodeId;
    if (ctx->userCallback) {
        ctx->userCallback(result);
    }
}

void ROMReader::EmitRootDirResult(const std::shared_ptr<RootDirReadContext>& ctx,
                                  bool success,
                                  uint32_t quadletCountForResult) const {
    ReadResult result{};
    result.success = success;
    result.nodeId = ctx->nodeId;
    result.generation = ctx->generation;
    result.address = ctx->baseAddress;
    result.data = ctx->buffer.data();
    result.dataLength = quadletCountForResult * ASFW::ConfigROM::kQuadletBytes;

    if (result.success) {
        ASFW_LOG_V2(ConfigROM, "ReadRootDir complete: node=%u gen=%u len=%u bytes (%u quads)",
                 ctx->nodeId, ctx->generation, result.dataLength, quadletCountForResult);
    } else {
        ASFW_LOG_V0(ConfigROM, "ReadRootDir FAILED: node=%u gen=%u success=%u/%u",
                 ctx->nodeId, ctx->generation, ctx->successCount, ctx->quadletCount);
    }

    if (ctx->userCallback) {
        ctx->userCallback(result);
    }
}

} // namespace ASFW::Discovery
