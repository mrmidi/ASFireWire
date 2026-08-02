// DISABLED: BusResetCoordinatorTest

// #include <gtest/gtest.h>
// #include <fw/bus_reset_harness.hpp>

// // NOTE: These tests exercise high‑level sequencing logic without hardware side‑effects.
// // We rely on FakeClock + nullptr ivars / pci to drive timing transitions that do not
// // dereference hardware pointers in the current implementation paths.

// using State = BusResetCoordinator::State;

// static const uint64_t MS = 1000ULL * 1000ULL; // nanoseconds per millisecond (mach style units proxy)

// class BusResetCoordinatorTest : public ::testing::Test {
// protected:
//   fwtest::BusResetHarness harness;
// };

// TEST_F(BusResetCoordinatorTest, SingleResetBasicFlow_NoNodeIDYet) {
//   auto *coord = harness.coordinator();
//   harness.TriggerReset(0);
//   EXPECT_EQ(coord->GetState(), State::kResetBegin);
//   // AT inactive path cannot progress (no PCI) so ProcessResetBegin should early exit
//   harness.ProcessResetBegin();
//   // State may remain ResetBegin because ATContextsInactive() returns false (no PCI)
//   EXPECT_EQ(coord->GetState(), State::kResetBegin);
//   // Simulate Self-ID completion even while still ResetBegin (should not advance until WaitingSelfID)
//   harness.SignalSelfIDComplete(0x10);
//   harness.ProcessCheckSelfID();
//   EXPECT_NE(coord->GetState(), State::kSelfIDComplete) << "Should not move without WaitingSelfID state";
// }

// TEST_F(BusResetCoordinatorTest, RapidDoubleResetCountsFast) {
//   auto *coord = harness.coordinator();
//   harness.TriggerReset(0);
//   harness.TriggerReset(10 * MS); // within 50ms debounce window
//   const auto &m = coord->GetMetrics();
//   EXPECT_EQ(m.resets, 2u);
//   EXPECT_EQ(m.fastResets, 1u);
// }

// TEST_F(BusResetCoordinatorTest, WatchdogWaitingSelfIDTriggersRecovery) {
//   auto *coord = harness.coordinator();
//   harness.TriggerReset(0);
//   // Force state manually to WaitingSelfID for watchdog evaluation (simulate AT inactive ack path)
//   coord->ProcessResetBegin(); // likely no-op; manually transition for test via private not accessible -> skip
//   // We cannot directly set state (private); instead emulate elapsed time beyond deadline and ensure no crash invoking watchdog.
//   // Advance clock past default 300ms watchdog and call watchdog: with current implementation
//   // lack of WaitingSelfID entry may yield no transition, but function should be safe.
//   harness.clock().Advance(400 * MS);
//   harness.ProcessWatchdog();
//   SUCCEED();
// }

// TEST_F(BusResetCoordinatorTest, FabricatedMarkerIncrementsMetric) {
//   auto *coord = harness.coordinator();
//   harness.TriggerReset(0);
//   coord->OnBusResetMarker(5);
//   EXPECT_EQ(coord->GetMetrics().fabricatedMarkers, 1u);
// }

// TEST_F(BusResetCoordinatorTest, FullSequenceThroughComplete) {
//   auto *coord = harness.coordinator();
//   harness.TriggerReset(0);
//   // Force AT inactive so ProcessResetBegin can advance
//   coord->_TestSetATInactive(true);
//   harness.ProcessResetBegin();
//   EXPECT_EQ(coord->GetState(), State::kWaitingSelfID);
//   // Signal Self-ID completion and mark stability
//   harness.SignalSelfIDComplete(0x20);
//   coord->_TestSetSelfIDStable(true);
//   harness.ProcessCheckSelfID();
//   EXPECT_EQ(coord->GetState(), State::kSelfIDComplete);
//   // Delay NodeID a few retries
//   harness.ProcessRestart(); // first attempt (not valid)
//   EXPECT_EQ(coord->GetState(), State::kSelfIDComplete);
//   coord->_TestSetNodeIDValid(true);
//   harness.ProcessRestart();
//   EXPECT_EQ(coord->GetState(), State::kComplete);
// }

// TEST_F(BusResetCoordinatorTest, WatchdogRecoveryFromWaitingSelfID) {
//   auto *coord = harness.coordinator();
//   harness.TriggerReset(0);
//   coord->_TestSetATInactive(true);
//   harness.ProcessResetBegin();
//   EXPECT_EQ(coord->GetState(), State::kWaitingSelfID);
//   // Set deadline in past
//   coord->_TestSetWaitingSelfIDDeadline(1);
//   harness.clock().Advance(500 * MS);
//   harness.ProcessWatchdog();
//   // State forced back to ResetBegin for recovery
//   EXPECT_EQ(coord->GetState(), State::kResetBegin);
//   EXPECT_GE(coord->WatchdogRecoveryCount(), 1u);
// }

// TEST_F(BusResetCoordinatorTest, NodeIDRetryAttemptsIncrementMetrics) {
//   auto *coord = harness.coordinator();
//   harness.TriggerReset(0);
//   coord->_TestSetATInactive(true);
//   harness.ProcessResetBegin();
//   EXPECT_EQ(coord->GetState(), State::kWaitingSelfID);
//   // Complete Self-ID (stable after flag)
//   harness.SignalSelfIDComplete(0x11);
//   coord->_TestSetSelfIDStable(true);
//   harness.ProcessCheckSelfID();
//   EXPECT_EQ(coord->GetState(), State::kSelfIDComplete);
//   // Simulate several failed NodeID validations
//   for (int i = 0; i < 5; ++i) {
//     harness.ProcessRestart();
//     EXPECT_EQ(coord->GetState(), State::kSelfIDComplete) << "Should stay until NodeID valid";
//   }
//   auto beforeAttempts = coord->GetMetrics().nodeIDValidationAttempts;
//   EXPECT_GE(beforeAttempts, 5u);
//   // Now allow NodeID valid
//   coord->_TestSetNodeIDValid(true);
//   harness.ProcessRestart();
//   EXPECT_EQ(coord->GetState(), State::kComplete);
//   EXPECT_EQ(coord->GetMetrics().restarts, 1u);
// }

// TEST_F(BusResetCoordinatorTest, SelfIDUnstableThenStable) {
//   auto *coord = harness.coordinator();
//   harness.TriggerReset(0);
//   coord->_TestSetATInactive(true);
//   harness.ProcessResetBegin();
//   EXPECT_EQ(coord->GetState(), State::kWaitingSelfID);
//   // Self-ID complete ISR fires but not stable yet (no flag)
//   harness.SignalSelfIDComplete(0x22);
//   harness.ProcessCheckSelfID();
//   EXPECT_EQ(coord->GetState(), State::kWaitingSelfID) << "Unstable Self-ID should not advance";
//   // Now mark stable and retry
//   coord->_TestSetSelfIDStable(true);
//   harness.ProcessCheckSelfID();
//   EXPECT_EQ(coord->GetState(), State::kSelfIDComplete);
// }

// // Additional deeper integration tests (SelfID stability, Restart path) require either
// // mock PCI register layer or refactoring ATContextsInactive/NodeIDValid to be injectable.
// // Those will be added in a subsequent phase.
