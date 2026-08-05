// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProtocol.cpp - Apogee Duet composition point.
//
// The duplex lifecycle and clock-transition FSM moved to ApogeeDuetDuplex
// (FW-127); what remains is construction plus the Duet's control surface -
// parameters, meters and the Oxford ID registers - each delegating to the piece
// that owns it.

#include "ApogeeDuetProtocol.hpp"

#include "ApogeeParamsSerdes.hpp"
#include "ApogeeTransport.hpp"

#include "../../../../Common/CallbackUtils.hpp"
#include "../../../../Protocols/AVC/CMP/CMPClient.hpp"

#include <memory>
#include <vector>

namespace ASFW::Audio::Oxford::Apogee {


ApogeeDuetProtocol::ApogeeDuetProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                       Protocols::Ports::FireWireBusInfo& busInfo,
                                       Discovery::DeviceRouteToken route,
                                       Protocols::AVC::FCPTransport* fcpTransport,
                                       IRM::IRMClient* irmClient,
                                       CMP::CMPClient* cmpClient,
                                       uint32_t formatSettleDelayMs,
                                       Scheduling::ITimerScheduler* timerScheduler)
    : runtime_{
          .busOps = busOps,
          .busInfo = busInfo,
          .route = route,
          .fcpTransport = fcpTransport,
          .irmClient = irmClient,
          .cmpClient = cmpClient,
          .timerScheduler = timerScheduler,
          .formatSettleDelayMs = formatSettleDelayMs,
      }
    , duplex_(runtime_) {
}

IOReturn ApogeeDuetProtocol::Initialize() {
    return kIOReturnSuccess;
}

IOReturn ApogeeDuetProtocol::Shutdown() {
    duplex_.Shutdown();
    return kIOReturnSuccess;
}


// Dispatch lives in ApogeeTransport (FW-129); these forward the protocol's
// current transport into it.
void ApogeeDuetProtocol::SendVendorCommand(const VendorCommand& command,
                                           bool isStatus,
                                           VendorResultCallback callback) {
    VendorFcp::Send(runtime_.fcpTransport, command, isStatus, std::move(callback));
}

void ApogeeDuetProtocol::ExecuteVendorSequence(const std::vector<VendorCommand>& commands,
                                               bool isStatus,
                                               VendorSequenceCallback callback) {
    VendorFcp::ExecuteSequence(runtime_.fcpTransport, commands, isStatus, std::move(callback));
}

void ApogeeDuetProtocol::GetKnobState(ResultCallback<KnobState> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildKnobStateQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess || responses.empty()) {
                Common::InvokeSharedCallback(callbackState,
                                             status != kIOReturnSuccess ? status : kIOReturnError,
                                             KnobState{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseKnobState(responses[0]));
        });
}

void ApogeeDuetProtocol::SetKnobState(const KnobState& state, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        {ParamsSerdes::BuildKnobStateControl(state)},
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetOutputParams(ResultCallback<OutputParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildOutputParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, OutputParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseOutputParams(responses));
        });
}

void ApogeeDuetProtocol::SetOutputParams(const OutputParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildOutputParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetInputParams(ResultCallback<InputParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildInputParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, InputParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseInputParams(responses));
        });
}

void ApogeeDuetProtocol::SetInputParams(const InputParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildInputParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetMixerParams(ResultCallback<MixerParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildMixerParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, MixerParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseMixerParams(responses));
        });
}

void ApogeeDuetProtocol::SetMixerParams(const MixerParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildMixerParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetDisplayParams(ResultCallback<DisplayParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildDisplayParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, DisplayParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseDisplayParams(responses));
        });
}

void ApogeeDuetProtocol::SetDisplayParams(const DisplayParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildDisplayParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::ClearDisplay(VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        {VendorCommand::Make(VendorCommand::Code::DisplayClear)},
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetInputMeter(ResultCallback<InputMeterState> callback) {
    MeterRegisters::ReadInput(runtime_.busOps, runtime_.route, MakeRouteValidator(), std::move(callback));
}

void ApogeeDuetProtocol::GetMixerMeter(ResultCallback<MixerMeterState> callback) {
    MeterRegisters::ReadMixer(runtime_.busOps, runtime_.route, MakeRouteValidator(), std::move(callback));
}

// The Oxford ASIC registers are chip-common, not Duet-specific (FW-137), so
// the register map and decode live in Oxford/OxfordCsr. What stays here is the
// Duet's own route-liveness policy, expressed as the validator that layer takes.
Oxford::RouteValidator ApogeeDuetProtocol::MakeRouteValidator() const {
    return [this, route = runtime_.route] {
        return runtime_.cmpClient != nullptr && runtime_.cmpClient->IsRouteCurrent(route);
    };
}

void ApogeeDuetProtocol::GetFirmwareId(ResultCallback<uint32_t> callback) {
    Oxford::ReadFirmwareId(runtime_.busOps, runtime_.route, MakeRouteValidator(), std::move(callback));
}

void ApogeeDuetProtocol::GetHardwareId(ResultCallback<uint32_t> callback) {
    Oxford::ReadHardwareId(runtime_.busOps, runtime_.route, MakeRouteValidator(), std::move(callback));
}

} // namespace ASFW::Audio::Oxford::Apogee
