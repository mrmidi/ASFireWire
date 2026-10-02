#pragma once
#include "ASFWDriver.h"
#include <DriverKit/IOUserClient.h>
struct ASFWDriverUserClient_IVars {
    ASFWDriver* driver{};
    OSAction* transactionAction{};
    bool transactionListenerRegistered{};
    void* runtimeState{};
    IOLock* actionLock{};
    bool stopping{};
    uint32_t transactionHolds{};
};
// Mock only the generated IIG boundary. Production TransactionHandler.cpp
// supplies every submission/callback/retain/release exercised by this target.
class ASFWDriverUserClient : public IOUserClient {
public:
    ASFWDriverUserClient_IVars state{};
    ASFWDriverUserClient_IVars* ivars{&state};
    std::atomic<bool>* destroyed{};
    ASFWDriverUserClient() { state.actionLock = IOLockAlloc(); }
    ~ASFWDriverUserClient() override {
        if (state.transactionAction) state.transactionAction->release();
        IOLockFree(state.actionLock);
        if (destroyed) destroyed->store(true);
    }
    void NotifyTransactionComplete(uint16_t, uint32_t) {}
};
