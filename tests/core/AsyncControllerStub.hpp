// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AsyncControllerStub.hpp — IAsyncControllerPort stub for controller-side tests.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "../../ASFWDriver/Async/Interfaces/IAsyncControllerPort.hpp"
#include "../../ASFWDriver/Testing/HostDriverKitStubs.hpp"

namespace ASFW::Driver {

class AsyncControllerStub final : public ASFW::Async::IAsyncControllerPort {
  public:
    kern_return_t ArmARContextsOnly() override { return kIOReturnSuccess; }
    void PostToWorkloop(void (^block)()) override {
        if (block != nullptr) {
            block();
        }
    }

    void OnTxInterrupt() override {}
    void OnRxRequestInterrupt() override {
        ++rxRequestInterrupts;
        if (onRxRequestInterrupt) {
            onRxRequestInterrupt();
        }
    }
    void OnRxResponseInterrupt() override {}

    void OnBusResetBegin(uint8_t nextGen) override { beginGenerations.push_back(nextGen); }
    void OnBusResetComplete(uint8_t stableGen) override {
        completeGenerations.push_back(stableGen);
    }
    void ConfirmBusGeneration(uint8_t confirmedGeneration) override {
        confirmedGenerations.push_back(confirmedGeneration);
    }
    void StopATContextsOnly() override { ++stopAtContextsCount; }
    void FlushATContexts() override { ++flushAtContextsCount; }
    void RearmATContexts() override { ++rearmAtContextsCount; }

    [[nodiscard]] ASFW::Async::AsyncBusStateSnapshot GetBusStateSnapshot() const override {
        return {};
    }

    [[nodiscard]] ASFW::Shared::DMAMemoryManager* GetDMAManager() override { return nullptr; }

    ASFW::Async::AsyncHandle Read(const ASFW::Async::ReadParams&,
                                  ASFW::Async::CompletionCallback) override {
        return {};
    }
    ASFW::Async::AsyncHandle ReadWithRetry(const ASFW::Async::ReadParams&,
                                           const ASFW::Async::RetryPolicy&,
                                           ASFW::Async::CompletionCallback) override {
        return {};
    }
    ASFW::Async::AsyncHandle Write(const ASFW::Async::WriteParams&,
                                   ASFW::Async::CompletionCallback) override {
        return {};
    }
    ASFW::Async::AsyncHandle Lock(const ASFW::Async::LockParams&, uint16_t,
                                  ASFW::Async::CompletionCallback) override {
        return {};
    }
    ASFW::Async::AsyncHandle CompareSwap(const ASFW::Async::CompareSwapParams&,
                                         ASFW::Async::CompareSwapCallback) override {
        return {};
    }
    ASFW::Async::AsyncHandle PhyRequest(const ASFW::Async::PhyParams&,
                                        ASFW::Async::CompletionCallback) override {
        return {};
    }

    bool Cancel(ASFW::Async::AsyncHandle) override { return false; }
    void OnTimeoutTick() override {}

    [[nodiscard]] ASFW::Async::AsyncWatchdogStats GetWatchdogStats() const override {
        return {};
    }
    [[nodiscard]] ASFW::Debug::BusResetPacketCapture* GetBusResetCapture() const override {
        return nullptr;
    }
    [[nodiscard]] std::optional<ASFW::Async::AsyncStatusSnapshot> GetStatusSnapshot() const override {
        return std::nullopt;
    }
    [[nodiscard]] ASFW::Debug::AsyncTraceCapture* GetAsyncTraceCapture() const override {
        return nullptr;
    }
    [[nodiscard]] ASFWDiagInboundCSRStats* GetInboundCSRStats() const override {
        return nullptr;
    }

    // Runs inside the handler's ring service, where a test can land a packet.
    std::function<void()> onRxRequestInterrupt;
    uint32_t rxRequestInterrupts{0};
    std::vector<uint8_t> beginGenerations;
    std::vector<uint8_t> confirmedGenerations;
    std::vector<uint8_t> completeGenerations;
    uint32_t stopAtContextsCount{0};
    uint32_t flushAtContextsCount{0};
    uint32_t rearmAtContextsCount{0};
};

} // namespace ASFW::Driver
