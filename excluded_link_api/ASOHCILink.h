//
//  ASOHCILink.h
//  ASOHCI
//
//  Created by Aleksandr Shabelnikov on 30.08.2025.
//

#ifndef ASOHCILink_h
#define ASOHCILink_h

#include "ASOHCILinkAPI.h"
#include <stdint.h>

// Forward declaration
class ASOHCI;

// Include OSObject for OSSharedPtr
#include <DriverKit/OSObject.h>
#include <DriverKit/OSSharedPtr.h>

/**
 * @class ASOHCILink
 * @brief Concrete implementation of ASOHCILinkAPI for ASOHCI
 *
 * This class provides the Link API implementation that delegates
 * to the ASOHCI hardware controller.
 */
class ASOHCILink : public ASOHCILinkAPI {
public:
  /**
   * @brief Factory method to create ASOHCILink instance
   * @param owner The ASOHCI instance that owns this Link API
   * @return Pointer to new ASOHCILink instance or nullptr on failure
   */
  static ASOHCILink *Create(ASOHCI *owner);

  /**
   * @brief Constructor
   * @param owner The ASOHCI instance that owns this Link API
   */
  explicit ASOHCILink(ASOHCI *owner);

  /**
   * @brief Destructor
   */
  virtual ~ASOHCILink();

  // ASOHCILinkAPI implementation
  virtual uint64_t GetLocalGUID() override;
  virtual kern_return_t ResetBus(bool forceIBR = false) override;
  virtual uint16_t GetNodeID() override;
  virtual uint32_t GetGeneration() override;
  virtual kern_return_t AsyncRead(uint16_t nodeID, uint32_t addrHi,
                                  uint32_t addrLo, uint32_t length,
                                  uint32_t generation, uint8_t speed) override;
  virtual kern_return_t AsyncWrite(uint16_t nodeID, uint32_t addrHi,
                                   uint32_t addrLo, const void *data,
                                   uint32_t length, uint32_t generation,
                                   uint8_t speed) override;
  virtual bool IsRoot() override;
  virtual uint8_t GetNodeCount() override;
  virtual void SetSelfIDCallback(void (*callback)(void *context),
                                 void *context) override;
  virtual void SetBusResetCallback(void (*callback)(void *context),
                                   void *context) override;

private:
  // fASOHCI is inherited from ASOHCILinkAPI
};

#endif /* ASOHCILink_h */