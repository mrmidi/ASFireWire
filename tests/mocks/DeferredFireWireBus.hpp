#pragma once

#include "../../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ASFW::Async::Testing {

class DeferredFireWireBus : public IFireWireBus {
public:
    struct WriteSummary {
        AsyncHandle handle{};
        FW::Generation generation{0};
        FW::NodeId nodeId{0};
        FWAddress address{};
        FW::FwSpeed speed{FW::FwSpeed::S100};
        std::vector<uint8_t> data;
    };

    /// How Lock() answers a compare-swap.
    ///
    /// `kZeroes` is the historical behaviour and stays the default so the
    /// SBP-2 / FCP suites that predate the programmable modes are unaffected.
    enum class LockMode : uint8_t {
        kZeroes,        ///< Always succeed, returning zeroes.
        kEchoCompare,   ///< Return the operand's compare half, so a CAS "succeeds".
    };

    DeferredFireWireBus() = default;

    void SetGeneration(FW::Generation generation) noexcept { generation_ = generation; }
    void SetLocalNodeID(FW::NodeId nodeId) noexcept { localNodeId_ = nodeId; }
    void SetDefaultSpeed(FW::FwSpeed speed) noexcept { defaultSpeed_ = speed; }

    // -------------------------------------------------------------------------
    // Programmable reads
    //
    // Unmapped addresses keep the historical `kTimeout` answer, so adding a map
    // cannot change the behaviour any existing consumer already relies on.
    // -------------------------------------------------------------------------

    void MapRead(uint64_t address, std::vector<uint8_t> bytes) {
        readMap_[address] = std::move(bytes);
    }

    void MapReadQuadlet(uint64_t address, uint32_t valueBigEndian) {
        readMap_[address] = {
            static_cast<uint8_t>(valueBigEndian >> 24),
            static_cast<uint8_t>(valueBigEndian >> 16),
            static_cast<uint8_t>(valueBigEndian >> 8),
            static_cast<uint8_t>(valueBigEndian),
        };
    }

    void ClearReadMap() noexcept { readMap_.clear(); }

    /// Answer every otherwise-unmapped read with this quadlet. Without it,
    /// unmapped addresses keep returning kTimeout.
    void SetDefaultReadQuadlet(uint32_t valueBigEndian) {
        defaultRead_ = {
            static_cast<uint8_t>(valueBigEndian >> 24),
            static_cast<uint8_t>(valueBigEndian >> 16),
            static_cast<uint8_t>(valueBigEndian >> 8),
            static_cast<uint8_t>(valueBigEndian),
        };
    }

    void ClearDefaultRead() noexcept { defaultRead_.reset(); }

    // -------------------------------------------------------------------------
    // Lock behaviour
    // -------------------------------------------------------------------------

    void SetLockMode(LockMode mode) noexcept { lockMode_ = mode; }

    /// Corrupt the next returned "old value" so a compare-swap reports failure.
    void FailNextCompareSwap() noexcept { failNextCompareSwap_ = true; }

    /// Same, but latched: every compare-swap fails until cleared.
    void SetFailCompareSwap(bool fail) noexcept { failCompareSwap_ = fail; }

    [[nodiscard]] const std::vector<uint8_t>& LastLockOperand() const noexcept {
        return lastLockOperand_;
    }
    [[nodiscard]] size_t LockCount() const noexcept { return lockCount_; }

    // -------------------------------------------------------------------------
    // Read/write recording
    // -------------------------------------------------------------------------

    struct ReadSummary {
        FW::NodeId nodeId{0};
        FWAddress address{};
        uint32_t length{0};
    };

    [[nodiscard]] size_t ReadCount() const noexcept { return readHistory_.size(); }
    [[nodiscard]] const ReadSummary& ReadAt(size_t index) const { return readHistory_.at(index); }

    // Force the next WriteBlock to fail synchronously (return a null handle),
    // modelling AT ring-full / async-label-pool exhaustion / a bus-reset
    // generation edge — the trigger for FetchAgent::AppendImmediate's
    // synchronous FailORB path. Auto-clears after one WriteBlock.
    void FailNextWriteBlock() noexcept { failNextWriteBlock_ = true; }

    [[nodiscard]] size_t WriteCount() const noexcept { return writeHistory_.size(); }
    [[nodiscard]] const WriteSummary& WriteAt(size_t index) const noexcept { return writeHistory_.at(index); }
    [[nodiscard]] size_t PendingWriteCount() const noexcept { return pendingWrites_.size(); }

    /// Inspect a queued-but-uncompleted write without completing it, so a
    /// caller can route by destination address before draining.
    [[nodiscard]] const WriteSummary& PendingWriteAt(size_t index) const {
        return pendingWrites_.at(index).summary;
    }

    bool CompleteNextWrite(AsyncStatus status, std::span<const uint8_t> payload = {}) {
        if (pendingWrites_.empty()) {
            return false;
        }

        PendingWrite pending = std::move(pendingWrites_.front());
        pendingWrites_.pop_front();
        if (pending.callback) {
            pending.callback(status, payload);
        }
        return true;
    }

    bool CompleteWrite(AsyncHandle handle,
                       AsyncStatus status,
                       std::span<const uint8_t> payload = {}) {
        const auto it = std::find_if(
            pendingWrites_.begin(), pendingWrites_.end(),
            [handle](const PendingWrite& pending) {
                return pending.summary.handle.value == handle.value;
            });
        if (it == pendingWrites_.end()) {
            return false;
        }

        auto callback = std::move(it->callback);
        pendingWrites_.erase(it);
        if (callback) {
            callback(status, payload);
        }
        return true;
    }

    AsyncHandle ReadBlock(FW::Generation generation,
                          FW::NodeId nodeId,
                          FWAddress address,
                          uint32_t length,
                          FW::FwSpeed speed,
                          InterfaceCompletionCallback callback) override {
        const AsyncHandle handle = NextHandle();
        readHistory_.push_back(ReadSummary{
            .nodeId = nodeId,
            .address = address,
            .length = length,
        });

        const auto it = readMap_.find(AddressKey(address));
        if (it == readMap_.end() && !defaultRead_.has_value()) {
            callback(AsyncStatus::kTimeout, std::span<const uint8_t>{});
            return handle;
        }

        const auto& bytes = (it != readMap_.end()) ? it->second : *defaultRead_;
        if (bytes.size() < length) {
            callback(AsyncStatus::kShortRead, std::span<const uint8_t>{});
            return handle;
        }
        callback(AsyncStatus::kSuccess, std::span<const uint8_t>{bytes.data(), length});
        return handle;
    }

    AsyncHandle WriteBlock(FW::Generation generation,
                           FW::NodeId nodeId,
                           FWAddress address,
                           std::span<const uint8_t> data,
                           FW::FwSpeed speed,
                           InterfaceCompletionCallback callback) override {
        if (failNextWriteBlock_) {
            failNextWriteBlock_ = false;
            return AsyncHandle{0};  // synchronous submit failure — no callback ever fires
        }
        const AsyncHandle handle = NextHandle();

        WriteSummary summary{};
        summary.handle = handle;
        summary.generation = generation;
        summary.nodeId = nodeId;
        summary.address = address;
        summary.speed = speed;
        summary.data.assign(data.begin(), data.end());
        writeHistory_.push_back(summary);

        pendingWrites_.push_back(PendingWrite{
            .summary = summary,
            .callback = std::move(callback),
        });
        return handle;
    }

    AsyncHandle Lock(FW::Generation generation,
                     FW::NodeId nodeId,
                     FWAddress address,
                     FW::LockOp lockOp,
                     std::span<const uint8_t> operand,
                     uint32_t responseLength,
                     FW::FwSpeed speed,
                     InterfaceCompletionCallback callback) override {
        const AsyncHandle handle = NextHandle();
        ++lockCount_;
        lastLockOperand_.assign(operand.begin(), operand.end());

        std::array<uint8_t, 4> oldValue{};
        if (lockMode_ == LockMode::kEchoCompare && operand.size() >= oldValue.size()) {
            // A compare-swap operand is [compare || new]; echoing the compare
            // half makes the CAS report success.
            std::copy_n(operand.begin(), oldValue.size(), oldValue.begin());
        }
        if (failNextCompareSwap_ || failCompareSwap_) {
            failNextCompareSwap_ = false;
            oldValue[3] ^= 0x01U;
        }

        callback(AsyncStatus::kSuccess,
                 std::span<const uint8_t>{oldValue.data(),
                                          std::min<size_t>(oldValue.size(), responseLength)});
        return handle;
    }

    bool Cancel(AsyncHandle handle) override {
        const auto it = std::find_if(
            pendingWrites_.begin(), pendingWrites_.end(),
            [handle](const PendingWrite& pending) {
                return pending.summary.handle.value == handle.value;
            });
        if (it == pendingWrites_.end()) {
            return false;
        }

        auto callback = std::move(it->callback);
        pendingWrites_.erase(it);
        if (callback) {
            callback(AsyncStatus::kAborted, std::span<const uint8_t>{});
        }
        return true;
    }

    FW::FwSpeed GetSpeed(FW::NodeId nodeId) const override {
        return defaultSpeed_;
    }

    uint32_t HopCount(FW::NodeId nodeA, FW::NodeId nodeB) const override {
        return 1;
    }

    FW::Generation GetGeneration() const override {
        return generation_;
    }

    FW::NodeId GetLocalNodeID() const override {
        return localNodeId_;
    }

private:
    struct PendingWrite {
        WriteSummary summary;
        InterfaceCompletionCallback callback;
    };

    AsyncHandle NextHandle() noexcept {
        const AsyncHandle handle = nextHandle_;
        nextHandle_ = AsyncHandle{nextHandle_.value + 1};
        return handle;
    }

    /// Node-independent 48-bit key: bits[47:32] = addressHi, bits[31:0] = addressLo.
    [[nodiscard]] static uint64_t AddressKey(const FWAddress& address) noexcept {
        return (static_cast<uint64_t>(address.addressHi) << 32U) |
               static_cast<uint64_t>(address.addressLo);
    }

    FW::Generation generation_{1};
    FW::NodeId localNodeId_{0};
    FW::FwSpeed defaultSpeed_{FW::FwSpeed::S400};
    bool failNextWriteBlock_{false};
    AsyncHandle nextHandle_{1};
    std::vector<WriteSummary> writeHistory_;
    std::deque<PendingWrite> pendingWrites_;

    std::unordered_map<uint64_t, std::vector<uint8_t>> readMap_;
    std::optional<std::vector<uint8_t>> defaultRead_;
    std::vector<ReadSummary> readHistory_;
    LockMode lockMode_{LockMode::kZeroes};
    bool failNextCompareSwap_{false};
    bool failCompareSwap_{false};
    size_t lockCount_{0};
    std::vector<uint8_t> lastLockOperand_;
};

} // namespace ASFW::Async::Testing
