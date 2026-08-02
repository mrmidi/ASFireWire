// DISABLED: BusResetCoordinatorTest
// #include "include/fw/bus_reset_harness.hpp"
// #include <cassert>
// #include <memory>

// // Include production header (relative path from fw-tests root)
// #include "../../ASOHCI/Core/BusResetCoordinator.hpp"

// using namespace fwtest;

// BusResetHarness::BusResetHarness() {
//   coord_ = std::make_unique<BusResetCoordinator>();
//   coord_->SetClock(&clock_); // inject fake clock
//   // Minimal init: we do not supply ivars / pci (nullptr paths exercised for logic not touching HW)
//   coord_->Init(nullptr, nullptr, 0, nullptr);
// }

// BusResetHarness::~BusResetHarness() = default;

// void BusResetHarness::TriggerReset(uint64_t t) {
//   clock_.now = t; // set absolute time
//   coord_->OnBusResetISR(t);
// }

// void BusResetHarness::SignalSelfIDComplete(uint32_t selfIDCount) {
//   coord_->OnSelfIDCompleteISR(selfIDCount);
// }

// void BusResetHarness::ProcessResetBegin() { coord_->ProcessResetBegin(); }
// void BusResetHarness::ProcessCheckSelfID() { coord_->ProcessCheckSelfID(); }
// void BusResetHarness::ProcessRestart() { coord_->ProcessRestart(); }
// void BusResetHarness::ProcessWatchdog() { coord_->ProcessWatchdog(); }
