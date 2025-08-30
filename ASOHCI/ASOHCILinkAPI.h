//
//  ASOHCILinkAPI.h
//  ASOHCI
//
//  Created by Aleksandr Shabelnikov on 30.08.2025.
//

#ifndef ASOHCILinkAPI_h
#define ASOHCILinkAPI_h

#include <DriverKit/IOReturn.h>
#include <DriverKit/OSObject.h>
#include <DriverKit/OSSharedPtr.h>
#include <stdint.h>

// Forward declarations
class ASOHCI;

/**
 * @class ASOHCILinkAPI
 * @brief High-level FireWire Link API that abstracts hardware details
 *
 * This class provides a clean interface for FireWire operations without
 * exposing low-level PCI registers, DMA management, or DriverKit plumbing.
 * It's designed to be used by higher-level controllers like
 * ASFireWireController.
 */
class ASOHCILinkAPI : public OSObject {
  OSDeclareDefaultStructors(ASOHCILinkAPI);

public:
  /**
   * @brief Get the local controller's GUID
   * @return 64-bit GUID of the local FireWire controller
   */
  virtual uint64_t GetLocalGUID();

  /**
   * @brief Force a bus reset
   * @param forceIBR If true, force an immediate bus reset
   * @return kIOReturnSuccess on success, error code otherwise
   */
  virtual kern_return_t ResetBus(bool forceIBR = false);

  /**
   * @brief Get current Node ID
   * @return 16-bit Node ID (includes bus ID and node address)
   */
  virtual uint16_t GetNodeID();

  /**
   * @brief Get current bus generation
   * @return 32-bit generation counter
   */
  virtual uint32_t GetGeneration();

  /**
   * @brief Perform asynchronous read from a remote node
   * @param nodeID Target node ID
   * @param addrHi High 32 bits of 48-bit FireWire address
   * @param addrLo Low 32 bits of 48-bit FireWire address
   * @param length Number of bytes to read (must be multiple of 4 for quadlet
   * reads)
   * @param generation Bus generation for transaction
   * @param speed Transfer speed (0=S100, 1=S200, 2=S400, etc.)
   * @return kIOReturnSuccess on success, error code otherwise
   */
  virtual kern_return_t AsyncRead(uint16_t nodeID, uint32_t addrHi,
                                  uint32_t addrLo, uint32_t length,
                                  uint32_t generation, uint8_t speed);

  /**
   * @brief Perform asynchronous write to a remote node
   * @param nodeID Target node ID
   * @param addrHi High 32 bits of 48-bit FireWire address
   * @param addrLo Low 32 bits of 48-bit FireWire address
   * @param data Pointer to data buffer
   * @param length Number of bytes to write (must be multiple of 4 for quadlet
   * writes)
   * @param generation Bus generation for transaction
   * @param speed Transfer speed (0=S100, 1=S200, 2=S400, etc.)
   * @return kIOReturnSuccess on success, error code otherwise
   */
  virtual kern_return_t AsyncWrite(uint16_t nodeID, uint32_t addrHi,
                                   uint32_t addrLo, const void *data,
                                   uint32_t length, uint32_t generation,
                                   uint8_t speed);

  /**
   * @brief Check if the local node is the root of the bus
   * @return true if this node is the bus root
   */
  virtual bool IsRoot();

  /**
   * @brief Get the number of nodes on the bus
   * @return Number of nodes currently on the bus
   */
  virtual uint8_t GetNodeCount();

  /**
   * @brief Set callback for Self-ID completion events
   * @param callback Function to call when Self-ID completes
   * @param context User context passed to callback
   */
  virtual void SetSelfIDCallback(void (*callback)(void *context),
                                 void *context);

  /**
   * @brief Set callback for bus reset events
   * @param callback Function to call when bus reset occurs
   * @param context User context passed to callback
   */
  virtual void SetBusResetCallback(void (*callback)(void *context),
                                   void *context);

protected:
  ASOHCI *fASOHCI; // Reference to the owning ASOHCI instance
};

#endif /* ASOHCILinkAPI_h */