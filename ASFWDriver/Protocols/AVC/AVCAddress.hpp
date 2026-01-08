//
// AVCAddress.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Helper class for AV/C addressing (Unit vs Subunit, Plug ID)
//

#pragma once

#include "AVCDefs.hpp"

namespace ASFW::Protocols::AVC {

class AVCAddress {
public:
    /// Create address for a Unit Plug
    static AVCAddress UnitPlugAddress(PlugType dir, uint8_t plugId) {
        return AVCAddress(true, 0xFF, dir, plugId);
    }

    /// Create address for a Subunit Plug
    static AVCAddress SubunitPlugAddress(uint8_t subunitId, PlugType dir, uint8_t plugId) {
        return AVCAddress(false, subunitId, dir, plugId);
    }

    bool IsUnitAddress() const { return isUnit_; }
    uint8_t GetSubunitID() const { return subunitId_; }
    PlugType GetDirection() const { return dir_; }
    uint8_t GetPlugID() const { return plugId_; }

private:
    AVCAddress(bool isUnit, uint8_t subunitId, PlugType dir, uint8_t plugId)
        : isUnit_(isUnit), subunitId_(subunitId), dir_(dir), plugId_(plugId) {}

    bool isUnit_;
    uint8_t subunitId_;
    PlugType dir_;
    uint8_t plugId_;
};

} // namespace ASFW::Protocols::AVC
