//
// CameraSubunit.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Camera Subunit implementation
//

#pragma once

#include "../Subunit.hpp"

namespace ASFW::Protocols::AVC::Camera {

class CameraSubunit : public Subunit {
public:
    CameraSubunit(AVCSubunitType type, uint8_t id);
    virtual ~CameraSubunit() = default;

    /// Parse capabilities
    void ParseCapabilities(AVCUnit& unit, std::function<void(bool)> completion) override;

    /// Get human-readable name
    std::string GetName() const override { return "Camera"; }

private:
    // Camera-specific state
};

} // namespace ASFW::Protocols::AVC::Camera
