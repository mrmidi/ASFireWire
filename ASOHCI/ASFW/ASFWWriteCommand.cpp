//
// ASFWWriteCommand.cpp
// Implementation of ASFWWriteCommand
//

#include "ASFWWriteCommand.hpp"
#include <DriverKit/IOLib.h>
#include <os/log.h>

// No OSObject boilerplate macro needed for DriverKit -
// OSDeclareDefaultStructors handles it

#pragma mark - Lifecycle

bool ASFWWriteCommand::init() {
  if (!ASFWAsyncCommand::init()) {
    return false;
  }

  if (!createMemberVariables()) {
    return false;
  }

  fWrite = true; // This is a write command
  fTCode = 1;    // TCode for write request (block write)

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWWriteCommand[%u] initialized", getId());
  return true;
}

void ASFWWriteCommand::free() {
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWWriteCommand[%u] freed", getId());

  // Clean up member variables
  destroyMemberVariables();

  ASFWAsyncCommand::free();
}

bool ASFWWriteCommand::initAll(ASOHCI *control, uint32_t generation,
                               ASFWAddress devAddress,
                               IOMemoryDescriptor *requestBuffer,
                               ASFWAsyncCompletion completion, void *refcon) {
  if (!ASFWAsyncCommand::initAll(control, generation, devAddress, requestBuffer,
                                 completion, refcon)) {
    return false;
  }

  fWrite = true;
  fTCode = 1; // TCode for write request (block write)

  // Set request buffer
  fRequestMD = requestBuffer;
  if (fRequestMD) {
    fRequestMD->retain();
    uint64_t length;
    if (fRequestMD->GetLength(&length) == kIOReturnSuccess) {
      fSize = static_cast<uint32_t>(length);
    }
  }

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWWriteCommand[%u] initAll - size=%d",
         getId(), fSize);
  return true;
}

bool ASFWWriteCommand::initAll(ASFWAddress devAddress,
                               IOMemoryDescriptor *requestBuffer,
                               ASFWAsyncCompletion completion, void *refcon,
                               bool failOnReset) {
  if (!ASFWAsyncCommand::initAll(devAddress, requestBuffer, completion, refcon,
                                 failOnReset)) {
    return false;
  }

  fWrite = true;
  fTCode = 1; // TCode for write request (block write)

  // Set request buffer
  fRequestMD = requestBuffer;
  if (fRequestMD) {
    fRequestMD->retain();
    uint64_t length;
    if (fRequestMD->GetLength(&length) == kIOReturnSuccess) {
      fSize = static_cast<uint32_t>(length);
    }
  }

  return true;
}

kern_return_t ASFWWriteCommand::reinit(ASFWAddress devAddress,
                                       IOMemoryDescriptor *requestBuffer,
                                       ASFWAsyncCompletion completion,
                                       void *refcon, bool failOnReset) {
  kern_return_t result = ASFWAsyncCommand::reinit(
      devAddress, requestBuffer, completion, refcon, failOnReset);

  if (result == kIOReturnSuccess) {
    fWrite = true;
    fTCode = 1; // TCode for write request (block write)

    // Update request buffer
    fRequestMD = requestBuffer;
    if (fRequestMD) {
      fRequestMD->retain();
      uint64_t length;
      if (fRequestMD->GetLength(&length) == kIOReturnSuccess) {
        fSize = static_cast<uint32_t>(length);
      }
    }
  }

  return result;
}

kern_return_t ASFWWriteCommand::reinit(uint32_t generation,
                                       ASFWAddress devAddress,
                                       IOMemoryDescriptor *requestBuffer,
                                       ASFWAsyncCompletion completion,
                                       void *refcon) {
  fGeneration = generation;
  return reinit(devAddress, requestBuffer, completion, refcon, fFailOnReset);
}

#pragma mark - Configuration

void ASFWWriteCommand::setDeferredNotify(bool state) {
  if (fWriteMembers) {
    fWriteMembers->fDeferredNotify = state;
  }
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWWriteCommand[%u] deferred notify set to %d",
         getId(), state);
}

void ASFWWriteCommand::setFastRetryOnBusy(bool state) {
  if (fWriteMembers) {
    fWriteMembers->fFastRetryOnBusy = state;
  }
  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWWriteCommand[%u] fast retry on busy set to %d", getId(),
         state);
}

#pragma mark - Execution

kern_return_t ASFWWriteCommand::execute() {
  os_log(
      OS_LOG_DEFAULT,
      "ASFW: ASFWWriteCommand[%u] execute - nodeID=0x%x, addr=0x%x:%x, size=%d",
      getId(), fNodeID, fAddress.addressHi, fAddress.addressLo, fSize);

  fStatus = kIOReturnBusy;

  // Validate that we have a request buffer
  if (!fRequestMD) {
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWWriteCommand[%u] no request buffer provided", getId());
    return complete(kIOReturnBadArgument);
  }

  // Validate generation if not failing on reset
  if (!fFailOnReset) {
    // TODO: Update nodeID and generation from controller
    // fControl->getNodeIDGeneration(fGeneration, fNodeID);
    // fSpeed = fControl->FWSpeed(fNodeID);
  }

  // Calculate packet size
  fPackSize = fSize;
  if (fPackSize > fMaxPack) {
    fPackSize = fMaxPack;
  }

  // TODO: Get max packet size from controller
  // int maxPack = (1 << fControl->maxPackLog(fWrite, fNodeID));
  // if (maxPack < fPackSize) {
  //     fPackSize = maxPack;
  // }

  // Allocate transaction
  kern_return_t result = allocateTransaction();
  if (result != kIOReturnSuccess) {
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWWriteCommand[%u] failed to allocate transaction",
           getId());
    return result;
  }

  // TODO: Build write flags
  uint32_t flags = 0; // kIOFWWriteFlagsNone
  if (fWriteMembers) {
    if (fWriteMembers->fDeferredNotify) {
      flags |= 0x00000001; // kIOFWWriteFlagsDeferredNotify
    }
    if (fForceBlockRequests) {
      flags |= 0x00000004; // kIOFWWriteBlockRequest
    }
  }

  // TODO: Submit to AT manager
  // This would build OUTPUT_MORE_Immediate header + OUTPUT_LAST with DMA from
  // request buffer

  result = submitToATManager();

  // Handle immediate failure
  if (result != kIOReturnSuccess) {
    freeTransaction();
    complete(result);
  }

  return fStatus.load();
}

#pragma mark - Response Handling

void ASFWWriteCommand::gotPacket(int rcode, const void *data, int size) {
  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWWriteCommand[%u] gotPacket - rcode=%d, size=%d", getId(),
         rcode, size);

  setResponseCode(rcode);

  if (rcode != 0) { // kFWResponseComplete
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWWriteCommand[%u] response error: rcode=%d", getId(),
           rcode);

    // Handle busy response with fast retry if enabled
    if (rcode == 4 && fWriteMembers && fWriteMembers->fFastRetryOnBusy &&
        fCurRetries > 0) {
      fCurRetries--;
      // TODO: Implement fast retry logic
      os_log(OS_LOG_DEFAULT,
             "ASFW: ASFWWriteCommand[%u] fast retry on busy (%d retries left)",
             getId(), fCurRetries);
      return;
    }

    complete(kIOFireWireResponseBase + rcode);
    return;
  }

  // Update bytes transferred
  fBytesTransferred += fPackSize;

  // Check if write is complete
  if (fBytesTransferred >= fSize) {
    // Write complete
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWWriteCommand[%u] write complete - %u bytes transferred",
           getId(), fBytesTransferred);
    complete(kIOReturnSuccess);
  } else {
    // Continue writing
    fAddress.addressLo += fPackSize;
    fSize -= fPackSize;

    // Update packet size for next write
    fPackSize = fSize;
    if (fPackSize > fMaxPack) {
      fPackSize = fMaxPack;
    }

    // Reset retry count
    fCurRetries = fMaxRetries;

    // Free current transaction
    freeTransaction();

    // Submit next write
    kern_return_t result = execute();
    if (result != kIOReturnBusy && result != kIOFireWirePending) {
      complete(result);
    }
  }
}

#pragma mark - Member Variables Management

bool ASFWWriteCommand::createMemberVariables() {
  fWriteMembers = new ASFWWriteCommand::MemberVariables();
  if (!fWriteMembers) {
    return false;
  }

  // Initialize defaults
  fWriteMembers->fDeferredNotify = false;
  fWriteMembers->fFastRetryOnBusy = false;

  return true;
}

void ASFWWriteCommand::destroyMemberVariables() {
  if (fWriteMembers) {
    delete fWriteMembers;
    fWriteMembers = nullptr;
  }
}