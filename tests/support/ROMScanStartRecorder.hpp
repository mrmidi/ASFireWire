// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// ROMScanStartRecorder.hpp — the ControllerCore harness's ROMScanner::Start
// stub (ControllerCoreCollaboratorStubs.cpp) hands each request to this
// recorder when a test installs one; without one a scan start fails the test.

#pragma once

#include <functional>

#include "ASFWDriver/ConfigROM/ROMScanner.hpp"

namespace ASFW::Discovery::Testing {

inline std::function<void(const ROMScanRequest&)>& ROMScanStartRecorder() {
    static std::function<void(const ROMScanRequest&)> recorder;
    return recorder;
}

} // namespace ASFW::Discovery::Testing
