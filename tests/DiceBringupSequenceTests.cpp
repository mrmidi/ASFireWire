#include <gtest/gtest.h>

#include "Testing/HostDriverKitStubs.hpp"
#include "Audio/Backends/DiceBringupSequence.hpp"

#include <string>
#include <vector>

namespace {

using ASFW::Audio::Detail::RunDiceBringupSequence;

TEST(DiceBringupSequenceTests, RunsPhase0ParityOrderBeforeConfirmation) {
    std::vector<std::string> calls;
    int rollbackCount = 0;

    const IOReturn status = RunDiceBringupSequence(
        [&]() -> IOReturn {
            calls.emplace_back("prepare");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("reserve_playback");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("program_rx");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("reserve_capture");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("start_receive");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("program_tx_enable");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("start_transmit");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("confirm");
            return kIOReturnSuccess;
        },
        [&]() { ++rollbackCount; });

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(rollbackCount, 0);
    EXPECT_EQ(calls,
              (std::vector<std::string>{
                  "prepare",
                  "reserve_playback",
                  "program_rx",
                  "reserve_capture",
                  "start_receive",
                  "program_tx_enable",
                  "start_transmit",
                  "confirm",
              }));
}

TEST(DiceBringupSequenceTests, RollsBackWhenTransmitStartFails) {
    std::vector<std::string> calls;
    int rollbackCount = 0;

    const IOReturn status = RunDiceBringupSequence(
        [&]() -> IOReturn {
            calls.emplace_back("prepare");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("reserve_playback");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("program_rx");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("reserve_capture");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("start_receive");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("program_tx_enable");
            return kIOReturnSuccess;
        },
        [&]() -> IOReturn {
            calls.emplace_back("start_transmit");
            return kIOReturnTimeout;
        },
        [&]() -> IOReturn {
            calls.emplace_back("confirm");
            return kIOReturnSuccess;
        },
        [&]() {
            calls.emplace_back("rollback");
            ++rollbackCount;
        });

    EXPECT_EQ(status, kIOReturnTimeout);
    EXPECT_EQ(rollbackCount, 1);
    EXPECT_EQ(calls,
              (std::vector<std::string>{
                  "prepare",
                  "reserve_playback",
                  "program_rx",
                  "reserve_capture",
                  "start_receive",
                  "program_tx_enable",
                  "start_transmit",
                  "rollback",
              }));
}

} // namespace
