//
// ASFWAsyncCommand.cpp
// Implementation of ASFWAsyncCommand base class
//

#include "ASFWAsyncCommand.hpp"
#include <DriverKit/IOLib.h>
#include <os/log.h>

// No OSObject boilerplate macro needed for DriverKit -
// OSDeclareDefaultStructors handles it

#pragma mark - Lifecycle

bool ASFWAsyncCommand::init() {
  if (!ASFWCommand::init()) {
    return false;
  }

  if (!createMemberVariables()) {
    return false;
  }

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWAsyncCommand[%u] initialized", getId());
  return true;
}

void ASFWAsyncCommand::free() {
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWAsyncCommand[%u] freed", getId());

  // Clean up memory descriptors
  if (fRequestMD) {
    fRequestMD->release();
    fRequestMD = nullptr;
  }

  if (fResponseMD) {
    fResponseMD->release();
    fResponseMD = nullptr;
  }

  // Free transaction if allocated
  freeTransaction();

  // Clean up member variables
  destroyMemberVariables();

  // Clean up completion callback
  fAsyncCompletion = nullptr;

  ASFWCommand::free();
}

bool ASFWAsyncCommand::initWithController(ASOHCI *control) {
  if (!ASFWCommand::initWithController(control)) {
    return false;
  }

  if (!createMemberVariables()) {
    return false;
  }

  return true;
}

bool ASFWAsyncCommand::initAll(ASOHCI *control, uint32_t generation,
                               ASFWAddress devAddress,
                               IOMemoryDescriptor *hostMem,
                               ASFWAsyncCompletion completion, void *refcon) {
  if (!initWithController(control)) {
    return false;
  }

  fGeneration = generation;
  fAddress = devAddress;
  fNodeID = devAddress.nodeID;

  // For writes, hostMem is the request data
  // For reads, hostMem will be the response buffer
  if (fWrite) {
    fRequestMD = hostMem;
    if (fRequestMD) {
      fRequestMD->retain();
    }
  } else {
    fResponseMD = hostMem;
    if (fResponseMD) {
      fResponseMD->retain();
    }
  }

  fAsyncCompletion = completion;

  // Set default speed and max packet
  fSpeed = ASFWSpeed::s400;
  fMaxPack = 512; // Default max packet size

  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWAsyncCommand[%u] initAll - nodeID=0x%x, addr=0x%x:%x",
         getId(), fNodeID, fAddress.addressHi, fAddress.addressLo);

  return true;
}

bool ASFWAsyncCommand::initAll(ASFWAddress devAddress,
                               IOMemoryDescriptor *hostMem,
                               ASFWAsyncCompletion completion, void *refcon,
                               bool failOnReset) {
  fAddress = devAddress;
  fNodeID = devAddress.nodeID;
  fFailOnReset = failOnReset;

  if (fWrite) {
    fRequestMD = hostMem;
    if (fRequestMD) {
      fRequestMD->retain();
    }
  } else {
    fResponseMD = hostMem;
    if (fResponseMD) {
      fResponseMD->retain();
    }
  }

  fAsyncCompletion = completion;

  return true;
}

kern_return_t ASFWAsyncCommand::reinit(ASFWAddress devAddress,
                                       IOMemoryDescriptor *hostMem,
                                       ASFWAsyncCompletion completion,
                                       void *refcon, bool failOnReset) {
  fAddress = devAddress;
  fNodeID = devAddress.nodeID;
  fFailOnReset = failOnReset;

  // Clean up old memory descriptors
  if (fRequestMD) {
    fRequestMD->release();
    fRequestMD = nullptr;
  }
  if (fResponseMD) {
    fResponseMD->release();
    fResponseMD = nullptr;
  }

  // Set new memory descriptor
  if (fWrite) {
    fRequestMD = hostMem;
    if (fRequestMD) {
      fRequestMD->retain();
    }
  } else {
    fResponseMD = hostMem;
    if (fResponseMD) {
      fResponseMD->retain();
    }
  }

  if (completion) {
    fAsyncCompletion = completion;
  }

  fStatus = kIOReturnNotReady;
  fCompleted = false;

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWAsyncCommand[%u] reinited", getId());
  return kIOReturnSuccess;
}

kern_return_t ASFWAsyncCommand::reinit(uint32_t generation,
                                       ASFWAddress devAddress,
                                       IOMemoryDescriptor *hostMem,
                                       ASFWAsyncCompletion completion,
                                       void *refcon) {
  fGeneration = generation;
  return reinit(devAddress, hostMem, completion, refcon, fFailOnReset);
}

#pragma mark - Configuration

void ASFWAsyncCommand::setMaxPacket(uint32_t maxBytes) {
  if (getStatus() == kIOReturnBusy || getStatus() == kIOFireWirePending) {
    return;
  }
  fMaxPack = maxBytes;
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWAsyncCommand[%u] max packet set to %u",
         getId(), maxBytes);
}

void ASFWAsyncCommand::configure(ASFWAddress address, ASFWSpeed speed,
                                 uint32_t generation, uint32_t tCode) {
  fAddress = address;
  fNodeID = address.nodeID;
  fSpeed = speed;
  fGeneration = generation;
  fTCode = tCode;

  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWAsyncCommand[%u] configured - nodeID=0x%x, speed=%d, "
         "gen=%u, tCode=%u",
         getId(), fNodeID, static_cast<int>(speed), generation, tCode);
}

void ASFWAsyncCommand::setAsyncCompletion(ASFWAsyncCompletion completion) {
  fAsyncCompletion = completion;
}

#pragma mark - Speed and Retry Configuration

void ASFWAsyncCommand::setMaxSpeed(ASFWSpeed speed) {
  if (fMembers) {
    fMembers->fMaxSpeed = static_cast<int32_t>(speed);
  }
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWAsyncCommand[%u] max speed set to %d",
         getId(), static_cast<int>(speed));
}

void ASFWAsyncCommand::setRetries(int32_t retries) {
  fMaxRetries = retries;
  fCurRetries = retries;
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWAsyncCommand[%u] retries set to %d",
         getId(), retries);
}

void ASFWAsyncCommand::setFastRetryCount(uint32_t count) {
  if (fMembers) {
    fMembers->fFastRetryCount = count;
  }
}

uint32_t ASFWAsyncCommand::getFastRetryCount() const {
  return fMembers ? fMembers->fFastRetryCount : 0;
}

void ASFWAsyncCommand::setResponseSpeed(ASFWSpeed speed) {
  if (fMembers) {
    fMembers->fResponseSpeed = static_cast<int32_t>(speed);
  }
}

ASFWSpeed ASFWAsyncCommand::getResponseSpeed() const {
  return fMembers ? static_cast<ASFWSpeed>(fMembers->fResponseSpeed)
                  : ASFWSpeed::s400;
}

#pragma mark - Response Handling

void ASFWAsyncCommand::gotAck(int ackCode) {
  fAckCode = ackCode;
  if (fMembers) {
    fMembers->fAckCode = ackCode;
  }

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWAsyncCommand[%u] got ack %d", getId(),
         ackCode);

  // Handle different ack codes
  switch (ackCode) {
  case 1: // kFWAckComplete
    // Transaction completed successfully
    break;
  case 2: // kFWAckPending
    // Response will come later
    break;
  case 4: // kFWAckBusyX
  case 5: // kFWAckBusyA
  case 6: // kFWAckBusyB
    // Target busy - may retry
    if (fCurRetries > 0) {
      fCurRetries--;
      // TODO: Implement retry logic
    } else {
      complete(kIOFireWireResponseBase + 4); // kFWResponseConflictError
    }
    break;
  case 13:                                 // kFWAckDataError
    complete(kIOFireWireResponseBase + 5); // kFWResponseDataError
    break;
  case 14:                                 // kFWAckTypeError
    complete(kIOFireWireResponseBase + 6); // kFWResponseTypeError
    break;
  default:
    complete(kIOReturnIOError);
    break;
  }
}

kern_return_t ASFWAsyncCommand::updateGeneration() {
  // TODO: Update generation and nodeID from device/controller
  // This would typically be called after bus reset
  return kIOReturnSuccess;
}

kern_return_t ASFWAsyncCommand::updateNodeID(uint32_t generation,
                                             uint16_t nodeID) {
  fGeneration = generation;
  fNodeID = nodeID;
  fAddress.nodeID = nodeID;

  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWAsyncCommand[%u] updated nodeID to 0x%x, gen=%u", getId(),
         nodeID, generation);

  return kIOReturnSuccess;
}

#pragma mark - Transaction Management

kern_return_t ASFWAsyncCommand::allocateTransaction() {
  // TODO: Allocate transaction from ASOHCI
  // This would get a transaction context for tracking
  fTrans = nullptr; // Placeholder
  return kIOReturnSuccess;
}

void ASFWAsyncCommand::freeTransaction() {
  if (fTrans) {
    // TODO: Return transaction to ASOHCI pool
    fTrans = nullptr;
  }
}

kern_return_t ASFWAsyncCommand::submitToATManager() {
  // TODO: Submit transaction to ASOHCI AT manager
  // This would build the appropriate descriptors and queue them
  return kIOReturnUnsupported;
}

#pragma mark - Progress Checking

kern_return_t ASFWAsyncCommand::checkProgress() {
  // Check if command has timed out
  if (fTimeoutMs > 0 && fDeadlineNs > 0) {
    uint64_t currentNs = 0; // TODO: Get current time
    if (currentNs > fDeadlineNs) {
      os_log(OS_LOG_DEFAULT, "ASFW: ASFWAsyncCommand[%u] timed out", getId());
      return cancel(kIOReturnTimeout);
    }
  }

  return ASFWCommand::checkProgress();
}

#pragma mark - Member Variables Management

bool ASFWAsyncCommand::createMemberVariables() {
  fMembers = new ASFWAsyncCommand::MemberVariables();
  if (!fMembers) {
    return false;
  }

  // Initialize defaults
  fMembers->fSubclassMembers = nullptr;
  fMembers->fMaxSpeed = static_cast<int32_t>(ASFWSpeed::s800);
  fMembers->fAckCode = 0;
  fMembers->fResponseCode = 0;
  fMembers->fFastRetryCount = 0;
  fMembers->fResponseSpeed = static_cast<int32_t>(ASFWSpeed::s400);
  fMembers->fForceBlockRequests = false;

  return true;
}

void ASFWAsyncCommand::destroyMemberVariables() {
  if (fMembers) {
    // Note: fSubclassMembers is managed by subclasses, not freed here
    delete fMembers;
    fMembers = nullptr;
  }
}