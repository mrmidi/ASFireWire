// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ControlsHandler.hpp - device controls for the app and MCP (FW-206/FW-207).
//
// One device's topology + state is served as the "asfw.controls.v1" JSON
// document (paged like the AV/C discovery document). Writes are asynchronous
// like raw FCP: SetControlParameter returns a request id at once and
// GetControlWriteResult reports the hardware-confirmed value later. The user
// client never waits on FCP.
//
// Today only AV/C devices have a control projection; other families answer
// kIOReturnUnsupported until their providers land (FW-203, FW-201).
#pragma once

#include <DriverKit/IOLib.h>

#include <cstdint>

struct IOUserClientMethodArguments;

namespace ASFW::Protocols::AVC {
class IAVCDiscovery;
}

namespace ASFW::UserClient {

/// Value kinds on the wire (scalarInput[4] / scalarOutput[0]). A double travels
/// as its IEEE-754 bit pattern.
enum class ControlValueKind : uint64_t { kBool = 0, kDouble = 1, kInt = 2 };

class ControlsHandler {
public:
    explicit ControlsHandler(Protocols::AVC::IAVCDiscovery* discovery) : discovery_(discovery) {}

    /// scalarInput: guidHi, guidLo, offset. structureOutput: one page.
    /// kIOReturnUnsupported when the device has no control projection.
    kern_return_t GetControlSnapshot(IOUserClientMethodArguments* args);

    /// scalarInput: guidHi, guidLo, revision, parameterId, valueKind, valueBits.
    /// scalarOutput[0]: request id. Refusals (stale revision, unknown id, wrong
    /// kind, out of domain) return an error at once, before any bus traffic.
    kern_return_t SetControlParameter(IOUserClientMethodArguments* args);

    /// scalarInput: request id. scalarOutput: valueKind, valueBits (confirmed).
    /// kIOReturnNotReady while the device has not answered.
    kern_return_t GetControlWriteResult(IOUserClientMethodArguments* args);

private:
    Protocols::AVC::IAVCDiscovery* discovery_{nullptr};
};

} // namespace ASFW::UserClient
