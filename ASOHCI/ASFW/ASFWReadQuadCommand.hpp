//
// ASFWReadQuadCommand.hpp
// Optimized 4-byte read command - DriverKit reimplementation of
// IOFWReadQuadCommand
//
// Builds OUTPUT_LAST_Immediate descriptor with quadlet read header.
// AR path returns quadlet in response buffer or inline.
//

#pragma once

#include "ASFWAsyncCommand.hpp"

#pragma mark -

//
// ASFWReadQuadCommand
// Handles optimized 4-byte (quadlet) read operations
//
class ASFWReadQuadCommand : public ASFWAsyncCommand {

protected:
  // Quadlet-specific state
  uint32_t *fQuads{nullptr}; // Buffer for quadlet data
  uint32_t fNumQuads{0};     // Number of quadlets to read
  uint32_t fQuadIndex{0};    // Current quadlet being processed

  // Member variables (similar to legacy)
  struct MemberVariables {
    bool fPingTime{false};
  };
  MemberVariables *fQuadMembers{nullptr};

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

  // Initialization for quadlet reads
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

  // Ping time configuration
  virtual void setPingTime(bool state);
};