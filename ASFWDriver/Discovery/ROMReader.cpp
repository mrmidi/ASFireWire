#include "ROMReader.hpp"
#include "DiscoveryValues.hpp"  // For ConfigROM address constants and READ_MODE_QUAD
#include "../Async/AsyncSubsystem.hpp"
#include "../Core/ControllerTypes.hpp"  // For ComposeNodeID helper
#include "../Logging/Logging.hpp"
#include <memory>
#include <vector>
#include <functional>

namespace ASFW::Discovery {

ROMReader::ROMReader(Async::AsyncSubsystem& asyncSubsystem)
    : async_(asyncSubsystem) {
}

void ROMReader::ReadBIB(uint8_t nodeId,
                        Generation generation,
                        FwSpeed speed,
                        uint16_t busBase16,
                        CompletionCallback callback) {
    // BIB is 20 bytes (5 quadlets): Q0(header) Q1("1394") Q2(caps) Q3(GUID_hi) Q4(GUID_lo)
    constexpr uint32_t kBIBLength = 20;
    constexpr uint32_t kBIBQuadlets = 5;
    
    // Validate Config ROM address space (must be 0xFFFF for CSR space)
    if (ConfigROMAddr::kAddressHi != 0xFFFF) {
        ASFW_LOG(Discovery, "ERROR: Config ROM addressHigh changed from 0xFFFF to 0x%04x!", ConfigROMAddr::kAddressHi);
        return;
    }
    
    // Compose full 16-bit destinationID: (bus<<6) | node
    const uint16_t destinationID = Driver::ComposeNodeID(busBase16, nodeId);
    const uint16_t busNum = static_cast<uint16_t>((busBase16 >> 6) & 0x3FFu);
    
    // Speed lookup table: S100=0→100, S200=1→200, S400=2→400, S800=3→800
    static constexpr uint16_t kSpeedMbit[4] = {100, 200, 400, 800};
    const uint16_t speedMbit = kSpeedMbit[static_cast<uint8_t>(speed) & 0x3];
    
    ASFW_LOG(Discovery, "ReadBIB: node=%u gen=%u speed=S%u addr=0x%04x:%08x dest=0x%04x (bus=%u) mode=%{public}s",
             nodeId, generation, speedMbit,
             ConfigROMAddr::kAddressHi, ConfigROMAddr::kAddressLo, destinationID, busNum,
             READ_MODE_QUAD ? "QUADLET-ONLY" : "BLOCK");
    
#if READ_MODE_QUAD
    // Quadlet-only mode: Read 4 quadlets individually and aggregate
    struct QuadletReadContext {
        CompletionCallback userCallback;
        uint8_t nodeId;
        Generation generation;
        uint16_t destinationID;
        uint8_t speedCode;
        std::vector<uint32_t> buffer;  // Accumulate quadlets here
        uint8_t quadletIndex{0};
        uint8_t successCount{0};
        ROMReader* reader;  // For recursive calls
        std::function<void()> issueNextQuadlet;  // Store recursive lambda here
    };
    
    auto* ctx = new QuadletReadContext{callback, nodeId, generation, destinationID, SpeedToCode(speed)};
    ctx->buffer.resize(kBIBQuadlets, 0);
    ctx->reader = this;
    
    // Lambda to issue next quadlet read (capture by reference for recursion)
    ctx->issueNextQuadlet = [ctx, kBIBQuadlets, kBIBLength]() {
        ASFW_LOG(Discovery, "🔄 [ROMReader] issueNextQuadlet ENTRY: quadlet=%u/%u success=%u/%u ctx=%p",
                 ctx->quadletIndex, kBIBQuadlets, ctx->successCount, kBIBQuadlets, ctx);

        if (ctx->quadletIndex >= kBIBQuadlets) {
            // All quadlets read - invoke callback with aggregated result
            ReadResult result{};
            result.success = (ctx->successCount == kBIBQuadlets);
            result.nodeId = ctx->nodeId;
            result.generation = ctx->generation;
            result.address = ConfigROMAddr::kAddressLo;
            result.data = ctx->buffer.data();
            result.dataLength = kBIBLength;
            
            ASFW_LOG(Discovery, "[ROMReader] BIB aggregate: success=%d total=%uB (node=%u gen=%u)",
                     result.success, result.dataLength, ctx->nodeId, ctx->generation);
            
            if (result.success) {
                ASFW_LOG(Discovery, "ReadBIB complete (quadlets): node=%u gen=%u len=%u bytes",
                         ctx->nodeId, ctx->generation, result.dataLength);
            } else {
                ASFW_LOG(Discovery, "ReadBIB FAILED (quadlets): node=%u gen=%u success=%u/%u",
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
            static constexpr uint32_t kFWBIBBusName = 0x31333934;  // "1394" big-endian
            ASFW_LOG(Discovery, "⏭️  [ROMReader] Skipping Q1, prefilling with '1394' (Apple pattern)");
            ctx->buffer[1] = kFWBIBBusName;
            ctx->successCount++;
            ctx->quadletIndex = 2;  // Skip to Q2
            ASFW_LOG(Discovery, "🔁 [ROMReader] Recursing to issue Q2 (quadletIndex now=%u)", ctx->quadletIndex);
            // Recurse immediately to issue Q2
            ctx->issueNextQuadlet();
            return;
        }
        
        // Issue quadlet read for current index
        Async::ReadParams params{};
        params.destinationID = ctx->destinationID;
        params.addressHigh = ConfigROMAddr::kAddressHi;
        params.addressLow = ConfigROMAddr::kAddressLo + (ctx->quadletIndex * 4);
        params.length = 4;  // Single quadlet
        params.speedCode = 0;  // S100 for Config ROM (Apple behavior)
        
        ASFW_LOG(Discovery, "[ROMReader] BIB Q%u issue: dst=0x%04x addr=%04x:%08x len=%u gen=%u",
                 ctx->quadletIndex, ctx->destinationID, params.addressHigh, params.addressLow,
                 params.length, ctx->generation);

        // Completion handler that captures context via lambda
        Async::CompletionCallback completionHandler = [ctx, kBIBQuadlets](Async::AsyncHandle handle,
                                                             Async::AsyncStatus status,
                                                             std::span<const uint8_t> responsePayload) {
            ASFW_LOG(Discovery, "📥 [ROMReader] COMPLETION HANDLER ENTRY: Q%u status=%u respLen=%zu handle=0x%x ctx=%p",
                     ctx->quadletIndex, static_cast<uint32_t>(status), responsePayload.size(),
                     handle.value, ctx);

            ASFW_LOG(Discovery, "[ROMReader] BIB Q%u done: status=%u respLen=%zu (successCount=%u/%u)",
                     ctx->quadletIndex, static_cast<uint32_t>(status), responsePayload.size(),
                     ctx->successCount, kBIBQuadlets);

            // CRITICAL FIX: Check status BEFORE continuing to prevent re-entry deadlock
            // If we don't check status and always call issueNextQuadlet(), then:
            // 1. Callback invoked with status=kTimeout (from WithTransaction which holds lock)
            // 2. ROMReader tries to issue next quadlet
            // 3. Calls RegisterTx → Allocate → IOLockLock(lock_)
            // 4. DEADLOCK! Lock already held by WithTransaction
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG(Discovery, "⚠️ [ROMReader] BIB Q%u failed with status=%u, aborting",
                         ctx->quadletIndex, static_cast<uint32_t>(status));

                // Call completion callback with error
                ReadResult result;
                result.success = false;
                result.generation = ctx->generation;
                result.nodeId = ctx->nodeId;
                ctx->userCallback(result);
                delete ctx;  // Clean up context
                return;  // CRITICAL: Don't continue!
            }

            if (responsePayload.size() != 4) {
                ASFW_LOG(Discovery, "⚠️ [ROMReader] BIB Q%u invalid length=%zu, aborting",
                         ctx->quadletIndex, responsePayload.size());

                ReadResult result;
                result.success = false;
                result.generation = ctx->generation;
                result.nodeId = ctx->nodeId;
                ctx->userCallback(result);
                delete ctx;  // Clean up context
                return;  // CRITICAL: Don't continue!
            }

            // Copy quadlet into buffer
            const uint32_t* quadlet = reinterpret_cast<const uint32_t*>(responsePayload.data());
            ctx->buffer[ctx->quadletIndex] = *quadlet;
            ctx->successCount++;
            ctx->quadletIndex++;

            // Check if we've read all BIB quadlets
            if (ctx->quadletIndex >= kBIBQuadlets) {
                ASFW_LOG(Discovery, "✅ [ROMReader] BIB complete: read %u/%u quadlets",
                         ctx->successCount, kBIBQuadlets);
                // issueNextQuadlet() will handle completion via early return
            }

            ASFW_LOG(Discovery, "🔁 [ROMReader] About to recurse from BIB completion: quadletIndex now=%u", ctx->quadletIndex);

            // CRITICAL FIX: Direct call - PostToWorkloop() doesn't wake DriverKit workloop
            // Safe because: (1) we're in completion callback not interrupt, (2) lambda has
            // early return preventing deep recursion
            // CRITICAL: Only called after status check above - prevents re-entry deadlock!
            ctx->issueNextQuadlet();

            ASFW_LOG(Discovery, "✅ [ROMReader] Returned from BIB recursion");
        };

        ASFW_LOG(Discovery, "📤 [ROMReader] About to call Read (DIRECT, no queue) for BIB Q%u", ctx->quadletIndex);

        // Use DIRECT Read (same path as AsyncRead) - bypass ReadWithRetry queue
        ctx->reader->async_.Read(params, completionHandler);

        ASFW_LOG(Discovery, "↩️  [ROMReader] BIB issueNextQuadlet EXIT: Read returned (async)");
    };
    
    // Start reading first quadlet
    ctx->issueNextQuadlet();
#else
    // Block read mode (simplified for callback testing)
    ASFW_LOG(Discovery, "📖 [ROMReader] ReadBIB BLOCK MODE: node=%u gen=%u addr=%04x:%08x len=%u",
             nodeId, generation, ConfigROMAddr::kAddressHi, ConfigROMAddr::kAddressLo, kBIBLength);

    Async::ReadParams params{};
    params.destinationID = destinationID;  // Full (bus<<6)|node composition
    params.addressHigh = ConfigROMAddr::kAddressHi;  // Must be 0xFFFF
    params.addressLow = ConfigROMAddr::kAddressLo;
    params.length = kBIBLength;
    params.speedCode = SpeedToCode(speed);  // Use peer-specific speed

    ASFW_LOG(Discovery, "📋 [ROMReader] Block read params: dest=0x%04x addr=%04x:%08x len=%u speed=%u",
             params.destinationID, params.addressHigh, params.addressLow, params.length, params.speedCode);

    // Use simple std::function callback (matches quadlet mode signature)
    Async::CompletionCallback completionHandler = [callback, nodeId, generation](
                                Async::AsyncHandle handle,
                                Async::AsyncStatus status,
                                std::span<const uint8_t> responsePayload) {
        ASFW_LOG(Discovery, "📥 [ROMReader] BLOCK CALLBACK INVOKED: handle=0x%x status=%u payloadLen=%zu node=%u gen=%u",
                 handle.value, static_cast<uint32_t>(status), responsePayload.size(), nodeId, generation);

        ReadResult result{};
        result.success = (status == Async::AsyncStatus::kSuccess);
        result.nodeId = nodeId;
        result.generation = generation;
        result.address = ConfigROMAddr::kAddressLo;
        result.data = reinterpret_cast<const uint32_t*>(responsePayload.data());
        result.dataLength = static_cast<uint32_t>(responsePayload.size());

        if (result.success) {
            ASFW_LOG(Discovery, "✅ [ROMReader] ReadBIB complete: node=%u gen=%u len=%u bytes",
                     nodeId, generation, result.dataLength);
        } else {
            ASFW_LOG(Discovery, "❌ [ROMReader] ReadBIB FAILED: node=%u gen=%u status=%u",
                     nodeId, generation, static_cast<uint32_t>(status));
        }

        ASFW_LOG(Discovery, "🔔 [ROMReader] About to invoke user callback (OnBIBComplete)");

        if (callback) {
            callback(result);
            ASFW_LOG(Discovery, "✅ [ROMReader] User callback invoked successfully");
        } else {
            ASFW_LOG(Discovery, "⚠️  [ROMReader] User callback is NULL!");
        }
    };

    ASFW_LOG(Discovery, "📤 [ROMReader] About to call ReadWithRetry (block mode)");

    // Use queued retry for sequential execution and automatic retry
    Async::RetryPolicy retryPolicy = Async::RetryPolicy::Default();
    async_.ReadWithRetry(params, retryPolicy, completionHandler);

    ASFW_LOG(Discovery, "↩️  [ROMReader] ReadBIB (block mode) returned from ReadWithRetry");
#endif
}

void ROMReader::ReadRootDirQuadlets(uint8_t nodeId,
                                    Generation generation,
                                    FwSpeed speed,
                                    uint16_t busBase16,
                                    uint32_t offsetBytes,
                                    uint32_t count,
                                    CompletionCallback callback) {
    const uint32_t lengthBytes = count * 4;  // Convert quadlet count to bytes
    
    // Validate Config ROM address space (must be 0xFFFF for CSR space)
    if (ConfigROMAddr::kAddressHi != 0xFFFF) {
        ASFW_LOG(Discovery, "ERROR: Config ROM addressHigh changed from 0xFFFF to 0x%04x!", ConfigROMAddr::kAddressHi);
        return;
    }
    
    // Compose full 16-bit destinationID: (bus<<6) | node
    const uint16_t destinationID = Driver::ComposeNodeID(busBase16, nodeId);
    const uint16_t busNum = static_cast<uint16_t>((busBase16 >> 6) & 0x3FFu);
    
    // Speed lookup table: S100=0→100, S200=1→200, S400=2→400, S800=3→800
    static constexpr uint16_t kSpeedMbit[4] = {100, 200, 400, 800};
    const uint16_t speedMbit = kSpeedMbit[static_cast<uint8_t>(speed) & 0x3];
    
    ASFW_LOG(Discovery, "ReadRootDir: node=%u gen=%u speed=S%u offset=%u count=%u dest=0x%04x (bus=%u) mode=%{public}s",
             nodeId, generation, speedMbit, offsetBytes, count, destinationID, busNum,
             READ_MODE_QUAD ? "QUADLET-ONLY" : "BLOCK");
    
#if READ_MODE_QUAD
    // Quadlet-only mode: Read each quadlet individually and aggregate
    struct QuadletReadContext {
        CompletionCallback userCallback;
        uint8_t nodeId;
        Generation generation;
        uint16_t destinationID;
        uint8_t speedCode;
        uint32_t baseAddress;
        uint32_t quadletCount;
        std::vector<uint32_t> buffer;  // Accumulate quadlets here
        uint32_t quadletIndex{0};
        uint32_t successCount{0};
        ROMReader* reader;  // For recursive calls
        std::function<void()> issueNextQuadlet;  // Store recursive lambda here
    };
    
    auto* ctx = new QuadletReadContext{callback, nodeId, generation, destinationID, SpeedToCode(speed),
                                       ConfigROMAddr::kAddressLo + offsetBytes, count};
    ctx->buffer.resize(count, 0);
    ctx->reader = this;
    
    // Lambda to issue next quadlet read (capture by reference for recursion)
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
            
            ASFW_LOG(Discovery, "[ROMReader] RootDir aggregate: success=%d total=%uB (node=%u gen=%u count=%u)",
                     result.success, result.dataLength, ctx->nodeId, ctx->generation, ctx->quadletCount);
            
            if (result.success) {
                ASFW_LOG(Discovery, "ReadRootDir complete (quadlets): node=%u gen=%u len=%u bytes (%u quads)",
                         ctx->nodeId, ctx->generation, result.dataLength, ctx->quadletCount);
            } else {
                ASFW_LOG(Discovery, "ReadRootDir FAILED (quadlets): node=%u gen=%u success=%u/%u",
                         ctx->nodeId, ctx->generation, ctx->successCount, ctx->quadletCount);
            }
            
            if (ctx->userCallback) {
                ctx->userCallback(result);
            }
            
            delete ctx;
            return;
        }
        
        // Issue quadlet read for current index
        Async::ReadParams params{};
        params.destinationID = ctx->destinationID;
        params.addressHigh = ConfigROMAddr::kAddressHi;
        params.addressLow = ctx->baseAddress + (ctx->quadletIndex * 4);
        params.length = 4;  // Single quadlet
        params.speedCode = 0;  // S100 for Config ROM (Apple behavior)
        
        ASFW_LOG(Discovery, "[ROMReader] RootDir Q%u issue: dst=0x%04x addr=%04x:%08x len=%u gen=%u",
                 ctx->quadletIndex, ctx->destinationID, params.addressHigh, params.addressLow,
                 params.length, ctx->generation);
        
        // Completion handler that captures context via lambda
        Async::CompletionCallback completionHandler = [ctx](Async::AsyncHandle handle,
                                                             Async::AsyncStatus status,
                                                             std::span<const uint8_t> responsePayload) {
            ASFW_LOG(Discovery, "[ROMReader] RootDir Q%u done: status=%u respLen=%zu (successCount=%u/%u)",
                     ctx->quadletIndex, static_cast<uint32_t>(status), responsePayload.size(),
                     ctx->successCount, ctx->quadletCount);

            // CRITICAL FIX: Check status BEFORE continuing to prevent re-entry deadlock
            // If we don't check status and always call issueNextQuadlet(), then:
            // 1. Callback invoked with status=kTimeout (from WithTransaction which holds lock)
            // 2. ROMReader tries to issue next quadlet
            // 3. Calls RegisterTx → Allocate → IOLockLock(lock_)
            // 4. DEADLOCK! Lock already held by WithTransaction
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG(Discovery, "⚠️ [ROMReader] RootDir Q%u failed with status=%u, aborting",
                         ctx->quadletIndex, static_cast<uint32_t>(status));

                // Call completion callback with error
                ReadResult result;
                result.success = false;
                result.generation = ctx->generation;
                result.nodeId = ctx->nodeId;
                ctx->userCallback(result);
                delete ctx;  // Clean up context
                return;  // CRITICAL: Don't continue!
            }

            if (responsePayload.size() != 4) {
                ASFW_LOG(Discovery, "⚠️ [ROMReader] RootDir Q%u invalid length=%zu, aborting",
                         ctx->quadletIndex, responsePayload.size());

                ReadResult result;
                result.success = false;
                result.generation = ctx->generation;
                result.nodeId = ctx->nodeId;
                ctx->userCallback(result);
                delete ctx;  // Clean up context
                return;  // CRITICAL: Don't continue!
            }

            // Only now copy quadlet and continue
            const uint32_t* quadlet = reinterpret_cast<const uint32_t*>(responsePayload.data());
            ctx->buffer[ctx->quadletIndex] = *quadlet;
            ctx->successCount++;
            ctx->quadletIndex++;

            // CRITICAL FIX: Direct call - PostToWorkloop() doesn't wake DriverKit workloop
            // Safe because: (1) we're in completion callback not interrupt, (2) lambda has
            // early return preventing deep recursion, (3) ReadWithRetry internally queues
            ctx->issueNextQuadlet();  // Safe now - only called on success
        };

        ASFW_LOG(Discovery, "📤 [ROMReader] About to call Read (DIRECT, no queue) for RootDir Q%u", ctx->quadletIndex);

        // Use DIRECT Read (same path as AsyncRead) - bypass ReadWithRetry queue
        ctx->reader->async_.Read(params, completionHandler);

        ASFW_LOG(Discovery, "↩️  [ROMReader] RootDir issueNextQuadlet EXIT: Read returned (async)");
    };

    // Start reading first quadlet
    ctx->issueNextQuadlet();
#else
    // Block read mode (simplified - matches ReadBIB signature)
    Async::ReadParams params{};
    params.destinationID = destinationID;  // Full (bus<<6)|node composition
    params.addressHigh = ConfigROMAddr::kAddressHi;  // Must be 0xFFFF
    params.addressLow = ConfigROMAddr::kAddressLo + offsetBytes;
    params.length = lengthBytes;
    params.speedCode = SpeedToCode(speed);  // Use peer-specific speed

    // Use simple std::function callback (matches new signature)
    const uint32_t address = params.addressLow;
    Async::CompletionCallback completionHandler = [callback, nodeId, generation, address](
                                Async::AsyncHandle handle,
                                Async::AsyncStatus status,
                                std::span<const uint8_t> responsePayload) {
        ReadResult result{};
        result.success = (status == Async::AsyncStatus::kSuccess);
        result.nodeId = nodeId;
        result.generation = generation;
        result.address = address;
        result.data = reinterpret_cast<const uint32_t*>(responsePayload.data());
        result.dataLength = static_cast<uint32_t>(responsePayload.size());

        if (result.success) {
            ASFW_LOG(Discovery, "ReadRootDir complete: node=%u gen=%u len=%u bytes (%u quads)",
                     nodeId, generation, result.dataLength, result.dataLength / 4);
        } else {
            ASFW_LOG(Discovery, "ReadRootDir FAILED: node=%u gen=%u status=%u",
                     nodeId, generation, static_cast<uint32_t>(status));
        }

        if (callback) {
            callback(result);
        }
    };

    // Use queued retry for sequential execution and automatic retry
    Async::RetryPolicy retryPolicy = Async::RetryPolicy::Default();
    async_.ReadWithRetry(params, retryPolicy, completionHandler);
#endif
}

uint8_t ROMReader::SpeedToCode(FwSpeed speed) {
    return static_cast<uint8_t>(speed);
}

} // namespace ASFW::Discovery

