#include "ROMReader.hpp"
#include "../Discovery/DiscoveryValues.hpp"  // For ConfigROM address constants
#include "../Async/Interfaces/IFireWireBus.hpp"
#include "../Common/FWCommon.hpp"  // For FW:: strong types
#include "../Logging/Logging.hpp"
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
    // BIB is 20 bytes (5 quadlets): Q0(header) Q1("1394") Q2(caps) Q3(GUID_hi) Q4(GUID_lo)
    constexpr uint32_t kBIBLength = 20;
    constexpr uint32_t kBIBQuadlets = 5;

    // Validate Config ROM address space (must be 0xFFFF for CSR space)
    if (ConfigROMAddr::kAddressHi != 0xFFFF) {
        ASFW_LOG(ConfigROM, "ERROR: Config ROM addressHigh changed from 0xFFFF to 0x%04x!", ConfigROMAddr::kAddressHi);
        return;
    }

    ASFW_LOG(ConfigROM, "ReadBIB: node=%u gen=%u addr=0x%04x:%08x (quadlet-only mode)",
             nodeId, generation,
             ConfigROMAddr::kAddressHi, ConfigROMAddr::kAddressLo);

    // Context for aggregating quadlet reads
    struct QuadletReadContext {
        CompletionCallback userCallback;
        uint8_t nodeId;
        Generation generation;
        std::vector<uint32_t> buffer;  // Accumulate quadlets here
        uint8_t quadletIndex{0};
        uint8_t successCount{0};
        ROMReader* reader;  // For recursive calls
        std::function<void()> issueNextQuadlet;  // Store recursive lambda here
    };

    auto* ctx = new QuadletReadContext{callback, nodeId, generation};
    ctx->buffer.resize(kBIBQuadlets, 0);
    ctx->reader = this;

    // Lambda to issue next quadlet read (capture by reference for recursion)
    ctx->issueNextQuadlet = [ctx, kBIBQuadlets, kBIBLength]() {
        if (ctx->quadletIndex >= kBIBQuadlets) {
            // All quadlets read - invoke callback with aggregated result
            ReadResult result{};
            result.success = (ctx->successCount == kBIBQuadlets);
            result.nodeId = ctx->nodeId;
            result.generation = ctx->generation;
            result.address = ConfigROMAddr::kAddressLo;
            result.data = ctx->buffer.data();
            result.dataLength = kBIBLength;

            if (result.success) {
                ASFW_LOG(ConfigROM, "ReadBIB complete: node=%u gen=%u len=%u bytes",
                         ctx->nodeId, ctx->generation, result.dataLength);
            } else {
                ASFW_LOG(ConfigROM, "ReadBIB FAILED: node=%u gen=%u success=%u/%u",
                         ctx->nodeId, ctx->generation, ctx->successCount, kBIBQuadlets);
            }

            if (ctx->userCallback) {
                ctx->userCallback(result);
            }

            delete ctx;
            return;
        }

        // CRITICAL: Skip Q1 (bus name "1394") and prefill (Apple behavior)
        // This avoids early-timeout/ack-busy traps on flaky hardware
        if (ctx->quadletIndex == 1) {
            // Store in host order so later UI/debug prints look natural.
            // ('1','3','9','4') = 0x31 0x33 0x39 0x34
            static constexpr uint32_t kFWBIBBusName =
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                0x34393331u; // "1394" as it should appear on little-endian host
#else
                0x31333934u; // big-endian host
#endif
            ASFW_LOG(ConfigROM, "Skipping Q1, prefilling with '1394' (Apple pattern)");
            ctx->buffer[1] = kFWBIBBusName;
            ctx->successCount++;
            ctx->quadletIndex = 2;  // Skip to Q2
            ctx->reader->ScheduleNextQuadlet(ctx);
            return;
        }

        // Build FWAddress for this quadlet
        Async::FWAddress addr{
            ConfigROMAddr::kAddressHi,
            ConfigROMAddr::kAddressLo + (ctx->quadletIndex * 4),
            static_cast<uint16_t>(ctx->nodeId)  // nodeID field
        };

        ASFW_LOG(ConfigROM, "BIB Q%u: node=%u addr=%04x:%08x",
                 ctx->quadletIndex, ctx->nodeId, addr.addressHi, addr.addressLo);

        // Completion handler (interface callback: no AsyncHandle parameter)
        Async::InterfaceCompletionCallback completionHandler =
            [ctx, kBIBQuadlets](Async::AsyncStatus status,
                               std::span<const uint8_t> responsePayload) {

            ASFW_LOG(ConfigROM, "BIB Q%u done: status=%u respLen=%zu",
                     ctx->quadletIndex, static_cast<uint32_t>(status), responsePayload.size());

            // Check status before continuing (prevents deadlock on error)
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG(ConfigROM, "BIB Q%u failed with status=%u, aborting",
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
                ASFW_LOG(ConfigROM, "BIB Q%u invalid length=%zu, aborting",
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

            // Copy quadlet into buffer
            const uint32_t* quadlet = reinterpret_cast<const uint32_t*>(responsePayload.data());
            ctx->buffer[ctx->quadletIndex] = *quadlet;
            ctx->successCount++;
            ctx->quadletIndex++;

            // Continue reading next quadlet
            ctx->reader->ScheduleNextQuadlet(ctx);
        };

        // Issue quadlet read via interface (always S100 for Config ROM)
        auto handle = ctx->reader->bus_.ReadQuad(
            FW::Generation{static_cast<uint32_t>(ctx->generation)},
            FW::NodeId{ctx->nodeId},
            addr,
            FW::FwSpeed::S100,  // Always S100 for Config ROM (Apple behavior)
            completionHandler
        );

        if (!handle) {
            ASFW_LOG(ConfigROM, "BIB Q%u submission failed (node=%u)",
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

    // Start reading first quadlet
    ScheduleNextQuadlet(ctx);
}

void ROMReader::ReadRootDirQuadlets(uint8_t nodeId,
                                    Generation generation,
                                    FwSpeed speed,
                                    uint32_t offsetBytes,
                                    uint32_t count,
                                    CompletionCallback callback) {
    // Validate Config ROM address space (must be 0xFFFF for CSR space)
    if (ConfigROMAddr::kAddressHi != 0xFFFF) {
        ASFW_LOG(ConfigROM, "ERROR: Config ROM addressHigh changed from 0xFFFF to 0x%04x!", ConfigROMAddr::kAddressHi);
        return;
    }

    ASFW_LOG(ConfigROM, "ReadRootDir: node=%u gen=%u offset=%u count=%u (quadlet-only mode)",
             nodeId, generation, offsetBytes, count);

    // Context for aggregating quadlet reads
    struct QuadletReadContext {
        CompletionCallback userCallback;
        uint8_t nodeId;
        Generation generation;
        uint32_t baseAddress;
        uint32_t quadletCount;
        std::vector<uint32_t> buffer;  // Accumulate quadlets here
        uint32_t quadletIndex{0};
        uint32_t successCount{0};
        bool headerFirstMode{false};    // count==0 → read header then expand
        ROMReader* reader;  // For recursive calls
        std::function<void()> issueNextQuadlet;  // Store recursive lambda here
    };

    auto* ctx = new QuadletReadContext{
        callback,
        nodeId,
        generation,
        ConfigROMAddr::kAddressLo + offsetBytes,
        count
    };
    // If caller passes count==0, interpret as "read the root-dir header first".
    if (count == 0) {
        ctx->headerFirstMode = true;
        ctx->quadletCount = 1;          // header only for now
    }
    ctx->buffer.resize(ctx->quadletCount, 0);
    ctx->reader = this;

    // Lambda to issue next quadlet read
    ctx->issueNextQuadlet = [ctx]() {
        if (ctx->quadletIndex >= ctx->quadletCount) {
            // All quadlets read - invoke callback with aggregated result
            ReadResult result{};
            result.success = (ctx->successCount == ctx->quadletCount);
            result.nodeId = ctx->nodeId;
            result.generation = ctx->generation;
            result.address = ctx->baseAddress;
            result.data = ctx->buffer.data();
            result.dataLength = ctx->quadletCount * 4;

            if (result.success) {
                ASFW_LOG(ConfigROM, "ReadRootDir complete: node=%u gen=%u len=%u bytes (%u quads)",
                         ctx->nodeId, ctx->generation, result.dataLength, ctx->quadletCount);
            } else {
                ASFW_LOG(ConfigROM, "ReadRootDir FAILED: node=%u gen=%u success=%u/%u",
                         ctx->nodeId, ctx->generation, ctx->successCount, ctx->quadletCount);
            }

            if (ctx->userCallback) {
                ctx->userCallback(result);
            }

            delete ctx;
            return;
        }

        // Build FWAddress for this quadlet
        Async::FWAddress addr{
            ConfigROMAddr::kAddressHi,
            ctx->baseAddress + (ctx->quadletIndex * 4),
            static_cast<uint16_t>(ctx->nodeId)
        };

        ASFW_LOG(ConfigROM, "RootDir Q%u: node=%u addr=%04x:%08x",
                 ctx->quadletIndex, ctx->nodeId, addr.addressHi, addr.addressLo);

        // Completion handler
        Async::InterfaceCompletionCallback completionHandler =
            [ctx](Async::AsyncStatus status, std::span<const uint8_t> responsePayload) {

            ASFW_LOG(ConfigROM, "RootDir Q%u done: status=%u respLen=%zu",
                     ctx->quadletIndex, static_cast<uint32_t>(status), responsePayload.size());

            if (status != Async::AsyncStatus::kSuccess) {
                // Graceful end-of-directory tolerance (Focusrite case):
                // If we've successfully read at least the header, treat address_error
                // (or other errors) as natural EOF instead of hard failure.
                // This handles firmware that reports entry_count+1 in the header.
                if (ctx->successCount > 0) {
                    const uint32_t validQuadlets = ctx->successCount;
                    ASFW_LOG(ConfigROM,
                             "RootDir Q%u non-success (status=%u), treating as EOF after %u valid quadlets",
                             ctx->quadletIndex, static_cast<uint32_t>(status), validQuadlets);

                    ReadResult result{};
                    result.success = true;  // Success with partial data
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

                // Hard failure if header itself failed
                ASFW_LOG(ConfigROM, "RootDir Q%u failed with status=%u, aborting",
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
                // Graceful EOF: if we have valid data, complete successfully
                if (ctx->successCount > 0) {
                    const uint32_t validQuadlets = ctx->successCount;
                    ASFW_LOG(ConfigROM,
                             "RootDir Q%u invalid length=%zu, treating as EOF after %u valid quadlets",
                             ctx->quadletIndex, responsePayload.size(), validQuadlets);

                    ReadResult result{};
                    result.success = true;  // Success with partial data
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

                // Hard failure if header itself failed
                ASFW_LOG(ConfigROM, "RootDir Q%u invalid length=%zu, aborting",
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

            // Copy quadlet into buffer
            const uint32_t* quadlet = reinterpret_cast<const uint32_t*>(responsePayload.data());
            ctx->buffer[ctx->quadletIndex] = *quadlet;
            ctx->successCount++;
            ctx->quadletIndex++;

            // If we were in header-first mode and just fetched the header,
            // expand the plan to fetch the remaining entries based on its length.
            if (ctx->headerFirstMode && ctx->quadletIndex == 1) {
                const uint32_t hdr_be = ctx->buffer[0];
                const uint32_t hdr    = BE32_TO_HOST(hdr_be);
                const uint16_t entryCount = static_cast<uint16_t>(hdr & 0xFFFF); // 1212: length=entry count
                ASFW_LOG(ConfigROM, "RootDir header parsed: entries=%u (hdr=0x%08x)", entryCount, hdr);

                if (entryCount > 0) {
                    const uint32_t total = 1u + static_cast<uint32_t>(entryCount);
                    ctx->buffer.resize(total, 0);
                    ctx->quadletCount = total;
                    // Continue reading the remaining directory entries.
                    ctx->reader->ScheduleNextQuadlet(ctx);
                    return;
                }
                // No entries — finish with just the header.
            }

            // Continue reading next quadlet
            ctx->reader->ScheduleNextQuadlet(ctx);
        };

        // Issue quadlet read via interface (always S100 for Config ROM)
        auto handle = ctx->reader->bus_.ReadQuad(
            FW::Generation{static_cast<uint32_t>(ctx->generation)},
            FW::NodeId{ctx->nodeId},
            addr,
            FW::FwSpeed::S100,
            completionHandler
        );

        if (!handle) {
            ASFW_LOG(ConfigROM, "RootDir Q%u submission failed (node=%u)",
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

    // Start reading first quadlet
    ScheduleNextQuadlet(ctx);
}

} // namespace ASFW::Discovery
