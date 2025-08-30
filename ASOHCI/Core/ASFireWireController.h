//
//  ASFireWireController.h
//  ASFireWire
//
//  Created by Aleksandr Shabelnikov on 30.08.2025.
//

#ifndef ASFireWireController_h
#define ASFireWireController_h

#include <DriverKit/IOReturn.h>
#include <DriverKit/OSObject.h>
#include <DriverKit/OSSharedPtr.h>
#include <stdint.h>

// Forward declaration
// class ASOHCILinkAPI;

/**
 * @class ASFireWireController
 * @brief High-level FireWire bus controller that uses the Link API
 *
 * This controller manages bus discovery, device enumeration, and
 * high-level FireWire operations without dealing with low-level
 * hardware details.
 */
class ASFireWireController {

public:
  /**
   * @brief Create a new controller with the given Link API
   * @param linkAPI The Link API instance to use
   * @return New controller instance or nullptr on failure
   */
  // static ASFireWireController *Create(OSSharedPtr<ASOHCILinkAPI> linkAPI);

  /**
   * @brief Initialize the controller
   * @return true on success
   */
  bool init();

  /**
   * @brief Start bus discovery and enumeration
   */
  void StartDiscovery();

  /**
   * @brief Stop all operations
   */
  void Stop();

private:
  /**
   * @brief Constructor
   * @param linkAPI The Link API to use
   */
  // explicit ASFireWireController(OSSharedPtr<ASOHCILinkAPI> linkAPI);

  /**
   * @brief Callback for Self-ID completion events
   * @param context The controller instance
   */
  static void SelfIDCallback(void *context);

  /**
   * @brief Callback for bus reset events
   * @param context The controller instance
   */
  static void BusResetCallback(void *context);

  /**
   * @brief Handle Self-ID completion
   */
  void HandleSelfIDComplete();

  /**
   * @brief Handle bus reset
   */
  void HandleBusReset();

  /**
   * @brief Read Config ROM from a node
   * @param nodeID The node to read from
   */
  void ReadConfigROM(uint16_t nodeID);

  // Link API instance
  // OSSharedPtr<ASOHCILinkAPI> fLinkAPI;

  // Controller state
  bool fDiscoveryInProgress;
  uint32_t fCurrentGeneration;
};

#endif /* ASFireWireController_h */