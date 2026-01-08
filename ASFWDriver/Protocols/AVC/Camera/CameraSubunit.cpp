//
// CameraSubunit.cpp
// ASFWDriver - AV/C Protocol Layer
//
// Camera Subunit implementation
//

#include "CameraSubunit.hpp"
#include "../AVCUnit.hpp"
#include "../../../Logging/Logging.hpp"

namespace ASFW::Protocols::AVC::Camera {

CameraSubunit::CameraSubunit(AVCSubunitType type, uint8_t id)
    : Subunit(type, id) {
    ASFW_LOG_DEBUG(Discovery, "CameraSubunit created: type=0x%02x id=%d",
                   static_cast<uint8_t>(type), id);
}

void CameraSubunit::ParseCapabilities(AVCUnit& unit, std::function<void(bool)> completion) {
    ASFW_LOG_INFO(Discovery, "CameraSubunit: Parsing capabilities...");

    // TODO: Implement Camera-specific capability parsing
    // For now, just succeed
    
    completion(true);
}

} // namespace ASFW::Protocols::AVC::Camera
