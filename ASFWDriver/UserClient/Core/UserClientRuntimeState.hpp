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
#include "../Storage/TransactionStorage.hpp"

class ASFWDriver;

namespace ASFW::UserClient {

/**
 * @brief Typed bridge that owns UserClient handlers and transient transaction storage.
 *
 * DriverKit's IIG ivars force an opaque pointer field. This object keeps that
 * opacity confined to a single bridge while the rest of the UserClient logic
 * stays fully typed.
 */
class UserClientRuntimeState final {
  public:
    UserClientRuntimeState() = default;
    ~UserClientRuntimeState() = default;

    UserClientRuntimeState(const UserClientRuntimeState&) = delete;
    UserClientRuntimeState& operator=(const UserClientRuntimeState&) = delete;

    [[nodiscard]] bool IsValid() const noexcept { return transactionStorage_.IsValid(); }

    [[nodiscard]] bool BindDriver(ASFWDriver* driver) {
        ResetHandlers();
        if (driver == nullptr) {
            return false;
        }

        busResetHandler_ = std::make_unique<BusResetHandler>(driver);
        topologyHandler_ = std::make_unique<TopologyHandler>(driver);
        statusHandler_ = std::make_unique<StatusHandler>(driver);
        transactionHandler_ = std::make_unique<TransactionHandler>(driver, &transactionStorage_);
        configRomHandler_ = std::make_unique<ConfigROMHandler>(driver);
        deviceDiscoveryHandler_ = std::make_unique<DeviceDiscoveryHandler>(driver);

        auto* controllerCore = GetControllerCorePtr(driver);
        auto* avcDiscovery = controllerCore ? controllerCore->GetAVCDiscovery() : nullptr;
        auto* sbp2Manager = controllerCore ? controllerCore->GetSbp2AddressSpaceManager() : nullptr;
        avcHandler_ = std::make_unique<AVCHandler>(avcDiscovery);
        isochHandler_ = std::make_unique<IsochHandler>(driver);
        sbp2Handler_ = std::make_unique<SBP2Handler>(sbp2Manager);

        return HandlersReady();
    }

    void ResetHandlers() noexcept {
        sbp2Handler_.reset();
        isochHandler_.reset();
        avcHandler_.reset();
        deviceDiscoveryHandler_.reset();
        configRomHandler_.reset();
        transactionHandler_.reset();
        statusHandler_.reset();
        topologyHandler_.reset();
        busResetHandler_.reset();
    }

    void ReleaseOwner(void* owner) noexcept {
        if (owner != nullptr && sbp2Handler_ != nullptr) {
            sbp2Handler_->ReleaseOwner(owner);
        }
    }

    [[nodiscard]] bool HandlersReady() const noexcept {
        return busResetHandler_ != nullptr && topologyHandler_ != nullptr &&
               statusHandler_ != nullptr && transactionHandler_ != nullptr &&
               configRomHandler_ != nullptr && deviceDiscoveryHandler_ != nullptr &&
               avcHandler_ != nullptr && isochHandler_ != nullptr &&
               sbp2Handler_ != nullptr;
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

  private:
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
};

template <typename ClientLike>
[[nodiscard]] inline UserClientRuntimeState* GetRuntimeState(ClientLike* userClient) noexcept {
    if (userClient == nullptr || userClient->ivars == nullptr) {
        return nullptr;
    }
    return static_cast<UserClientRuntimeState*>(userClient->ivars->runtimeState);
}

} // namespace ASFW::UserClient
