//
//  ConfigROMHandler.hpp
//  ASFWDriver
//
//  Handler for Config ROM related UserClient methods
//

#ifndef ASFW_USERCLIENT_CONFIG_ROM_HANDLER_HPP
#define ASFW_USERCLIENT_CONFIG_ROM_HANDLER_HPP

#include <DriverKit/IOUserClient.h>

// Forward declarations
class ASFWDriver;

namespace ASFW::UserClient {

class ConfigROMHandler {
public:
    explicit ConfigROMHandler(ASFWDriver* driver);
    ~ConfigROMHandler() = default;

    // Disable copy/move
    ConfigROMHandler(const ConfigROMHandler&) = delete;
    ConfigROMHandler& operator=(const ConfigROMHandler&) = delete;

    // Method 14: Export Config ROM for a given nodeId and generation
    kern_return_t ExportConfigROM(IOUserClientMethodArguments* args);

    // Method 15: Manually trigger ROM read for a specific nodeId
    kern_return_t TriggerROMRead(IOUserClientMethodArguments* args);

private:
    ASFWDriver* driver_;
};

} // namespace ASFW::UserClient

#endif // ASFW_USERCLIENT_CONFIG_ROM_HANDLER_HPP
