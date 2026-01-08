#pragma once
#include "../../ASFWDriver/Testing/HostDriverKitStubs.hpp"

// HostDriverKitStubs.hpp doesn't declare IOMemoryDescriptor base class for IOBufferMemoryDescriptor
// We might need to forward declare it or typedef it if it's used as a type.
class IOMemoryDescriptor : public OSObject {};
