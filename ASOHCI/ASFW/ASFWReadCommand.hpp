//
// ASFWReadCommand.hpp
// Async read command for arbitrary-length reads - DriverKit reimplementation of
// IOFWReadCommand
//
// Builds OUTPUT_MORE_Immediate header + OUTPUT_LAST descriptors with AR
// completion path delivering data into response buffer.
//

#pragma once

#include "ASFWAsyncCommand.hpp"

#pragma mark -

//
// ASFWReadCommand
// Handles arbitrary-length async read operations
//
class ASFWReadCommand : public ASFWAsyncCommand {

protected:
  // Read-specific state
  int32_t fPackSize{0}; // Current packet size being processed

  // Response handling
  virtual void gotPacket(int rcode, const void *data, int size) override;
  virtual kern_return_t execute() override;

public:
  // OSObject lifecycle
  virtual bool init() override;

  // Initialization
  virtual bool initAll(ASOHCI *control, uint32_t generation,
                       ASFWAddress devAddress,
                       IOMemoryDescriptor *responseBuffer,
                       ASFWAsyncCompletion completion, void *refcon) override;
  virtual bool initAll(ASFWAddress devAddress,
                       IOMemoryDescriptor *responseBuffer,
                       ASFWAsyncCompletion completion, void *refcon,
                       bool failOnReset) override;

  // Reinitialization
  virtual kern_return_t reinit(ASFWAddress devAddress,
                               IOMemoryDescriptor *responseBuffer,
                               ASFWAsyncCompletion completion = nullptr,
                               void *refcon = nullptr,
                               bool failOnReset = false) override;
  virtual kern_return_t reinit(uint32_t generation, ASFWAddress devAddress,
                               IOMemoryDescriptor *responseBuffer,
                               ASFWAsyncCompletion completion = nullptr,
                               void *refcon = nullptr) override;
};