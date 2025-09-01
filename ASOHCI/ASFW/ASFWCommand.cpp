//
// ASFWCommand.cpp
// Implementation of base ASFWCommand class
//

#include "ASFWCommand.hpp"
#include <DriverKit/IOLib.h>
#include <DriverKit/OSAction.h>
#include <atomic>
#include <cassert>
#include <os/log.h>

// Static member initialization
std::atomic<uint32_t> ASFWCommand::sNextId{1};

// No OSObject boilerplate macro needed for DriverKit -
// OSDeclareDefaultStructors handles it

#pragma mark - Lifecycle

bool ASFWCommand::init() {
  if (!OSObject::init()) {
    return false;
  }

  // Assign unique ID for tracing
  fId = sNextId.fetch_add(1);

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWCommand[%u] initialized", fId);
  return true;
}

void ASFWCommand::free() {
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWCommand[%u] freed", fId);

  // Clean up completion callback
  fCompletion = nullptr;

  // Clean up sync wakeup if present
  if (fSyncWakeup) {
    // TODO: Release sync object
    fSyncWakeup = nullptr;
  }

  OSObject::free();
}

bool ASFWCommand::initWithController(ASOHCI *control) {
  if (!init()) {
    return false;
  }

  fControl = control;
  fStatus = kIOReturnNotReady;

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWCommand[%u] initialized with controller",
         fId);
  return true;
}

#pragma mark - Command Execution

kern_return_t ASFWCommand::submit(bool queue) {
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWCommand[%u] submit (queue=%d)", fId, queue);

  kern_return_t result;

  // TODO: Acquire workloop gate when available
  // fControl->closeGate();

  if (queue) {
    // TODO: Add to pending queue
    // ASFWCmdQ& pendingQ = fControl->getPendingQ();
    // Add to queue...
    result = fStatus = kIOFireWirePending;
  } else {
    result = fStatus = startExecution();
  }

  // TODO: Release workloop gate
  // fControl->openGate();

  if (result == kIOReturnBusy || result == kIOFireWirePending) {
    result = kIOReturnSuccess;
  }

  // TODO: Handle sync completion
  if (fSync) {
    // Wait for completion if synchronous
    // result = fSyncWakeup->wait();
  }

  return result;
}

kern_return_t ASFWCommand::startExecution() {
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWCommand[%u] startExecution", fId);

  // Update deadline if timeout is set
  updateTimer();

  return execute();
}

kern_return_t ASFWCommand::complete(kern_return_t status) {
  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWCommand[%u] complete with status 0x%{public}x", fId,
         status);

  // Remove from queue
  removeFromQ();

  // Mark as completed
  fCompleted = true;

  // Update status
  fStatus = status;

  // Call completion callback if set
  if (fCompletion) {
    fCompletion(status);
  }

  // TODO: Handle sync wakeup
  if (fSync && fSyncWakeup) {
    // Signal sync completion
    // fSyncWakeup->signal();
  }

  return status;
}

kern_return_t ASFWCommand::cancel(kern_return_t reason) {
  os_log(OS_LOG_DEFAULT,
         "ASFW: ASFWCommand[%u] cancel with reason 0x%{public}x", fId, reason);

  kern_return_t result = kIOReturnSuccess;

  // TODO: Acquire gate
  // fControl->closeGate();

  // Protect against release during completion
  retain();

  result = complete(reason);

  // TODO: Release gate
  // fControl->openGate();

  release();

  return result;
}

#pragma mark - Completion Setup

void ASFWCommand::setCompletion(ASFWCommandCompletion completion) {
  fCompletion = completion;
  os_log(OS_LOG_DEFAULT, "ASFW: ASFWCommand[%u] completion callback set", fId);
}

void ASFWCommand::setTimeout(uint32_t timeoutMs) {
  fTimeoutMs = timeoutMs;

  if (fTimeoutMs > 0) {
    // Convert to nanoseconds and add to current time
    uint64_t timeoutNs = (uint64_t)timeoutMs * 1000000ULL; // ms to ns
    uint64_t currentNs = 0; // TODO: Get current time in ns
    fDeadlineNs = currentNs + timeoutNs;
  } else {
    fDeadlineNs = 0;
  }

  os_log(OS_LOG_DEFAULT, "ASFW: ASFWCommand[%u] timeout set to %u ms", fId,
         timeoutMs);
}

#pragma mark - Timer Management

void ASFWCommand::updateTimer() {
  if (fTimeoutMs == 0) {
    return;
  }

  // TODO: Implement timer update logic
  // This would involve updating position in timeout queue
  // Similar to IOFWCommand::updateTimer()
}

#pragma mark - Queue Management

void ASFWCommand::setHead(ASFWCmdQ &queue) {
  assert(fQueue == nullptr);

  ASFWCommand *oldHead = queue.fHead;
  queue.fHead = this;
  fQueue = &queue;
  fQueuePrev = nullptr;
  fQueueNext = oldHead;

  if (!oldHead) {
    queue.fTail = this;
  } else {
    oldHead->fQueuePrev = this;
  }

  queue.headChanged(oldHead);
}

void ASFWCommand::insertAfter(ASFWCommand &prev) {
  ASFWCommand *next;

  assert(fQueue == nullptr);

  next = prev.fQueueNext;
  fQueue = prev.fQueue;
  prev.fQueueNext = this;
  fQueuePrev = &prev;
  fQueueNext = next;

  if (!next) {
    fQueue->fTail = this;
  } else {
    next->fQueuePrev = this;
  }
}

void ASFWCommand::removeFromQ() {
  if (!fQueue) {
    return;
  }

  ASFWCmdQ *queue = fQueue;
  ASFWCommand *oldHead = queue->fHead;

  if (fQueuePrev) {
    assert(fQueuePrev->fQueueNext == this);
    fQueuePrev->fQueueNext = fQueueNext;
  } else {
    assert(queue->fHead == this);
    queue->fHead = fQueueNext;
  }

  if (fQueueNext) {
    assert(fQueueNext->fQueuePrev == this);
    fQueueNext->fQueuePrev = fQueueNext;
  } else {
    assert(queue->fTail == this);
    queue->fTail = fQueuePrev;
  }

  fQueue = nullptr;

  if (oldHead == this) {
    queue->headChanged(this);
  }
}

#pragma mark - Progress Checking

kern_return_t ASFWCommand::checkProgress() {
  // Default implementation - subclasses can override
  return kIOReturnSuccess;
}

#pragma mark - ASFWCmdQ Implementation

bool ASFWCmdQ::executeQueue(bool all) {
  bool hasMoreCommands = false;
  ASFWCommand *cmd = fHead;

  while (cmd) {
    ASFWCommand *nextCmd = cmd->getNext();

    // Execute the command
    kern_return_t result = cmd->startExecution();

    // If command completed immediately, it will have been removed from queue
    // If it's pending, it stays in queue for timeout tracking

    if (result == kIOReturnBusy || result == kIOFireWirePending) {
      // Command is still active, keep it in queue
      hasMoreCommands = true;
    } else {
      // Command completed immediately, it should have been removed from queue
      // by the complete() method
    }

    // If not processing all commands, stop after first one
    if (!all) {
      hasMoreCommands = (nextCmd != nullptr);
      break;
    }

    cmd = nextCmd;
  }

  return hasMoreCommands;
}

void ASFWCmdQ::headChanged(ASFWCommand *oldHead) {
  // Default implementation - controller can override for specific behavior
  // This is called when the head command changes (new command becomes head or
  // head is removed)

  if (fHead != oldHead) {
    // Head has actually changed
    if (fHead) {
      os_log(OS_LOG_DEFAULT, "ASFW: Queue head changed to command[%u]",
             fHead->getId());
    } else {
      os_log(OS_LOG_DEFAULT, "ASFW: Queue is now empty");
    }
  }
}

void ASFWCmdQ::checkProgress() {
  // Check progress on all commands in queue and handle timeouts
  ASFWCommand *cmd = fHead;
  ASFWCommand *nextCmd;

  while (cmd) {
    nextCmd = cmd->getNext();

    // Check if command has timed out
    if (cmd->getDeadline() > 0) {
      // TODO: Get current time in nanoseconds
      uint64_t currentNs = 0; // Placeholder - need to implement time source

      if (currentNs > cmd->getDeadline()) {
        os_log(OS_LOG_DEFAULT, "ASFW: Command[%u] timed out, cancelling",
               cmd->getId());
        cmd->cancel(kIOReturnTimeout);
        // cancel() will remove the command from queue
      }
    }

    // Call command's progress check method
    kern_return_t result = cmd->checkProgress();
    if (result != kIOReturnSuccess) {
      os_log(OS_LOG_DEFAULT,
             "ASFW: Command[%u] progress check failed: 0x%{public}x",
             cmd->getId(), result);
    }

    cmd = nextCmd;
  }
}

#pragma mark - Queue Utility Methods

uint32_t ASFWCmdQ::getLength() const {
  uint32_t count = 0;
  ASFWCommand *cmd = fHead;

  while (cmd) {
    count++;
    cmd = cmd->getNext();
  }

  return count;
}

ASFWCommand *ASFWCmdQ::findCommand(uint32_t commandId) const {
  ASFWCommand *cmd = fHead;

  while (cmd) {
    if (cmd->getId() == commandId) {
      return cmd;
    }
    cmd = cmd->getNext();
  }

  return nullptr;
}

void ASFWCmdQ::clearQueue() {
  ASFWCommand *cmd = fHead;

  while (cmd) {
    ASFWCommand *nextCmd = cmd->getNext();
    cmd->cancel(kIOReturnAborted);
    cmd = nextCmd;
  }

  // Queue should be empty now after cancelling all commands
  assert(fHead == nullptr && fTail == nullptr);
}