// DISABLED: BusResetCoordinatorTest
// #pragma once
// #include <cstdint>
// #include <functional>
// #include <memory>

// // Forward declare production types (we include minimal header only in .cpp to keep build lean)
// class BusResetCoordinator;

// namespace fwtest {

// // Fake monotonic clock implementing coordinator interface
// class FakeClock : public BusResetCoordinator::IMonoClock {
// public:
//   uint64_t now = 0;
//   uint64_t Now() const override { return now; }
//   void Advance(uint64_t delta) { now += delta; }
// };

// // Scenario driver for BusResetCoordinator
// class BusResetHarness {
// public:
//   BusResetHarness();
//   ~BusResetHarness();

//   // Access to underlying coordinator
//   BusResetCoordinator* coordinator() { return coord_.get(); }
//   FakeClock& clock() { return clock_; }

//   // High-level scripted operations
//   void TriggerReset(uint64_t t);                  // Simulate ISR bus reset time
//   void SignalSelfIDComplete(uint32_t selfIDCount); // Simulate Self-ID complete ISR
//   void ProcessResetBegin();
//   void ProcessCheckSelfID();
//   void ProcessRestart();
//   void ProcessWatchdog();

// private:
//   FakeClock clock_;
//   std::unique_ptr<BusResetCoordinator> coord_;
// };

// } // namespace fwtest
