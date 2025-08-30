//
// ASFWReadCommand.cpp
// Implementation of ASFWReadCommand
//

#include "ASFWReadCommand.hpp"
#include <DriverKit/IOLib.h>
#include <os/log.h>

// No OSObject boilerplate macro needed for DriverKit -
// OSDeclareDefaultStructors handles it

#pragma mark - Lifecycle

bool ASFWReadCommand::init() {
  if (!ASFWAsyncCommand::init()) {
    return false;
  }

  fWrite = false; // This is a read command
  fTCode = 4;     // TCode for block read request

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWReadCommand[%u] initialized", getId());
  return true;
}

bool ASFWReadCommand::initAll(ASOHCI *control, uint32_t generation,
                              ASFWAddress devAddress,
                              IOMemoryDescriptor *responseBuffer,
                              ASFWAsyncCompletion completion, void *refcon) {
  if (!ASFWAsyncCommand::initAll(control, generation, devAddress,
                                 responseBuffer, completion, refcon)) {
    return false;
  }

  fWrite = false;
  fTCode = 4; // TCode for block read request

  // Set response buffer
  fResponseMD = responseBuffer;
  if (fResponseMD) {
    fResponseMD->retain();
    uint64_t length;
    if (fResponseMD->GetLength(&length) == kIOReturnSuccess) {
      fSize = static_cast<uint32_t>(length);
    }
  }

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWReadCommand[%u] initAll - size=%d", getId(),
         fSize);
  return true;
}

bool ASFWReadCommand::initAll(ASFWAddress devAddress,
                              IOMemoryDescriptor *responseBuffer,
                              ASFWAsyncCompletion completion, void *refcon,
                              bool failOnReset) {
  if (!ASFWAsyncCommand::initAll(devAddress, responseBuffer, completion, refcon,
                                 failOnReset)) {
    return false;
  }

  fWrite = false;
  fTCode = 4; // TCode for block read request

  // Set response buffer
  fResponseMD = responseBuffer;
  if (fResponseMD) {
    fResponseMD->retain();
    uint64_t length;
    if (fResponseMD->GetLength(&length) == kIOReturnSuccess) {
      fSize = static_cast<uint32_t>(length);
    }
  }

  return true;
}

kern_return_t ASFWReadCommand::reinit(ASFWAddress devAddress,
                                      IOMemoryDescriptor *responseBuffer,
                                      ASFWAsyncCompletion completion,
                                      void *refcon, bool failOnReset) {
  kern_return_t result = ASFWAsyncCommand::reinit(
      devAddress, responseBuffer, completion, refcon, failOnReset);

  if (result == kIOReturnSuccess) {
    fWrite = false;
    fTCode = 4; // TCode for block read request

    // Update response buffer
    fResponseMD = responseBuffer;
    if (fResponseMD) {
      fResponseMD->retain();
      uint64_t length;
      if (fResponseMD->GetLength(&length) == kIOReturnSuccess) {
        fSize = static_cast<uint32_t>(length);
      }
    }
  }

  return result;
}

kern_return_t ASFWReadCommand::reinit(uint32_t generation,
                                      ASFWAddress devAddress,
                                      IOMemoryDescriptor *responseBuffer,
                                      ASFWAsyncCompletion completion,
                                      void *refcon) {
  fGeneration = generation;
  return reinit(devAddress, responseBuffer, completion, refcon, fFailOnReset);
}

#pragma mark - Execution

kern_return_t ASFWReadCommand::execute() {
  os_log(
      OS_LOG_DEFAULT,
      "ASFW: ASFWReadCommand[%u] execute - nodeID=0x%x, addr=0x%x:%x, size=%d",
      getId(), fNodeID, fAddress.addressHi, fAddress.addressLo, fSize);

  fStatus = kIOReturnBusy;

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
           "ASFW: ASFWReadCommand[%u] failed to allocate transaction", getId());
    return result;
  }

  // TODO: Submit to AT manager
  // This would build OUTPUT_MORE_Immediate header + OUTPUT_LAST for read
  // request The AR context would handle the response and call gotPacket()

  result = submitToATManager();

  // Handle immediate failure
  if (result != kIOReturnSuccess) {
    freeTransaction();
    complete(result);
  }

  return fStatus.load();
}

#pragma mark - Response Handling

void ASFWReadCommand::gotPacket(int rcode, const void *data, int size) {
  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWReadCommand[%u] gotPacket - rcode=%d, size=%d", getId(),
         rcode, size);

  setResponseCode(rcode);

  if (rcode != 0) { // kFWResponseComplete
    os_log(OS_LOG_DEFAULT, "ASFW: ASFWReadCommand[%u] response error: rcode=%d",
           getId(), rcode);
    complete(kIOFireWireResponseBase + rcode);
    return;
  }

  // Copy data to response buffer
  if (fResponseMD && data && size > 0) {
    // TODO: Copy data to response buffer at appropriate offset
    // fResponseMD->writeBytes(fBytesTransferred, data, size);
    fBytesTransferred += size;
  }

  // Check if read is complete
  if (fBytesTransferred >= fSize) {
    // Read complete
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWReadCommand[%u] read complete - %u bytes transferred",
           getId(), fBytesTransferred);
    complete(kIOReturnSuccess);
  } else {
    // Continue reading
    fAddress.addressLo += fPackSize;
    fSize -= fPackSize;

    // Update packet size for next read
    fPackSize = fSize;
    if (fPackSize > fMaxPack) {
      fPackSize = fMaxPack;
    }

    // Reset retry count
    fCurRetries = fMaxRetries;

    // Free current transaction
    freeTransaction();

    // Submit next read
    kern_return_t result = execute();
    if (result != kIOReturnBusy && result != kIOFireWirePending) {
      complete(result);
    }
  }
}