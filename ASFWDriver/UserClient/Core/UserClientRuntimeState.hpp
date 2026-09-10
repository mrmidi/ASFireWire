#pragma once

#include <memory>

#include "../../Controller/ControllerCore.hpp"
#include "../Handlers/AVCHandler.hpp"
#include "../Handlers/BusResetHandler.hpp"
#include "../Handlers/ConfigROMHandler.hpp"
#include "../Handlers/ControllerCoreAccess.hpp"
#include "../Handlers/DeviceDiscoveryHandler.hpp"
#include "../Handlers/IsochHandler.hpp"
#include "../Handlers/SBP2Handler.hpp"
#include "../Handlers/StatusHandler.hpp"
#include "../Handlers/TopologyHandler.hpp"
#include "../Handlers/TransactionHandler.hpp"
#include "../Handlers/DiagnosticsHandler.hpp"
#include "../Storage/TransactionStorage.hpp"

class ASFWDriver;

namespace ASFW::UserClient {

/**
 * @brief Typed bridge that owns UserClient handlers and transient transaction storage.
 *
 * The generated UserClient ivars hold an opaque pointer to this bridge because
 * IIG cannot model plain project C++ class pointers directly.
 */
class UserClientRuntimeState final {
  public:
    UserClientRuntimeState() = default;
    ~UserClientRuntimeState() = default;

    UserClientRuntimeState(const UserClientRuntimeState&) = delete;
    UserClientRuntimeState& operator=(const UserClientRuntimeState&) = delete;

    [[nodiscard]] bool IsValid() const noexcept { return transactionStorage_.IsValid(); }

    [[nodiscard]] bool BindDriver(ASFWDriver* driver, void* owner) {
        ResetHandlers();
        if (driver == nullptr) {
            return false;
        }
        driver_ = driver;

        busResetHandler_ = std::make_unique<BusResetHandler>(driver);
        topologyHandler_ = std::make_unique<TopologyHandler>(driver);
        statusHandler_ = std::make_unique<StatusHandler>(driver);
        transactionHandler_ = std::make_unique<TransactionHandler>(driver, &transactionStorage_);
        configRomHandler_ = std::make_unique<ConfigROMHandler>(driver);
        deviceDiscoveryHandler_ = std::make_unique<DeviceDiscoveryHandler>(driver);

        auto* controllerCore = GetControllerCorePtr(driver);
        auto* avcDiscovery = controllerCore ? controllerCore->GetAVCDiscovery() : nullptr;
        auto* sbp2Manager = controllerCore ? controllerCore->GetSbp2AddressSpaceManager() : nullptr;
        auto* sbp2Registry = controllerCore ? controllerCore->GetSbp2SessionRegistry() : nullptr;
        avcHandler_ = std::make_unique<AVCHandler>(avcDiscovery);
        isochHandler_ = std::make_unique<IsochHandler>(
            driver, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owner)));
        sbp2Handler_ = std::make_unique<SBP2Handler>(sbp2Manager, sbp2Registry);
        diagnosticsHandler_ = std::make_unique<DiagnosticsHandler>(driver);

        return HandlersReady();
    }

    void ResetHandlers() noexcept {
        if (isochHandler_ != nullptr) {
            isochHandler_->ReleaseOwner();
        }
        sbp2Handler_.reset();
        isochHandler_.reset();
        avcHandler_.reset();
        deviceDiscoveryHandler_.reset();
        configRomHandler_.reset();
        transactionHandler_.reset();
        statusHandler_.reset();
        topologyHandler_.reset();
        busResetHandler_.reset();
        diagnosticsHandler_.reset();
        driver_ = nullptr;
    }

    void ReleaseOwner(void* owner) noexcept {
        if (isochHandler_ != nullptr) {
            isochHandler_->ReleaseOwner();
        }
        // SBP2Handler snapshots raw AddressSpaceManager/SessionRegistry pointers at bind
        // time -- unlike its sibling handlers, which re-resolve through
        // GetControllerCorePtr() on every call. ServiceContext::Reset() destroys those
        // objects during quiesce, and quiesce runs on the driver's work queue, so it can
        // complete while this user client is still stopping on ASFWDriverUserClient-Default.
        // Releasing sessions then walks a freed std::map: a zeroed __begin_node_ makes
        // begin() null while end() still points inside the object, so the very first
        // iteration dereferences null at SessionRecord::owner (node+0x30). This is the
        // FW-60 cross-queue teardown class. Reset() drops `controller` alongside the
        // registry, so a live core is the validity signal for both raw pointers.
        if (owner != nullptr && sbp2Handler_ != nullptr &&
            GetControllerCorePtr(driver_) != nullptr) {
            sbp2Handler_->ReleaseOwner(owner);
        }
    }

    [[nodiscard]] bool HandlersReady() const noexcept {
        return busResetHandler_ != nullptr && topologyHandler_ != nullptr &&
               statusHandler_ != nullptr && transactionHandler_ != nullptr &&
               configRomHandler_ != nullptr && deviceDiscoveryHandler_ != nullptr &&
               avcHandler_ != nullptr && isochHandler_ != nullptr &&
               sbp2Handler_ != nullptr && diagnosticsHandler_ != nullptr;
    }

    [[nodiscard]] TransactionStorage& TransactionResults() noexcept { return transactionStorage_; }

    [[nodiscard]] BusResetHandler& BusReset() noexcept { return *busResetHandler_; }
    [[nodiscard]] TopologyHandler& Topology() noexcept { return *topologyHandler_; }
    [[nodiscard]] StatusHandler& Status() noexcept { return *statusHandler_; }
    [[nodiscard]] TransactionHandler& Transactions() noexcept { return *transactionHandler_; }
    [[nodiscard]] ConfigROMHandler& ConfigROM() noexcept { return *configRomHandler_; }
    [[nodiscard]] DeviceDiscoveryHandler& DeviceDiscovery() noexcept {
        return *deviceDiscoveryHandler_;
    }
    [[nodiscard]] AVCHandler& AVC() noexcept { return *avcHandler_; }
    [[nodiscard]] IsochHandler& Isoch() noexcept { return *isochHandler_; }
    [[nodiscard]] SBP2Handler& SBP2() noexcept { return *sbp2Handler_; }
    [[nodiscard]] DiagnosticsHandler& Diagnostics() noexcept { return *diagnosticsHandler_; }

  private:
    /// Provider that owns every handler's driver-side state. Non-owning: IOKit keeps the
    /// provider alive across its clients' Stop, and BindDriver/ResetHandlers bracket it.
    ASFWDriver* driver_{nullptr};
    TransactionStorage transactionStorage_{};
    std::unique_ptr<BusResetHandler> busResetHandler_{};
    std::unique_ptr<TopologyHandler> topologyHandler_{};
    std::unique_ptr<StatusHandler> statusHandler_{};
    std::unique_ptr<TransactionHandler> transactionHandler_{};
    std::unique_ptr<ConfigROMHandler> configRomHandler_{};
    std::unique_ptr<DeviceDiscoveryHandler> deviceDiscoveryHandler_{};
    std::unique_ptr<AVCHandler> avcHandler_{};
    std::unique_ptr<IsochHandler> isochHandler_{};
    std::unique_ptr<SBP2Handler> sbp2Handler_{};
    std::unique_ptr<DiagnosticsHandler> diagnosticsHandler_{};
};

template <typename ClientLike>
[[nodiscard]] inline UserClientRuntimeState* GetRuntimeState(ClientLike* userClient) noexcept {
    if (userClient == nullptr || userClient->ivars == nullptr) {
        return nullptr;
    }
    return static_cast<UserClientRuntimeState*>(userClient->ivars->runtimeState);
}

} // namespace ASFW::UserClient
