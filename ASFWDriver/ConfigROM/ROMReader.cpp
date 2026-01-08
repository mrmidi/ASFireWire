#include "ROMReader.hpp"
#include "../Discovery/DiscoveryValues.hpp"  // For ConfigROM address constants
#include "../Async/Interfaces/IFireWireBus.hpp"
#include "../Common/FWCommon.hpp"  // For FW:: strong types
#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"
#include <memory>
#include <vector>
#include <functional>

// Local BE→host helper (DriverKit is little-endian on Apple Silicon)
namespace {
static inline uint32_t BE32_TO_HOST(uint32_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(v);
#else
    return v;
#endif
}
} // namespace

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
    constexpr uint32_t kBIBLength = 20;
    constexpr uint32_t kBIBQuadlets = 5;

    if (ConfigROMAddr::kAddressHi != 0xFFFF) {
        ASFW_LOG_V0(ConfigROM, "ERROR: Config ROM addressHigh changed from 0xFFFF to 0x%04x!", ConfigROMAddr::kAddressHi);
        return;
    }

    ASFW_LOG_V3(ConfigROM, "ReadBIB: node=%u gen=%u addr=0x%04x:%08x",
             nodeId, generation,
             ConfigROMAddr::kAddressHi, ConfigROMAddr::kAddressLo);

    struct QuadletReadContext {
        CompletionCallback userCallback;
        uint8_t nodeId;
        Generation generation;
        std::vector<uint32_t> buffer;
        uint8_t quadletIndex{0};
        uint8_t successCount{0};
        ROMReader* reader;
        std::function<void()> issueNextQuadlet;
    };

    auto* ctx = new QuadletReadContext{callback, nodeId, generation};
    ctx->buffer.resize(kBIBQuadlets, 0);
    ctx->reader = this;

    ctx->issueNextQuadlet = [ctx, kBIBQuadlets, kBIBLength]() {
        if (ctx->quadletIndex >= kBIBQuadlets) {
            ReadResult result{};
            result.success = (ctx->successCount == kBIBQuadlets);
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

            delete ctx;
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
            ctx->reader->ScheduleNextQuadlet(ctx);
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
            [ctx, kBIBQuadlets](Async::AsyncStatus status,
                               std::span<const uint8_t> responsePayload) {

            ASFW_LOG_V3(ConfigROM, "BIB Q%u done: status=%u respLen=%zu",
                     ctx->quadletIndex, static_cast<uint32_t>(status), responsePayload.size());

            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG_V0(ConfigROM, "BIB Q%u failed with status=%u, aborting",
                         ctx->quadletIndex, static_cast<uint32_t>(status));

                ReadResult result;
                result.success = false;
                result.generation = ctx->generation;
                result.nodeId = ctx->nodeId;
                if (ctx->userCallback) {
                    ctx->userCallback(result);
                }
                delete ctx;
                return;
            }

            if (responsePayload.size() != 4) {
                ASFW_LOG_V0(ConfigROM, "BIB Q%u invalid length=%zu, aborting",
                         ctx->quadletIndex, responsePayload.size());

                ReadResult result;
                result.success = false;
                result.generation = ctx->generation;
                result.nodeId = ctx->nodeId;
                if (ctx->userCallback) {
                    ctx->userCallback(result);
                }
                delete ctx;
                return;
            }

            const uint32_t* quadlet = reinterpret_cast<const uint32_t*>(responsePayload.data());
            ctx->buffer[ctx->quadletIndex] = *quadlet;
            ctx->successCount++;
            ctx->quadletIndex++;

            ctx->reader->ScheduleNextQuadlet(ctx);
        };

        auto handle = ctx->reader->bus_.ReadQuad(
            FW::Generation{static_cast<uint32_t>(ctx->generation)},
            FW::NodeId{ctx->nodeId},
            addr,
            FW::FwSpeed::S100,
            completionHandler
        );

        if (!handle) {
            ASFW_LOG_V0(ConfigROM, "BIB Q%u submission failed (node=%u)",
                     ctx->quadletIndex, ctx->nodeId);

            ReadResult result{};
            result.success = false;
            result.nodeId = ctx->nodeId;
            result.generation = ctx->generation;
            if (ctx->userCallback) {
                ctx->userCallback(result);
            }
            delete ctx;
            return;
        }
    };

    ScheduleNextQuadlet(ctx);
}

void ROMReader::ReadRootDirQuadlets(uint8_t nodeId,
                                    Generation generation,
                                    FwSpeed speed,
                                    uint32_t offsetBytes,
                                    uint32_t count,
                                    CompletionCallback callback) {
    if (ConfigROMAddr::kAddressHi != 0xFFFF) {
        ASFW_LOG_V0(ConfigROM, "ERROR: Config ROM addressHigh changed from 0xFFFF to 0x%04x!", ConfigROMAddr::kAddressHi);
        return;
    }

    ASFW_LOG_V3(ConfigROM, "ReadRootDir: node=%u gen=%u offset=%u count=%u",
             nodeId, generation, offsetBytes, count);

    struct QuadletReadContext {
        CompletionCallback userCallback;
        uint8_t nodeId;
        Generation generation;
        uint32_t baseAddress;
        uint32_t quadletCount;
        std::vector<uint32_t> buffer;
        uint32_t quadletIndex{0};
        uint32_t successCount{0};
        bool headerFirstMode{false};
        ROMReader* reader;
        std::function<void()> issueNextQuadlet;
    };

    auto* ctx = new QuadletReadContext{
        callback,
        nodeId,
        generation,
        ConfigROMAddr::kAddressLo + offsetBytes,
        count
    };
    ctx->quadletCount = count;
    if (count == 0) {
        ctx->headerFirstMode = true;
        ctx->quadletCount = 1;
    }
    ctx->buffer.resize(ctx->quadletCount, 0);
    ctx->reader = this;

    ctx->issueNextQuadlet = [ctx]() {
        if (ctx->quadletIndex >= ctx->quadletCount) {
            ReadResult result{};
            result.success = (ctx->successCount == ctx->quadletCount);
            result.nodeId = ctx->nodeId;
            result.generation = ctx->generation;
            result.address = ctx->baseAddress;
            result.data = ctx->buffer.data();
            result.dataLength = ctx->quadletCount * 4;

            if (result.success) {
                ASFW_LOG_V2(ConfigROM, "ReadRootDir complete: node=%u gen=%u len=%u bytes (%u quads)",
                         ctx->nodeId, ctx->generation, result.dataLength, ctx->quadletCount);
            } else {
                ASFW_LOG_V0(ConfigROM, "ReadRootDir FAILED: node=%u gen=%u success=%u/%u",
                         ctx->nodeId, ctx->generation, ctx->successCount, ctx->quadletCount);
            }

            if (ctx->userCallback) {
                ctx->userCallback(result);
            }

            delete ctx;
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
            [ctx](Async::AsyncStatus status, std::span<const uint8_t> responsePayload) {

            ASFW_LOG_V3(ConfigROM, "RootDir Q%u done: status=%u respLen=%zu",
                     ctx->quadletIndex, static_cast<uint32_t>(status), responsePayload.size());

            if (status != Async::AsyncStatus::kSuccess) {
                if (ctx->successCount > 0) {
                    const uint32_t validQuadlets = ctx->successCount;
                    ASFW_LOG_V2(ConfigROM,
                             "RootDir Q%u non-success (status=%u), treating as EOF after %u valid quadlets",
                             ctx->quadletIndex, static_cast<uint32_t>(status), validQuadlets);

                    ReadResult result{};
                    result.success = true;
                    result.nodeId = ctx->nodeId;
                    result.generation = ctx->generation;
                    result.address = ctx->baseAddress;
                    result.data = ctx->buffer.data();
                    result.dataLength = validQuadlets * 4;

                    if (ctx->userCallback) {
                        ctx->userCallback(result);
                    }

                    delete ctx;
                    return;
                }

                ASFW_LOG_V0(ConfigROM, "RootDir Q%u failed with status=%u, aborting",
                         ctx->quadletIndex, static_cast<uint32_t>(status));

                ReadResult result;
                result.success = false;
                result.generation = ctx->generation;
                result.nodeId = ctx->nodeId;
                if (ctx->userCallback) {
                    ctx->userCallback(result);
                }
                delete ctx;
                return;
            }

            if (responsePayload.size() != 4) {
                if (ctx->successCount > 0) {
                    const uint32_t validQuadlets = ctx->successCount;
                    ASFW_LOG_V2(ConfigROM,
                             "RootDir Q%u invalid length=%zu, treating as EOF after %u valid quadlets",
                             ctx->quadletIndex, responsePayload.size(), validQuadlets);

                    ReadResult result{};
                    result.success = true;
                    result.nodeId = ctx->nodeId;
                    result.generation = ctx->generation;
                    result.address = ctx->baseAddress;
                    result.data = ctx->buffer.data();
                    result.dataLength = validQuadlets * 4;

                    if (ctx->userCallback) {
                        ctx->userCallback(result);
                    }

                    delete ctx;
                    return;
                }

                ASFW_LOG_V0(ConfigROM, "RootDir Q%u invalid length=%zu, aborting",
                         ctx->quadletIndex, responsePayload.size());

                ReadResult result;
                result.success = false;
                result.generation = ctx->generation;
                result.nodeId = ctx->nodeId;
                if (ctx->userCallback) {
                    ctx->userCallback(result);
                }
                delete ctx;
                return;
            }

            const uint32_t* quadlet = reinterpret_cast<const uint32_t*>(responsePayload.data());
            ctx->buffer[ctx->quadletIndex] = *quadlet;
            ctx->successCount++;
            ctx->quadletIndex++;

            if (ctx->headerFirstMode && ctx->quadletIndex == 1) {
                const uint32_t hdr_be = ctx->buffer[0];
                const uint32_t hdr    = BE32_TO_HOST(hdr_be);
                const uint16_t entryCount = static_cast<uint16_t>(hdr & 0xFFFF);
                ASFW_LOG_V3(ConfigROM, "RootDir header parsed: entries=%u (hdr=0x%08x)", entryCount, hdr);

                if (entryCount > 0) {
                    const uint32_t total = 1u + static_cast<uint32_t>(entryCount);
                    ctx->buffer.resize(total, 0);
                    ctx->quadletCount = total;
                    ctx->reader->ScheduleNextQuadlet(ctx);
                    return;
                }
            }

            ctx->reader->ScheduleNextQuadlet(ctx);
        };

        auto handle = ctx->reader->bus_.ReadQuad(
            FW::Generation{static_cast<uint32_t>(ctx->generation)},
            FW::NodeId{ctx->nodeId},
            addr,
            FW::FwSpeed::S100,
            completionHandler
        );

        if (!handle) {
            ASFW_LOG_V0(ConfigROM, "RootDir Q%u submission failed (node=%u)",
                     ctx->quadletIndex, ctx->nodeId);

            ReadResult result{};
            result.success = false;
            result.nodeId = ctx->nodeId;
            result.generation = ctx->generation;
            if (ctx->userCallback) {
                ctx->userCallback(result);
            }
            delete ctx;
            return;
        }
    };

    ScheduleNextQuadlet(ctx);
}

} // namespace ASFW::Discovery
