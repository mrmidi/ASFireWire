//
// ASFWWriteQuadCommand.hpp
// Optimized 4-byte write command - DriverKit reimplementation of
// IOFWWriteQuadCommand
//
// Builds OUTPUT_LAST_Immediate descriptor with write request header and inline
// quadlet when available, or DMA buffer for larger transfers.
//

#pragma once

#include "ASFWAsyncCommand.hpp"

#pragma mark -

//
// ASFWWriteQuadCommand
// Handles optimized 4-byte (quadlet) write operations
//
class ASFWWriteQuadCommand : public ASFWAsyncCommand {

protected:
  // Quadlet-specific state
  uint32_t *fQuads{nullptr}; // Buffer for quadlet data
  uint32_t fNumQuads{0};     // Number of quadlets to write
  uint32_t fQuadIndex{0};    // Current quadlet being processed
  int32_t fPackSize{0};      // Current packet size

  // Memory descriptor for quadlet data
  IOMemoryDescriptor *fQuadMD{nullptr};

  // Write options
  struct MemberVariables {
    bool fDeferredNotify{false};
    IOMemoryDescriptor *fMemory{nullptr};
  };
  MemberVariables *fQuadWriteMembers{nullptr};

  // Helper methods
  virtual bool createMemberVariables() override;
  virtual void destroyMemberVariables() override;
  virtual void setQuads(uint32_t *quads, int numQuads);
  virtual bool createMemoryDescriptor();
  virtual void destroyMemoryDescriptor();

  // Response handling
  virtual void gotPacket(int rcode, const void *data, int size) override;
  virtual kern_return_t execute() override;

public:
  // OSObject lifecycle
  virtual bool init() override;
  virtual void free() override;

  // Initialization for quadlet writes
  virtual bool initAll(ASOHCI *control, uint32_t generation,
                       ASFWAddress devAddress, uint32_t *quads, int numQuads,
                       ASFWAsyncCompletion completion, void *refcon);
  virtual bool initAll(ASFWAddress devAddress, uint32_t *quads, int numQuads,
                       ASFWAsyncCompletion completion, void *refcon,
                       bool failOnReset);

  // Reinitialization
  virtual kern_return_t reinit(ASFWAddress devAddress, uint32_t *quads,
                               int numQuads,
                               ASFWAsyncCompletion completion = nullptr,
                               void *refcon = nullptr,
                               bool failOnReset = false);
  virtual kern_return_t reinit(uint32_t generation, ASFWAddress devAddress,
                               uint32_t *quads, int numQuads,
                               ASFWAsyncCompletion completion = nullptr,
                               void *refcon = nullptr);

  // Write options
  virtual void setDeferredNotify(bool state);
};