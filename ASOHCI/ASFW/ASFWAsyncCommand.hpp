//
// ASFWAsyncCommand.hpp
// Base class for async transaction commands - DriverKit reimplementation of
// IOFWAsyncCommand
//
// Handles node-directed async requests (read, write, lock) that use AT Request
// context. Provides addressing, payload management, and response handling.
//

#pragma once

#include "../Shared/ASFWShared.hpp"
#include "ASFWCommand.hpp"
#include <DriverKit/IOMemoryDescriptor.h>
#include <atomic>

// Forward declarations
class ASOHCI;
struct AsyncPendingTrans;

// Async command completion callback
using ASFWAsyncCompletion =
    std::function<void(kern_return_t status, uint32_t responseCode,
                       IOMemoryDescriptor *responseData)>;

#pragma mark -

//
// ASFWAsyncCommand
// Base for node-directed async requests (read, write, lock) using AT Request
// context
//
class ASFWAsyncCommand : public ASFWCommand {

protected:
  // Addressing
  ASFWAddress fAddress{};
  ASFWSpeed fSpeed{ASFWSpeed::s400};
  uint32_t fGeneration{0};
  uint16_t fNodeID{0};

  // Transaction parameters
  uint32_t fTLabel{0}; // Transaction label
  uint32_t fTCode{0};  // Transaction code

  // Payload management
  IOMemoryDescriptor *fRequestMD{nullptr};  // For write payloads
  IOMemoryDescriptor *fResponseMD{nullptr}; // For read responses

  // Transaction state
  AsyncPendingTrans *fTrans{nullptr};
  uint32_t fBytesTransferred{0};
  int32_t fSize{0};    // Transfer size
  int32_t fMaxPack{0}; // Maximum packet size

  // Retry logic
  int32_t fCurRetries{0};
  int32_t fMaxRetries{3}; // Default retry count

  // Response handling
  uint32_t fResponseCode{0};
  uint32_t fAckCode{0};

  // Flags and options
  bool fFailOnReset{true};
  bool fWrite{false};
  bool fForceBlockRequests{false};

  // Completion
  ASFWAsyncCompletion fAsyncCompletion{nullptr};

  // Member variables structure (similar to legacy)
  struct MemberVariables {
    void *fSubclassMembers{nullptr};
    int32_t fMaxSpeed{static_cast<int32_t>(ASFWSpeed::s800)};
    int32_t fAckCode{0};
    uint32_t fResponseCode{0};
    uint32_t fFastRetryCount{0};
    int32_t fResponseSpeed{static_cast<int32_t>(ASFWSpeed::s400)};
    bool fForceBlockRequests{false};
  };
  MemberVariables *fMembers{nullptr};

  // Helper methods
  virtual bool createMemberVariables();
  virtual void destroyMemberVariables();

  // Transaction lifecycle
  virtual kern_return_t allocateTransaction();
  virtual void freeTransaction();

  // Response handling (called by ASOHCI)
  virtual void gotPacket(int rcode, const void *data, int size) = 0;
  virtual void gotAck(int ackCode);

  // Generation/NodeID updates
  virtual kern_return_t updateGeneration();
  virtual kern_return_t updateNodeID(uint32_t generation, uint16_t nodeID);

public:
  // OSObject lifecycle
  virtual bool init() override;
  virtual void free() override;

  // Initialization
  virtual bool initWithController(ASOHCI *control) override;
  virtual bool initAll(ASOHCI *control, uint32_t generation,
                       ASFWAddress devAddress, IOMemoryDescriptor *hostMem,
                       ASFWAsyncCompletion completion, void *refcon);
  virtual bool initAll(ASFWAddress devAddress, IOMemoryDescriptor *hostMem,
                       ASFWAsyncCompletion completion, void *refcon,
                       bool failOnReset);

  // Reinitialization
  virtual kern_return_t reinit(ASFWAddress devAddress,
                               IOMemoryDescriptor *hostMem,
                               ASFWAsyncCompletion completion = nullptr,
                               void *refcon = nullptr,
                               bool failOnReset = false);
  virtual kern_return_t reinit(uint32_t generation, ASFWAddress devAddress,
                               IOMemoryDescriptor *hostMem,
                               ASFWAsyncCompletion completion = nullptr,
                               void *refcon = nullptr);

  // Configuration
  virtual void setMaxPacket(uint32_t maxBytes);
  virtual void configure(ASFWAddress address, ASFWSpeed speed,
                         uint32_t generation, uint32_t tCode);

  // Completion setup
  virtual void setAsyncCompletion(ASFWAsyncCompletion completion);

  // State access
  ASFWAddress getAddress() const { return fAddress; }
  uint32_t getBytesTransferred() const { return fBytesTransferred; }
  bool failOnReset() const { return fFailOnReset; }

  // Speed and retry configuration
  virtual void setMaxSpeed(ASFWSpeed speed);
  virtual void setRetries(int32_t retries);
  virtual int32_t getMaxRetries() const { return fMaxRetries; }

  // Response codes
  virtual void setResponseCode(uint32_t rcode) { fResponseCode = rcode; }
  virtual uint32_t getResponseCode() const { return fResponseCode; }

  virtual void setAckCode(int32_t ack) { fAckCode = ack; }
  virtual int32_t getAckCode() const { return fAckCode; }

  // Fast retry configuration
  virtual void setFastRetryCount(uint32_t count);
  virtual uint32_t getFastRetryCount() const;

  virtual void setResponseSpeed(ASFWSpeed speed);
  virtual ASFWSpeed getResponseSpeed() const;

  // Block request forcing
  virtual void setForceBlockRequests(bool enabled) {
    fForceBlockRequests = enabled;
  }

  // Progress checking
  virtual kern_return_t checkProgress() override;

  // Transaction submission (to be called by subclasses)
  virtual kern_return_t submitToATManager();

  // Friend for ASOHCI access
  friend class ASOHCI;
};