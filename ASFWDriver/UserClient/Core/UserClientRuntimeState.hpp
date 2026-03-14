#pragma once

#include <memory>

#include "../../Controller/ControllerCore.hpp"
#include "../Handlers/AVCHandler.hpp"
#include "../Handlers/BusResetHandler.hpp"
#include "../Handlers/ConfigROMHandler.hpp"
#include "../Handlers/ControllerCoreAccess.hpp"
#include "../Handlers/DeviceDiscoveryHandler.hpp"
#include "../Handlers/IsochHandler.hpp"
#include "../Handlers/StatusHandler.hpp"
#include "../Handlers/TopologyHandler.hpp"
#include "../Handlers/TransactionHandler.hpp"
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
        avcHandler_ = std::make_unique<AVCHandler>(avcDiscovery);
        isochHandler_ = std::make_unique<IsochHandler>(driver);

        return HandlersReady();
    }

    void ResetHandlers() noexcept {
        isochHandler_.reset();
        avcHandler_.reset();
        deviceDiscoveryHandler_.reset();
        configRomHandler_.reset();
        transactionHandler_.reset();
        statusHandler_.reset();
        topologyHandler_.reset();
        busResetHandler_.reset();
    }

    [[nodiscard]] bool HandlersReady() const noexcept {
        return busResetHandler_ != nullptr && topologyHandler_ != nullptr &&
               statusHandler_ != nullptr && transactionHandler_ != nullptr &&
               configRomHandler_ != nullptr && deviceDiscoveryHandler_ != nullptr &&
               avcHandler_ != nullptr && isochHandler_ != nullptr;
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
};

template <typename ClientLike>
[[nodiscard]] inline UserClientRuntimeState* GetRuntimeState(ClientLike* userClient) noexcept {
    if (userClient == nullptr || userClient->ivars == nullptr) {
        return nullptr;
    }
    return static_cast<UserClientRuntimeState*>(userClient->ivars->runtimeState);
}

} // namespace ASFW::UserClient
