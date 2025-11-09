#pragma once

#include <cstdint>
#include <type_traits>

namespace ASFW::Async {

/**
 * @brief Completion strategy for async transactions.
 *
 * FireWire async transactions follow IEEE 1394-1995 two-phase protocol:
 * 1. AT (Asynchronous Transmit): gotAck equivalent - ACK from target
 * 2. AR (Asynchronous Receive): gotPacket equivalent - Response packet
 *
 * Different transaction types complete at different phases:
 * - Read operations: Always complete on AR (need response data)
 * - Write quadlet: Usually complete on AT (ack_complete)
 * - Write block: May complete on AT or AR depending on ack code
 * - Lock operations: Always complete on AR (need old value response)
 *
 * Reference: Apple IOFireWireFamily
 * - IOFWReadQuadCommand::gotAck() - stores ack, doesn't complete
 * - IOFWReadQuadCommand::gotPacket() - completes command with data
 * - IOFWWriteCommand::gotAck() - may complete if ack_complete
 * - IOFWWriteCommand::gotPacket() - completes if deferred notify
 */
enum class CompletionStrategy : uint8_t {
    /**
     * Complete on AT acknowledgment only (unified transaction).
     *
     * Used for: Write quadlet with ack_complete (0x0)
     *
     * Behavior:
     * - OnATCompletion(): Check ack code, complete if successful
     * - OnARResponse(): Ignore (unexpected for unified transactions)
     *
     * State flow: Submitted → ATPosted → ATCompleted → Completed
     */
    CompleteOnAT = 0,

    /**
     * Complete on AR response only (split transaction).
     *
     * Used for:
     * - Read quadlet (tCode 0x4): Need response data
     * - Read block (tCode 0x5): Need response data
     * - Lock operations (tCode 0x9): Need old value response
     *
     * Behavior:
     * - OnATCompletion(): Store ack code, transition to AwaitingAR, don't complete
     * - OnARResponse(): Extract data, complete transaction
     *
     * State flow: Submitted → ATPosted → ATCompleted → AwaitingAR → ARReceived → Completed
     *
     * Reference: IOFWReadQuadCommand.cpp gotAck() + gotPacket() pattern
     */
    CompleteOnAR = 1,

    /**
     * Require both AT and AR paths (complex split transaction).
     *
     * Used for:
     * - Write block with ack_pending (0x1): AT confirms receipt, AR confirms completion
     * - Deferred operations: Target acknowledges, then processes and responds
     *
     * Behavior:
     * - OnATCompletion(): Validate ack code, transition to AwaitingAR if ack_pending
     * - OnARResponse(): Complete transaction
     *
     * State flow: Submitted → ATPosted → ATCompleted → AwaitingAR → ARReceived → Completed
     */
    RequireBoth = 2
};

/**
 * @brief Trait to determine if a strategy requires AR response.
 */
constexpr bool RequiresARResponse(CompletionStrategy strategy) noexcept {
    return strategy == CompletionStrategy::CompleteOnAR ||
           strategy == CompletionStrategy::RequireBoth;
}

/**
 * @brief Trait to determine if a strategy processes AT completion.
 */
constexpr bool ProcessesATCompletion(CompletionStrategy strategy) noexcept {
    return strategy == CompletionStrategy::CompleteOnAT ||
           strategy == CompletionStrategy::RequireBoth;
}

/**
 * @brief Trait to determine if AT completion should immediately complete the transaction.
 */
constexpr bool CompletesOnATAck(CompletionStrategy strategy) noexcept {
    return strategy == CompletionStrategy::CompleteOnAT;
}

/**
 * @brief Convert CompletionStrategy to string for logging.
 */
constexpr const char* ToString(CompletionStrategy strategy) noexcept {
    switch (strategy) {
        case CompletionStrategy::CompleteOnAT: return "CompleteOnAT";
        case CompletionStrategy::CompleteOnAR: return "CompleteOnAR";
        case CompletionStrategy::RequireBoth: return "RequireBoth";
    }
    return "Unknown";
}

// =============================================================================
// Concepts for Compile-Time Strategy Validation
// =============================================================================

/**
 * @brief Concept for commands that complete on AR response.
 *
 * Usage:
 * @code
 * template<ARCompletingTransaction T>
 * void ProcessARResponse(T& transaction) {
 *     // Guaranteed to be read/lock operation
 * }
 * @endcode
 */
template<typename T>
concept ARCompletingTransaction = requires(T t) {
    { t.GetCompletionStrategy() } -> std::same_as<CompletionStrategy>;
} && requires(const T& t) {
    requires RequiresARResponse(t.GetCompletionStrategy());
};

/**
 * @brief Concept for commands that complete on AT acknowledgment.
 */
template<typename T>
concept ATCompletingTransaction = requires(T t) {
    { t.GetCompletionStrategy() } -> std::same_as<CompletionStrategy>;
} && requires(const T& t) {
    requires CompletesOnATAck(t.GetCompletionStrategy());
};

/**
 * @brief Derive completion strategy from IEEE 1394 transaction code.
 *
 * Per IEEE 1394-1995 Table 6-2 "Transaction Codes":
 * - 0x0: Write quadlet request (may complete on AT)
 * - 0x1: Write block request (may complete on AT or AR)
 * - 0x4: Read quadlet request (always complete on AR)
 * - 0x5: Read block request (always complete on AR)
 * - 0x8: Cycle start (async stream, no response)
 * - 0x9: Lock request (always complete on AR)
 *
 * @param tCode IEEE 1394 transaction code (4 bits)
 * @param expectsDeferred For writes: true if expecting ack_pending response
 * @return Recommended completion strategy
 */
constexpr CompletionStrategy StrategyFromTCode(uint8_t tCode, bool expectsDeferred = false) noexcept {
    switch (tCode) {
        case 0x4:  // Read quadlet
        case 0x5:  // Read block
        case 0x9:  // Lock
        case 0xA:  // Stream (if response expected)
            return CompletionStrategy::CompleteOnAR;

        case 0x0:  // Write quadlet
            return expectsDeferred ? CompletionStrategy::RequireBoth
                                   : CompletionStrategy::CompleteOnAT;

        case 0x1:  // Write block
            return expectsDeferred ? CompletionStrategy::RequireBoth
                                   : CompletionStrategy::CompleteOnAT;

        default:
            return CompletionStrategy::CompleteOnAT;
    }
}

// Compile-time validation
static_assert(StrategyFromTCode(0x4) == CompletionStrategy::CompleteOnAR,
              "Read quadlet must complete on AR");
static_assert(StrategyFromTCode(0x5) == CompletionStrategy::CompleteOnAR,
              "Read block must complete on AR");
static_assert(StrategyFromTCode(0x0) == CompletionStrategy::CompleteOnAT,
              "Write quadlet defaults to AT completion");
static_assert(StrategyFromTCode(0x1, true) == CompletionStrategy::RequireBoth,
              "Deferred write block requires both paths");

} // namespace ASFW::Async
