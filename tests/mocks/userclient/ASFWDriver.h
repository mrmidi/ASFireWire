#pragma once
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"
class ASFWDriver : public IOService {
public:
    void* asyncPort{nullptr};
    void* GetAsyncSubsystem() { return asyncPort; }
    void* GetControllerCore() { return nullptr; }
};
