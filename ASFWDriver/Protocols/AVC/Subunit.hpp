//
// Subunit.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Abstract base class for AV/C subunits
//

#pragma once

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/OSObject.h>
#include <DriverKit/OSSharedPtr.h>
#endif
#include <vector>
#include <string>
#include <functional>
#include "AVCDefs.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Protocols::AVC {

class AVCUnit; // Forward declaration

/// Abstract base class for AV/C subunits
class Subunit {
public:
    virtual ~Subunit() = default;

    /// Get subunit type
    AVCSubunitType GetType() const { return type_; }

    /// Get subunit ID
    uint8_t GetID() const { return id_; }

    /// Get subunit address byte
    uint8_t GetAddress() const {
        return MakeSubunitAddress(type_, id_);
    }

    /// Get plug counts
    uint8_t GetNumDestPlugs() const { return numDestPlugs_; }
    uint8_t GetNumSrcPlugs() const { return numSrcPlugs_; }

    /// Set plug counts (called by AVCUnit after PLUG_INFO)
    void SetPlugCounts(uint8_t dest, uint8_t src) {
        numDestPlugs_ = dest;
        numSrcPlugs_ = src;
    }

    /// Parse capabilities (optional, override in subclasses)
    /// @param unit Pointer to parent AVCUnit (for sending commands)
    /// @param completion Callback when done
    virtual void ParseCapabilities(AVCUnit& unit, std::function<void(bool)> completion) {
        // Default implementation: do nothing, just succeed
        completion(true);
    }

    /// Get human-readable name
    virtual std::string GetName() const = 0;

protected:
    Subunit(AVCSubunitType type, uint8_t id)
        : type_(type), id_(id) {}

    AVCSubunitType type_;
    uint8_t id_;
    uint8_t numDestPlugs_{0};
    uint8_t numSrcPlugs_{0};
};

} // namespace ASFW::Protocols::AVC
