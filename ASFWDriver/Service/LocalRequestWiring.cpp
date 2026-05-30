// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// LocalRequestWiring.cpp — see LocalRequestWiring.hpp. The single home for the
// per-protocol local-address handler adapters and their registration order.

#include "LocalRequestWiring.hpp"

#include "DriverContext.hpp"

#include "../Async/AsyncSubsystem.hpp"
#include "../Async/Rx/LocalRequestDispatch.hpp"
#include "../Async/Rx/PacketRouter.hpp"
#include "../Bus/CSR/CSRHardwareAdapters.hpp"
#include "../Bus/CSR/CSRResponder.hpp"
#include "../Bus/CSR/TopologyMapService.hpp"
#include "../Bus/BusManager/BusManagerElectionDriver.hpp"
#include "../Controller/ControllerCore.hpp"
#include "../Hardware/IEEE1394.hpp"
#include "../Logging/Logging.hpp"
#include "../Protocols/AVC/FCPResponseRouter.hpp"
#include "../Protocols/Audio/DICE/Core/DICENotificationMailbox.hpp"
#include "../Protocols/Ports/FireWireRxPort.hpp"
#include "../Protocols/SBP2/AddressSpaceManager.hpp"

#include <memory>

namespace ASFW::Service {

namespace {

using ASFW::Async::ILocalAddressHandler;
using ASFW::Async::LocalRequestContext;
using ASFW::Async::LocalRequestResult;
using ASFW::Async::ResponseCode;
using AReq = ASFW::Async::HW::AsyncRequestHeader;

// --- CSR: STATE_SET/CLEAR, RESET_START, BROADCAST_CHANNEL, TOPOLOGY_MAP --------
class CSRLocalHandler final : public ILocalAddressHandler {
public:
    explicit CSRLocalHandler(ASFW::Bus::CSRResponder* csr) noexcept : csr_(csr) {}
    [[nodiscard]] const char* Name() const noexcept override { return "CSR"; }

    [[nodiscard]] LocalRequestResult HandleLocalRequest(const LocalRequestContext& ctx) override {
        if (csr_ == nullptr) {
            return LocalRequestResult::NotMine();
        }
        // CSR register space only: destination high 16 bits == 0xFFFF.
        if ((ctx.destOffset >> 32) != 0xFFFFu) {
            return LocalRequestResult::NotMine();
        }
        const uint32_t off = static_cast<uint32_t>(ctx.destOffset & 0xFFFFFFFFu);
        if (off == ASFW::FW::kCSR_BusManagerID ||
            off == ASFW::FW::kCSR_BandwidthAvailable ||
            off == ASFW::FW::kCSR_ChannelsAvailableHi ||
            off == ASFW::FW::kCSR_ChannelsAvailableLo) {
            ASFW_LOG(Controller, "⚠️ WARNING: Inbound request targeting autonomous hardware CSR (offset=0x%x, tCode=0x%x) reached software dispatch unexpectedly!", off, ctx.tCode);
        }
        switch (ctx.tCode) {
        case AReq::kTcodeWriteQuad: {
            const auto r = csr_->WriteQuadlet(off, ctx.quadletData);
            return r.mine ? LocalRequestResult::Write(r.rcode) : LocalRequestResult::NotMine();
        }
        case AReq::kTcodeReadQuad: {
            const auto r = csr_->ReadQuadlet(off);
            return r.mine ? LocalRequestResult::Quadlet(r.rcode, r.readValue)
                          : LocalRequestResult::NotMine();
        }
        case AReq::kTcodeReadBlock: {
            const auto r = csr_->BlockReadClaim(off, ctx.dataLength);
            if (!r.mine) {
                return LocalRequestResult::NotMine();
            }
            return LocalRequestResult::Block(r.rcode, r.readBlockDeviceAddress, r.readBlockLength);
        }
        default:
            return LocalRequestResult::NotMine();
        }
    }

private:
    ASFW::Bus::CSRResponder* csr_;
};

// --- FCP: AV/C command/response block writes -----------------------------------
class FcpLocalHandler final : public ILocalAddressHandler {
public:
    explicit FcpLocalHandler(ASFW::Protocols::AVC::FCPResponseRouter* fcp) noexcept : fcp_(fcp) {}
    [[nodiscard]] const char* Name() const noexcept override { return "FCP"; }

    [[nodiscard]] LocalRequestResult HandleLocalRequest(const LocalRequestContext& ctx) override {
        if (fcp_ == nullptr || ctx.tCode != AReq::kTcodeWriteBlock || ctx.writePayload.empty()) {
            return LocalRequestResult::NotMine();
        }
        const ASFW::Protocols::Ports::BlockWriteRequestView request{
            .sourceID = ctx.sourceID,
            .destOffset = ctx.destOffset,
            .payload = ctx.writePayload,
        };
        const auto disposition = fcp_->RouteBlockWrite(request);
        if (disposition == ASFW::Protocols::Ports::BlockWriteDisposition::kAddressError) {
            return LocalRequestResult::NotMine();
        }
        return LocalRequestResult::Write(ResponseCode::Complete);
    }

private:
    ASFW::Protocols::AVC::FCPResponseRouter* fcp_;
};

// --- DICE: notification mailbox quadlet writes ---------------------------------
class DiceLocalHandler final : public ILocalAddressHandler {
public:
    [[nodiscard]] const char* Name() const noexcept override { return "DICE"; }

    [[nodiscard]] LocalRequestResult HandleLocalRequest(const LocalRequestContext& ctx) override {
        if (ctx.tCode != AReq::kTcodeWriteQuad) {
            return LocalRequestResult::NotMine();
        }
        if (!ASFW::Audio::DICE::NotificationMailbox::MatchesDestOffset(ctx.destOffset)) {
            return LocalRequestResult::NotMine();
        }
        if (ctx.writePayload.size() < 4) {
            return LocalRequestResult::Write(ResponseCode::TypeError);
        }
        const uint32_t bits =
            ASFW::Audio::DICE::NotificationMailbox::PublishWireQuadlet(ctx.writePayload.data());
        ASFW_LOG(DICE, "DICE notification quadlet: dest=0x%010llx bits=0x%08x",
                 static_cast<unsigned long long>(ctx.destOffset), bits);
        return LocalRequestResult::Write(ResponseCode::Complete);
    }
};

// --- SBP-2: dynamically allocated device address ranges ------------------------
class Sbp2LocalHandler final : public ILocalAddressHandler {
public:
    explicit Sbp2LocalHandler(ASFW::Protocols::SBP2::AddressSpaceManager* mgr) noexcept
        : mgr_(mgr) {}
    [[nodiscard]] const char* Name() const noexcept override { return "SBP2"; }

    [[nodiscard]] LocalRequestResult HandleLocalRequest(const LocalRequestContext& ctx) override {
        if (mgr_ == nullptr) {
            return LocalRequestResult::NotMine();
        }
        switch (ctx.tCode) {
        case AReq::kTcodeWriteQuad:
        case AReq::kTcodeWriteBlock: {
            if (ctx.writePayload.empty()) {
                return LocalRequestResult::NotMine();
            }
            const auto rc = mgr_->ApplyRemoteWrite(ctx.destOffset, ctx.writePayload);
            return rc == ResponseCode::AddressError ? LocalRequestResult::NotMine()
                                                    : LocalRequestResult::Write(rc);
        }
        case AReq::kTcodeReadQuad: {
            uint32_t value = 0;
            const auto rc = mgr_->ReadQuadlet(ctx.destOffset, &value);
            return rc == ResponseCode::AddressError ? LocalRequestResult::NotMine()
                                                    : LocalRequestResult::Quadlet(rc, value);
        }
        case AReq::kTcodeReadBlock: {
            if (ctx.dataLength == 0) {
                return LocalRequestResult::NotMine();
            }
            ASFW::Protocols::SBP2::AddressSpaceManager::ReadSlice slice{};
            const auto rc = mgr_->ResolveReadSlice(ctx.destOffset, ctx.dataLength, &slice);
            if (rc == ResponseCode::AddressError) {
                return LocalRequestResult::NotMine();
            }
            if (rc == ResponseCode::Complete) {
                return LocalRequestResult::Block(rc, slice.payloadDeviceAddress, slice.payloadLength);
            }
            return LocalRequestResult{.claimed = true, .rcode = rc};
        }
        default:
            return LocalRequestResult::NotMine();
        }
    }

private:
    ASFW::Protocols::SBP2::AddressSpaceManager* mgr_;
};

} // namespace

void WireLocalRequestDispatch(::ServiceContext& ctx) {
    auto& d = ctx.deps;
    if (!d.asyncSubsystem) {
        return;
    }
    auto* router = d.asyncSubsystem->GetPacketRouter();
    if (router == nullptr) {
        return;
    }

    // Create the CSR responder + hardware adapters once.
    if (!d.csrResponder && d.hardware) {
        d.csrRootStatus = std::make_shared<ASFW::Bus::HardwareRootStatus>(d.hardware.get());
        d.csrCycleMasterControl =
            std::make_shared<ASFW::Bus::HardwareCycleMasterControl>(d.hardware.get());
        d.csrResetTrigger =
            std::make_shared<ASFW::Bus::HardwareBusResetTrigger>(d.hardware.get());
        d.csrResponder = std::make_shared<ASFW::Bus::CSRResponder>(ASFW::Bus::CSRResponder::Deps{
            .root = d.csrRootStatus.get(),
            .cycleMaster = d.csrCycleMasterControl.get(),
            .resetTrigger = d.csrResetTrigger.get(),
            .topologyMap = d.topologyMapService.get(),
        });
        ASFW_LOG(Controller, "[Controller] CSRResponder initialized with TopologyMapService (FW-20)");

        // Create and wire the Bus Manager election driver (FW-18)
        if (!d.busManagerElectionDriver && d.asyncController && d.scheduler) {
            ASFW::Bus::BusManagerElectionDriver::Deps electDeps{
                .asyncController = d.asyncController.get(),
                .scheduler = d.scheduler.get(),
                .csrResponder = d.csrResponder.get(),
                .hardware = d.hardware.get(),
                .localIrmController = ctx.controller ? ctx.controller->GetLocalIRMResourceController() : nullptr,
            };
            d.busManagerElectionDriver = std::make_shared<ASFW::Bus::BusManagerElectionDriver>(electDeps, ctx.rolePolicy.roleMode);
            if (ctx.controller) {
                d.busManagerElectionDriver->SetObserver(ctx.controller.get());
                ctx.controller->SetBusManagerElectionDriver(d.busManagerElectionDriver);
            }
            ASFW_LOG(Controller, "[Controller] BusManagerElectionDriver initialized (FW-18)");
        }
    }

    auto dispatch = std::make_shared<ASFW::Async::LocalRequestDispatch>();
    // Priority order: specific CSR offsets first, then FCP / DICE fixed regions,
    // then SBP-2's dynamically allocated ranges. Ranges do not overlap; each
    // handler also self-filters and declines (NotMine) addresses it does not own.
    dispatch->AddHandler(std::make_unique<CSRLocalHandler>(d.csrResponder.get()));
    if (d.fcpResponseRouter) {
        dispatch->AddHandler(std::make_unique<FcpLocalHandler>(d.fcpResponseRouter.get()));
    }
    dispatch->AddHandler(std::make_unique<DiceLocalHandler>());
    if (d.sbp2AddressSpaceManager) {
        dispatch->AddHandler(std::make_unique<Sbp2LocalHandler>(d.sbp2AddressSpaceManager.get()));
    }

    dispatch->Install(*router, router->GetResponseSender());
    d.localRequestDispatch = dispatch;

    ASFW_LOG(Controller,
             "✅ LocalRequestDispatch wired: %zu handlers (CSR/FCP/DICE/SBP2), tCodes 0x0/0x1/0x4/0x5",
             dispatch->HandlerCount());
}

} // namespace ASFW::Service
