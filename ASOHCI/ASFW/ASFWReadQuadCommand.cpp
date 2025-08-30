//
// ASFWReadQuadCommand.cpp
// Implementation of ASFWReadQuadCommand
//

#include "ASFWReadQuadCommand.hpp"
#include <DriverKit/IOLib.h>
#include <os/log.h>

// No OSObject boilerplate macro needed for DriverKit -
// OSDeclareDefaultStructors handles it

#pragma mark - Lifecycle

bool ASFWReadQuadCommand::init() {
  if (!ASFWAsyncCommand::init()) {
    return false;
  }

  if (!createMemberVariables()) {
    return false;
  }

  fWrite = false; // This is a read command
  fTCode = 4;     // TCode for quadlet read request (same as block read)
  fSize = 4;      // Always 4 bytes for quadlet

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWReadQuadCommand[%u] initialized", getId());
  return true;
}

void ASFWReadQuadCommand::free() {
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWReadQuadCommand[%u] freed", getId());

  // Clean up quad buffer if we allocated it
  if (fQuads) {
    IOFree(fQuads, fNumQuads * sizeof(uint32_t));
    fQuads = nullptr;
  }

  // Clean up member variables
  destroyMemberVariables();

  ASFWAsyncCommand::free();
}

bool ASFWReadQuadCommand::initAll(ASOHCI *control, uint32_t generation,
                                  ASFWAddress devAddress, uint32_t *quads,
                                  int numQuads, ASFWAsyncCompletion completion,
                                  void *refcon) {
  if (numQuads > 1) {
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWReadQuadCommand[%u] warning: numQuads=%d, only first "
           "will be used",
           getId(), numQuads);
  }

  if (!ASFWAsyncCommand::initAll(control, generation, devAddress, nullptr,
                                 completion, refcon)) {
    return false;
  }

  fWrite = false;
  fTCode = 4; // TCode for quadlet read request
  fSize = 4;  // Always 4 bytes

  // Store quadlet buffer
  fQuads = quads;
  fNumQuads = numQuads;
  fQuadIndex = 0;

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWReadQuadCommand[%u] initAll - numQuads=%d",
         getId(), numQuads);
  return true;
}

bool ASFWReadQuadCommand::initAll(ASFWAddress devAddress, uint32_t *quads,
                                  int numQuads, ASFWAsyncCompletion completion,
                                  void *refcon, bool failOnReset) {
  if (numQuads > 1) {
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWReadQuadCommand[%u] warning: numQuads=%d, only first "
           "will be used",
           getId(), numQuads);
  }

  if (!ASFWAsyncCommand::initAll(devAddress, nullptr, completion, refcon,
                                 failOnReset)) {
    return false;
  }

  fWrite = false;
  fTCode = 4; // TCode for quadlet read request
  fSize = 4;  // Always 4 bytes

  // Store quadlet buffer
  fQuads = quads;
  fNumQuads = numQuads;
  fQuadIndex = 0;

  return true;
}

kern_return_t ASFWReadQuadCommand::reinit(ASFWAddress devAddress,
                                          uint32_t *quads, int numQuads,
                                          ASFWAsyncCompletion completion,
                                          void *refcon, bool failOnReset) {
  if (numQuads > 1) {
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWReadQuadCommand[%u] warning: numQuads=%d, only first "
           "will be used",
           getId(), numQuads);
  }

  // Clean up old quad buffer
  if (fQuads) {
    IOFree(fQuads, fNumQuads * sizeof(uint32_t));
    fQuads = nullptr;
  }

  kern_return_t result = ASFWAsyncCommand::reinit(
      devAddress, nullptr, completion, refcon, failOnReset);

  if (result == kIOReturnSuccess) {
    fWrite = false;
    fTCode = 4; // TCode for quadlet read request
    fSize = 4;  // Always 4 bytes

    // Store new quadlet buffer
    fQuads = quads;
    fNumQuads = numQuads;
    fQuadIndex = 0;
  }

  return result;
}

kern_return_t ASFWReadQuadCommand::reinit(uint32_t generation,
                                          ASFWAddress devAddress,
                                          uint32_t *quads, int numQuads,
                                          ASFWAsyncCompletion completion,
                                          void *refcon) {
  fGeneration = generation;
  return reinit(devAddress, quads, numQuads, completion, refcon, fFailOnReset);
}

#pragma mark - Configuration

void ASFWReadQuadCommand::setPingTime(bool state) {
  if (fQuadMembers) {
    fQuadMembers->fPingTime = state;
  }
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWReadQuadCommand[%u] ping time set to %d",
         getId(), state);
}

#pragma mark - Execution

kern_return_t ASFWReadQuadCommand::execute() {
  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWReadQuadCommand[%u] execute - nodeID=0x%x, addr=0x%x:%x",
         getId(), fNodeID, fAddress.addressHi, fAddress.addressLo);

  fStatus = kIOReturnBusy;

  // Validate that we have a quad buffer
  if (!fQuads || fNumQuads == 0) {
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWReadQuadCommand[%u] no quad buffer provided", getId());
    return complete(kIOReturnBadArgument);
  }

  // Validate generation if not failing on reset
  if (!fFailOnReset) {
    // TODO: Update nodeID and generation from controller
    // fControl->getNodeIDGeneration(fGeneration, fNodeID);
    // fSpeed = fControl->FWSpeed(fNodeID);
  }

  // Allocate transaction
  kern_return_t result = allocateTransaction();
  if (result != kIOReturnSuccess) {
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWReadQuadCommand[%u] failed to allocate transaction",
           getId());
    return result;
  }

  // TODO: Submit quadlet read to AT manager
  // This would build OUTPUT_LAST_Immediate descriptor for quadlet read
  // The AR context would handle the response and call gotPacket()

  result = submitToATManager();

  // Handle immediate failure
  if (result != kIOReturnSuccess) {
    freeTransaction();
    complete(result);
  }

  return fStatus.load();
}

#pragma mark - Response Handling

void ASFWReadQuadCommand::gotPacket(int rcode, const void *data, int size) {
  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWReadQuadCommand[%u] gotPacket - rcode=%d, size=%d", getId(),
         rcode, size);

  setResponseCode(rcode);

  if (rcode != 0) { // kFWResponseComplete
    os_log(OS_LOG_DEFAULT,
           "ASFW: ASFWReadQuadCommand[%u] response error: rcode=%d", getId(),
           rcode);
    complete(kIOFireWireResponseBase + rcode);
    return;
  }

  // Validate response size
  if (size != 4) {
    os_log(
        OS_LOG_DEFAULT,
        "ASFW: ASFWReadQuadCommand[%u] invalid response size: %d (expected 4)",
        getId(), size);
    complete(kIOReturnIOError);
    return;
  }

  // Copy quadlet data
  if (fQuads && fQuadIndex < fNumQuads && data) {
    // Copy the 4-byte quadlet (handle endianness if needed)
    const uint32_t *quadData = static_cast<const uint32_t *>(data);
    fQuads[fQuadIndex] =
        *quadData; // Assume network byte order or handle conversion

    fBytesTransferred = 4;
    fQuadIndex++;

    os_log(OS_LOG_DEFAULT, "ASFW: ASFWReadQuadCommand[%u] quadlet read: 0x%x",
           getId(), fQuads[fQuadIndex - 1]);
  }

  // Quadlet read is always complete after one response
  complete(kIOReturnSuccess);
}

#pragma mark - Member Variables Management

bool ASFWReadQuadCommand::createMemberVariables() {
  fQuadMembers = new ASFWReadQuadCommand::MemberVariables();
  if (!fQuadMembers) {
    return false;
  }

  // Initialize defaults
  fQuadMembers->fPingTime = false;

  return true;
}

void ASFWReadQuadCommand::destroyMemberVariables() {
  if (fQuadMembers) {
    delete fQuadMembers;
    fQuadMembers = nullptr;
  }
}