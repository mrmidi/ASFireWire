//
// ASFWWriteCommand.hpp
// Async write command for arbitrary-length writes - DriverKit reimplementation
// of IOFWWriteCommand
//
// Builds OUTPUT_MORE_Immediate header + OUTPUT_LAST descriptors with DMA from
// request buffer. Supports posted write policy and retry-on-ackd via policy
// bits.
//

#pragma once

#include "ASFWAsyncCommand.hpp"

#pragma mark -

//
// ASFWWriteCommand
// Handles arbitrary-length async write operations
//
class ASFWWriteCommand : public ASFWAsyncCommand {

protected:
  // Write-specific state
  int32_t fPackSize{0}; // Current packet size being processed

  // Write options
  struct MemberVariables {
    bool fDeferredNotify{false};
    bool fFastRetryOnBusy{false};
  };
  MemberVariables *fWriteMembers{nullptr};

  // Helper methods
  virtual bool createMemberVariables() override;
  virtual void destroyMemberVariables() override;

  // Response handling
  virtual void gotPacket(int rcode, const void *data, int size) override;
  virtual kern_return_t execute() override;

public:
  // OSObject lifecycle
  virtual bool init() override;
  virtual void free() override;

  // Initialization
  virtual bool initAll(ASOHCI *control, uint32_t generation,
                       ASFWAddress devAddress,
                       IOMemoryDescriptor *requestBuffer,
                       ASFWAsyncCompletion completion, void *refcon) override;
  virtual bool initAll(ASFWAddress devAddress,
                       IOMemoryDescriptor *requestBuffer,
                       ASFWAsyncCompletion completion, void *refcon,
                       bool failOnReset) override;

  // Reinitialization
  virtual kern_return_t reinit(ASFWAddress devAddress,
                               IOMemoryDescriptor *requestBuffer,
                               ASFWAsyncCompletion completion = nullptr,
                               void *refcon = nullptr,
                               bool failOnReset = false) override;
  virtual kern_return_t reinit(uint32_t generation, ASFWAddress devAddress,
                               IOMemoryDescriptor *requestBuffer,
                               ASFWAsyncCompletion completion = nullptr,
                               void *refcon = nullptr) override;

  // Write options
  virtual void setDeferredNotify(bool state);
  virtual void setFastRetryOnBusy(bool state);
};