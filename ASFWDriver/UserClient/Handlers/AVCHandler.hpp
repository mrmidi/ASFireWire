//
//  AVCHandler.hpp
//  ASFWDriver
//
//  Handler for AV/C Protocol API
//

#pragma once

#include <DriverKit/IOLib.h>
#include <DriverKit/OSArray.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSNumber.h>
#include <memory>

class ASFWDriver;
struct IOUserClientMethodArguments;

namespace ASFW::UserClient {

/**
 * @brief Handler for AV/C protocol queries
 *
 * Provides GUI access to discovered AV/C units and their subunits.
 * Serializes AV/C unit information from AVCDiscovery into wire format.
 */
class AVCHandler {
public:
    explicit AVCHandler(ASFWDriver* driver);
    ~AVCHandler() = default;

    /**
     * @brief Get array of all discovered AV/C units
     *
     * Returns serialized AV/C unit data through IOUserClientMethodArguments.
     *
     * @param args IOUserClientMethodArguments with structureOutput
     * @return kIOReturnSuccess on success, error code otherwise
     */
    kern_return_t GetAVCUnits(IOUserClientMethodArguments* args);

private:
    ASFWDriver* driver_;
};

} // namespace ASFW::UserClient
