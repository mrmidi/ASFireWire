//
// ASFWCommand.hpp
// Base class for all ASFireWire commands - DriverKit reimplementation of
// IOFWCommand
//
// Provides OSObject lifecycle, state management, completion handling, and
// timeout support. Mirrors IOFWCommand from IOFireWireFamily.kmodproj but
// adapted for DriverKit.
//

#pragma once

#include <DriverKit/IOReturn.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSMetaClass.h>
#include <DriverKit/OSObject.h>
#include <atomic>
#include <functional>

// FireWire-specific codes
#include "../Shared/FWCodes.hpp"

// Forward declarations
class ASOHCI;
class ASFWCommand;

// Command completion callback type
using ASFWCommandCompletion = std::function<void(kern_return_t status)>;

// Command queue structure (mirrors IOFWCmdQ from legacy)
struct ASFWCmdQ {
  ASFWCommand *fHead = nullptr;
  ASFWCommand *fTail = nullptr;

  bool executeQueue(bool all = false);
  virtual void headChanged(ASFWCommand *oldHead);
  virtual ~ASFWCmdQ() = default;

  void checkProgress();

  // Utility methods
  bool isEmpty() const { return fHead == nullptr; }
  uint32_t getLength() const;
  ASFWCommand *findCommand(uint32_t commandId) const;
  void clearQueue();
};

// Forward declarations
class ASOHCI;
class ASFWCommand;

// Command completion callback type
using ASFWCommandCompletion = std::function<void(kern_return_t status)>;

#pragma mark -

//
// ASFWCommand
// Base class for all FireWire commands - provides state, queueing hooks, and
// completion
//
class ASFWCommand : public OSObject {

protected:
  // Core state
  std::atomic<kern_return_t> fStatus{kIOReturnNotReady};
  std::atomic<bool> fCompleted{false};
  ASOHCI *fControl{nullptr};

  // Queue management
  ASFWCommand *fQueuePrev{nullptr};
  ASFWCommand *fQueueNext{nullptr};
  ASFWCmdQ *fQueue{nullptr};

  // Timeout support
  uint32_t fTimeoutMs{0};  // Timeout in milliseconds
  uint64_t fDeadlineNs{0}; // Absolute deadline in nanoseconds

  // Completion handling
  ASFWCommandCompletion fCompletion{nullptr};

  // Sync support (for synchronous command execution)
  bool fSync{false};
  void *fSyncWakeup{nullptr};

  // Generation tracking for bus reset validation
  uint32_t fGeneration{0};

  // Command identification for tracing
  uint32_t fId{0};
  static std::atomic<uint32_t> sNextId;

  // Private methods
  virtual kern_return_t complete(kern_return_t status);
  virtual void updateTimer();
  virtual kern_return_t startExecution();

  // Abstract execute method - must be implemented by subclasses
  virtual kern_return_t execute() = 0;

public:
  // OSObject lifecycle
  virtual bool init() override;
  virtual void free() override;

  // Initialization
  virtual bool initWithController(ASOHCI *control);

  // Core command API
  virtual kern_return_t submit(bool queue = false);
  virtual kern_return_t cancel(kern_return_t reason = kIOReturnAborted);

  // Completion setup
  virtual void setCompletion(ASFWCommandCompletion completion);
  virtual void setTimeout(uint32_t timeoutMs);
  virtual void setGeneration(uint32_t generation) { fGeneration = generation; }

  // State access
  kern_return_t getStatus() const { return fStatus.load(); }
  uint32_t getGeneration() const { return fGeneration; }
  uint32_t getId() const { return fId; }
  bool isCompleted() const { return fCompleted.load(); }

  // Queue management
  virtual void setHead(ASFWCmdQ &queue);
  virtual void insertAfter(ASFWCommand &prev);
  virtual void removeFromQ();

  // Queue navigation
  ASFWCommand *getPrevious() const { return fQueuePrev; }
  ASFWCommand *getNext() const { return fQueueNext; }
  uint64_t getDeadline() const { return fDeadlineNs; }

  // Progress checking
  virtual kern_return_t checkProgress();

  // Utility
  bool isBusy() const {
    kern_return_t status = fStatus.load();
    return (status == kIOReturnBusy || status == kIOFireWirePending);
  }

  // Friend for queue management
  friend struct ASFWCmdQ;
};