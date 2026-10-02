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
#include "../../Common/Lifetime.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Protocols::AVC {

/// Abstract base class for AV/C subunits
class Subunit {
public:
    virtual ~Subunit() = default;

    /// Expires when this subunit is destroyed; see IAvcUnit::LifetimeToken.
    [[nodiscard]] std::weak_ptr<const void> LifetimeToken() const noexcept { return lifetime_.Token(); }

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

    struct PlugCounts {
        uint8_t dest{0};
        uint8_t src{0};
    };

    /// Set plug counts (called by AVCUnit after PLUG_INFO)
    void SetPlugCounts(PlugCounts counts) {
        numDestPlugs_ = counts.dest;
        numSrcPlugs_ = counts.src;
    }

    /// Get human-readable name
    virtual std::string GetName() const = 0;

protected:
    Subunit(AVCSubunitType type, uint8_t id)
        : type_(type), id_(id) {}

    AVCSubunitType type_;
    uint8_t id_;
    Common::LifetimeAnchor lifetime_;
    uint8_t numDestPlugs_{0};
    uint8_t numSrcPlugs_{0};
};

} // namespace ASFW::Protocols::AVC
