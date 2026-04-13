#include "AsyncSubsystem.hpp"

#include "Commands/LockCommand.hpp"
#include "Commands/PhyCommand.hpp"
#include "Commands/ReadCommand.hpp"
#include "Commands/WriteCommand.hpp"

#include "../Logging/Logging.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

namespace ASFW::Async {

AsyncHandle AsyncSubsystem::Read(const ReadParams& params, CompletionCallback callback) {
    return ReadCommand{params, std::move(callback)}.Submit(*this);
}

AsyncHandle AsyncSubsystem::Write(const WriteParams& params, CompletionCallback callback) {
    return WriteCommand{params, std::move(callback)}.Submit(*this);
}

AsyncHandle AsyncSubsystem::Lock(const LockParams& params,
                                 uint16_t extendedTCode,
                                 CompletionCallback callback) {
    return LockCommand{params, extendedTCode, std::move(callback)}.Submit(*this);
}

namespace {
struct CompareSwapOperandStorage {
    std::array<uint32_t, 2> beOperands{};
    uint32_t compareHost{0};
};
} // namespace

AsyncHandle AsyncSubsystem::CompareSwap(const CompareSwapParams& params,
                                        CompareSwapCallback callback) {
    auto storage = std::make_shared<CompareSwapOperandStorage>();
    storage->compareHost      = params.compareValue;
    storage->beOperands[0]    = OSSwapHostToBigInt32(params.compareValue);
    storage->beOperands[1]    = OSSwapHostToBigInt32(params.swapValue);

    LockParams lockParams{};
    lockParams.destinationID   = params.destinationID;
    lockParams.addressHigh     = params.addressHigh;
    lockParams.addressLow      = params.addressLow;
    lockParams.operand         = storage->beOperands.data();
    lockParams.operandLength   = static_cast<uint32_t>(storage->beOperands.size() * sizeof(uint32_t));
    lockParams.responseLength  = sizeof(uint32_t);
    lockParams.speedCode       = params.speedCode;

    const uint16_t kExtendedTCodeCompareSwap = 0x02;

    CompletionCallback internalCallback = [callback, storage](AsyncHandle,
                                                             AsyncStatus status,
                                                             uint8_t,
                                                             std::span<const uint8_t> payload) {
        if (status != AsyncStatus::kSuccess) {
            callback(status, 0u, false);
            return;
        }

        if (payload.size() != sizeof(uint32_t)) {
            callback(AsyncStatus::kHardwareError, 0u, false);
            return;
        }

        uint32_t raw = 0;
        std::memcpy(&raw, payload.data(), sizeof(uint32_t));
        uint32_t oldValueHost = OSSwapBigToHostInt32(raw);
        const bool matched = (oldValueHost == storage->compareHost);
        callback(AsyncStatus::kSuccess, oldValueHost, matched);
    };

    return Lock(lockParams, kExtendedTCodeCompareSwap, std::move(internalCallback));
}

AsyncHandle AsyncSubsystem::PhyRequest(const PhyParams& params,
                                      CompletionCallback callback) {
    return PhyCommand{params, std::move(callback)}.Submit(*this);
}

AsyncHandle AsyncSubsystem::Stream(const StreamParams& /* params */) {
    ASFW_LOG_ERROR(Async, "Stream packets not yet implemented");
    return AsyncHandle{0};
}

} // namespace ASFW::Async
