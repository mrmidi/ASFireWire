#pragma once

#include "../../ASFWDriver/Async/Interfaces/IFireWireBus.hpp"

#include <algorithm>
#include <array>
#include <deque>
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

    DeferredFireWireBus() = default;

    void SetGeneration(FW::Generation generation) noexcept { generation_ = generation; }
    void SetLocalNodeID(FW::NodeId nodeId) noexcept { localNodeId_ = nodeId; }
    void SetDefaultSpeed(FW::FwSpeed speed) noexcept { defaultSpeed_ = speed; }

    [[nodiscard]] size_t WriteCount() const noexcept { return writeHistory_.size(); }
    [[nodiscard]] const WriteSummary& WriteAt(size_t index) const noexcept { return writeHistory_.at(index); }
    [[nodiscard]] size_t PendingWriteCount() const noexcept { return pendingWrites_.size(); }

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
        callback(AsyncStatus::kTimeout, std::span<const uint8_t>{});
        return handle;
    }

    AsyncHandle WriteBlock(FW::Generation generation,
                           FW::NodeId nodeId,
                           FWAddress address,
                           std::span<const uint8_t> data,
                           FW::FwSpeed speed,
                           InterfaceCompletionCallback callback) override {
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
        std::array<uint8_t, 4> zeroes{};
        callback(AsyncStatus::kSuccess,
                 std::span<const uint8_t>{zeroes.data(),
                                          std::min<size_t>(zeroes.size(), responseLength)});
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

    FW::Generation generation_{1};
    FW::NodeId localNodeId_{0};
    FW::FwSpeed defaultSpeed_{FW::FwSpeed::S400};
    AsyncHandle nextHandle_{1};
    std::vector<WriteSummary> writeHistory_;
    std::deque<PendingWrite> pendingWrites_;
};

} // namespace ASFW::Async::Testing
