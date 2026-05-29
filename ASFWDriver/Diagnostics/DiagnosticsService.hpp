#ifndef ASFW_DIAGNOSTICS_SERVICE_HPP
#define ASFW_DIAGNOSTICS_SERVICE_HPP

#include <cstdint>
#include <memory>
#include "../Shared/ASFWDiagnosticsABI.h"

namespace ASFW::Driver {
class ControllerCore;
}

namespace ASFW::Diagnostics {

class DiagnosticsService {
public:
    explicit DiagnosticsService(Driver::ControllerCore* controller) noexcept;
    ~DiagnosticsService() = default;

    // Prevent copy/move
    DiagnosticsService(const DiagnosticsService&) = delete;
    DiagnosticsService& operator=(const DiagnosticsService&) = delete;

    // Direct collection methods returning status codes
    ASFWDiagStatus CollectBusContract(ASFWDiagBusContract* out) const noexcept;
    ASFWDiagStatus CollectTopology(ASFWDiagTopology* out) const noexcept;
    ASFWDiagStatus CollectRoleCoordinator(ASFWDiagRoleCoordinator* out) const noexcept;
    ASFWDiagStatus CollectOHCI(ASFWDiagOHCI* out) const noexcept;
    ASFWDiagStatus CollectPHY(ASFWDiagPHY* out) const noexcept;
    ASFWDiagStatus CollectCSRContract(ASFWDiagCSRContract* out) const noexcept;
    ASFWDiagStatus CollectAsyncTrace(ASFWDiagAsyncTrace* out) const noexcept;
    ASFWDiagStatus CollectInboundCSRStats(ASFWDiagInboundCSRStats* out) const noexcept;
    ASFWDiagStatus CollectBusManager(ASFWDiagBusManager* out) const noexcept;

private:
    Driver::ControllerCore* controller_{nullptr};
    mutable uint32_t snapshotSeq_{0};
};

} // namespace ASFW::Diagnostics

#endif // ASFW_DIAGNOSTICS_SERVICE_HPP
