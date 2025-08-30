//
// ASFWWriteQuadCommand.cpp
// Implementation of ASFWWriteQuadCommand
//

#include "ASFWWriteQuadCommand.hpp"
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <os/log.h>

// No OSObject boilerplate macro needed for DriverKit -
// OSDeclareDefaultStructors handles it

#pragma mark - Lifecycle

bool ASFWWriteQuadCommand::init() {
  if (!ASFWAsyncCommand::init()) {
    return false;
  }

  if (!createMemberVariables()) {
    return false;
  }

  fWrite = true; // This is a write command
  fTCode = 0;    // TCode for quadlet write request
  fSize = 4;     // Always 4 bytes for quadlet

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWWriteQuadCommand[%u] initialized", getId());
  return true;
}

void ASFWWriteQuadCommand::free() {
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWWriteQuadCommand[%u] freed", getId());

  // Clean up quad buffer if we allocated it
  if (fQuads) {
    IOFree(fQuads, fNumQuads * sizeof(uint32_t));
    fQuads = nullptr;
  }

  // Clean up memory descriptor
  destroyMemoryDescriptor();

  // Clean up member variables
  destroyMemberVariables();

  ASFWAsyncCommand::free();
}

bool ASFWWriteQuadCommand::initAll(ASOHCI *control, uint32_t generation,
                                   ASFWAddress devAddress, uint32_t *quads,
                                   int numQuads, ASFWAsyncCompletion completion,
                                   void *refcon) {
  if (numQuads > 8) { // kMaxWriteQuads equivalent
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWWriteQuadCommand[%u] too many quads: %d (max 8)", getId(),
           numQuads);
    return false;
  }

  if (!ASFWAsyncCommand::initAll(control, generation, devAddress, nullptr,
                                 completion, refcon)) {
    return false;
  }

  fWrite = true;
  fTCode = 0; // TCode for quadlet write request
  fSize = 4;  // Always 4 bytes per quadlet

  // Store quadlet data
  setQuads(quads, numQuads);

  // Create memory descriptor for the quadlet data
  if (!createMemoryDescriptor()) {
    return false;
  }

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWWriteQuadCommand[%u] initAll - numQuads=%d",
         getId(), numQuads);
  return true;
}

bool ASFWWriteQuadCommand::initAll(ASFWAddress devAddress, uint32_t *quads,
                                   int numQuads, ASFWAsyncCompletion completion,
                                   void *refcon, bool failOnReset) {
  if (numQuads > 8) { // kMaxWriteQuads equivalent
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWWriteQuadCommand[%u] too many quads: %d (max 8)", getId(),
           numQuads);
    return false;
  }

  if (!ASFWAsyncCommand::initAll(devAddress, nullptr, completion, refcon,
                                 failOnReset)) {
    return false;
  }

  fWrite = true;
  fTCode = 0; // TCode for quadlet write request
  fSize = 4;  // Always 4 bytes per quadlet

  // Store quadlet data
  setQuads(quads, numQuads);

  // Create memory descriptor for the quadlet data
  if (!createMemoryDescriptor()) {
    return false;
  }

  return true;
}

kern_return_t ASFWWriteQuadCommand::reinit(ASFWAddress devAddress,
                                           uint32_t *quads, int numQuads,
                                           ASFWAsyncCompletion completion,
                                           void *refcon, bool failOnReset) {
  if (numQuads > 8) { // kMaxWriteQuads equivalent
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWWriteQuadCommand[%u] too many quads: %d (max 8)", getId(),
           numQuads);
    return kIOReturnBadArgument;
  }

  // Clean up old quad buffer
  if (fQuads) {
    IOFree(fQuads, fNumQuads * sizeof(uint32_t));
    fQuads = nullptr;
  }

  // Clean up old memory descriptor
  destroyMemoryDescriptor();

  kern_return_t result = ASFWAsyncCommand::reinit(
      devAddress, nullptr, completion, refcon, failOnReset);

  if (result == kIOReturnSuccess) {
    fWrite = true;
    fTCode = 0; // TCode for quadlet write request
    fSize = 4;  // Always 4 bytes per quadlet

    // Store new quadlet data
    setQuads(quads, numQuads);

    // Create new memory descriptor
    if (!createMemoryDescriptor()) {
      return kIOReturnNoMemory;
    }
  }

  return result;
}

kern_return_t ASFWWriteQuadCommand::reinit(uint32_t generation,
                                           ASFWAddress devAddress,
                                           uint32_t *quads, int numQuads,
                                           ASFWAsyncCompletion completion,
                                           void *refcon) {
  fGeneration = generation;
  return reinit(devAddress, quads, numQuads, completion, refcon, fFailOnReset);
}

#pragma mark - Configuration

void ASFWWriteQuadCommand::setDeferredNotify(bool state) {
  if (fQuadWriteMembers) {
    fQuadWriteMembers->fDeferredNotify = state;
  }
  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWWriteQuadCommand[%u] deferred notify set to %d", getId(),
         state);
}

#pragma mark - Execution

kern_return_t ASFWWriteQuadCommand::execute() {
  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWWriteQuadCommand[%u] execute - nodeID=0x%x, addr=0x%x:%x, "
         "numQuads=%u",
         getId(), fNodeID, fAddress.addressHi, fAddress.addressLo, fNumQuads);

  fStatus = kIOReturnBusy;

  // Validate that we have quad data
  if (!fQuads || fNumQuads == 0) {
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWWriteQuadCommand[%u] no quad data provided", getId());
    return complete(kIOReturnBadArgument);
  }

  // Validate generation if not failing on reset
  if (!fFailOnReset) {
    // TODO: Update nodeID and generation from controller
    // fControl->getNodeIDGeneration(fGeneration, fNodeID);
    // fSpeed = fControl->FWSpeed(fNodeID);
  }

  // For quadlet writes, we can write multiple quadlets at once
  // Calculate how many we can send in this packet
  fPackSize = fNumQuads * 4; // 4 bytes per quadlet
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
           "ASFW: ASFWWriteQuadCommand[%u] failed to allocate transaction",
           getId());
    return result;
  }

  // TODO: Submit quadlet write to AT manager
  // This would build OUTPUT_LAST_Immediate descriptor for quadlet write
  // Can use inline data for small transfers or DMA for larger ones

  result = submitToATManager();

  // Handle immediate failure
  if (result != kIOReturnSuccess) {
    freeTransaction();
    complete(result);
  }

  return fStatus.load();
}

#pragma mark - Response Handling

void ASFWWriteQuadCommand::gotPacket(int rcode, const void *data, int size) {
  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWWriteQuadCommand[%u] gotPacket - rcode=%d, size=%d",
         getId(), rcode, size);

  setResponseCode(rcode);

  if (rcode != 0) { // kFWResponseComplete
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWWriteQuadCommand[%u] response error: rcode=%d", getId(),
           rcode);
    complete(kIOFireWireResponseBase + rcode);
    return;
  }

  // Update bytes transferred
  fBytesTransferred += fPackSize;
  fQuadIndex += (fPackSize / 4); // Update quadlet index

  // Check if write is complete
  if (fQuadIndex >= fNumQuads) {
    // Write complete
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWWriteQuadCommand[%u] quadlet write complete - %u quads "
           "written",
           getId(), fNumQuads);
    complete(kIOReturnSuccess);
  } else {
    // Continue writing remaining quadlets
    fAddress.addressLo += fPackSize;

    // Calculate remaining quadlets
    uint32_t remainingQuads = fNumQuads - fQuadIndex;
    fPackSize = remainingQuads * 4;
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

#pragma mark - Helper Methods

void ASFWWriteQuadCommand::setQuads(uint32_t *quads, int numQuads) {
  fQuads = quads;
  fNumQuads = numQuads;
  fQuadIndex = 0;
  fSize = numQuads * 4; // 4 bytes per quadlet
}

bool ASFWWriteQuadCommand::createMemoryDescriptor() {
  if (!fQuads || fNumQuads == 0) {
    return false;
  }

  // TODO: Create memory descriptor from existing quadlet data
  // For now, skip this to get basic compilation working
  /*
  fQuadMD = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut,
                                             fNumQuads * sizeof(uint32_t),
                                             0, &fQuadMD);
  if (!fQuadMD) {
      return false;
  }

  // Copy data to the descriptor
  IOAddressSegment range;
  if (fQuadMD->GetAddressRange(&range) == kIOReturnSuccess) {
      memcpy((void*)range.address, fQuads, fNumQuads * sizeof(uint32_t));
      fQuadMD->SetLength(fNumQuads * sizeof(uint32_t));
  }

  // Store in member variables
  if (fQuadWriteMembers) {
      fQuadWriteMembers->fMemory = fQuadMD;
      fQuadWriteMembers->fMemory->retain();
  }
  */

  return true;
}

void ASFWWriteQuadCommand::destroyMemoryDescriptor() {
  if (fQuadMD) {
    fQuadMD->release();
    fQuadMD = nullptr;
  }

  if (fQuadWriteMembers && fQuadWriteMembers->fMemory) {
    fQuadWriteMembers->fMemory->release();
    fQuadWriteMembers->fMemory = nullptr;
  }
}

#pragma mark - Member Variables Management

bool ASFWWriteQuadCommand::createMemberVariables() {
  fQuadWriteMembers = new ASFWWriteQuadCommand::MemberVariables();
  if (!fQuadWriteMembers) {
    return false;
  }

  // Initialize defaults
  fQuadWriteMembers->fDeferredNotify = false;
  fQuadWriteMembers->fMemory = nullptr;

  return true;
}

void ASFWWriteQuadCommand::destroyMemberVariables() {
  if (fQuadWriteMembers) {
    if (fQuadWriteMembers->fMemory) {
      fQuadWriteMembers->fMemory->release();
      fQuadWriteMembers->fMemory = nullptr;
    }
    delete fQuadWriteMembers;
    fQuadWriteMembers = nullptr;
  }
}