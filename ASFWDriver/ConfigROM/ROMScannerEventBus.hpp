#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#ifdef ASFW_HOST_TEST
#include <mutex>
#else
#include <DriverKit/IOLib.h>
#endif

#include "ROMReader.hpp"

namespace ASFW::Discovery {

enum class ROMScannerEventType : uint8_t {
    BIBComplete,
    IRMReadComplete,
    IRMLockComplete,
    RootDirComplete,
    EnsurePrefixComplete,
};

struct ROMScannerReadEventData {
    bool success{false};
    uint8_t nodeId{0};
    Generation generation{0};
    uint32_t address{0};
    std::vector<uint32_t> quadlets;

    [[nodiscard]] static ROMScannerReadEventData FromReadResult(uint8_t node,
                                                                const ROMReader::ReadResult& result) {
        ROMScannerReadEventData data{};
        data.success = result.success;
        data.nodeId = node;
        data.generation = result.generation;
        data.address = result.address;

        if (result.data && result.dataLength >= sizeof(uint32_t)) {
            const auto count = result.dataLength / sizeof(uint32_t);
            data.quadlets.assign(result.data, result.data + count);
        }
        return data;
    }

    [[nodiscard]] ROMReader::ReadResult ToReadResult() const {
        ROMReader::ReadResult result{};
        result.success = success;
        result.nodeId = nodeId;
        result.generation = generation;
        result.address = address;
        result.data = quadlets.empty() ? nullptr : quadlets.data();
        result.dataLength = static_cast<uint32_t>(quadlets.size() * sizeof(uint32_t));
        return result;
    }
};

struct ROMScannerEvent {
    ROMScannerEventType type{ROMScannerEventType::BIBComplete};
    ROMScannerReadEventData payload{};
    uint32_t requiredTotalQuadlets{0};
    std::shared_ptr<std::function<void(bool)>> ensurePrefixCompletion;
};

class ROMScannerEventBus {
public:
    ROMScannerEventBus() {
#ifndef ASFW_HOST_TEST
        lock_ = IOLockAlloc();
#endif
    }

    ~ROMScannerEventBus() {
#ifndef ASFW_HOST_TEST
        if (lock_ != nullptr) {
            IOLockFree(lock_);
            lock_ = nullptr;
        }
#endif
    }

    ROMScannerEventBus(const ROMScannerEventBus&) = delete;
    ROMScannerEventBus& operator=(const ROMScannerEventBus&) = delete;

    void Publish(ROMScannerEvent event) {
        Lock();
        queue_.push_back(std::move(event));
        Unlock();
    }

    template <typename Handler>
    void Drain(Handler&& handler) {
        std::deque<ROMScannerEvent> local;

        Lock();
        local.swap(queue_);
        Unlock();

        while (!local.empty()) {
            handler(local.front());
            local.pop_front();
        }
    }

    void Clear() {
        Lock();
        queue_.clear();
        Unlock();
    }

private:
    void Lock() {
#ifdef ASFW_HOST_TEST
        lock_.lock();
#else
        if (lock_ != nullptr) {
            IOLockLock(lock_);
        }
#endif
    }

    void Unlock() {
#ifdef ASFW_HOST_TEST
        lock_.unlock();
#else
        if (lock_ != nullptr) {
            IOLockUnlock(lock_);
        }
#endif
    }

#ifdef ASFW_HOST_TEST
    std::mutex lock_;
#else
    IOLock* lock_{nullptr};
#endif
    std::deque<ROMScannerEvent> queue_;
};

} // namespace ASFW::Discovery
