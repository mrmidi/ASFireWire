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
#include "../../../../Logging/Logging.hpp"
#include "../../../../Protocols/AVC/CMP/CMPClient.hpp"

#include <DriverKit/IOLib.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <memory>
#include <vector>

namespace ASFW::Audio::Oxford::Apogee {

struct ApogeeDuetProtocol::SemanticControlState final {
    IOLock* lock{IOLockAlloc()};
    uint64_t epoch{1};
    uint32_t revision{0};
    bool valid{false};
    bool writeInFlight{false};
    OutputParams output{};
    InputParams input{};
    MixerParams mixer{};
    bool micsGrouped{false};
    KnobState knob{};

    ~SemanticControlState() {
        if (lock) IOLockFree(lock);
    }
};

struct ApogeeDuetProtocol::MeteringState final {
    IOLock* lock{IOLockAlloc()};
    std::array<int16_t, 6> values{};
    uint32_t revision{0};
    uint64_t epoch{1};
    Scheduling::TimerToken timer{Scheduling::kInvalidTimerToken};
    bool enabled{false};
    bool readInFlight{false};
    uint64_t readEpoch{0};

    ~MeteringState() {
        if (lock) IOLockFree(lock);
    }
};

namespace {

constexpr uint32_t kInputGain1 = 1;
constexpr uint32_t kInputGain2 = 2;
constexpr uint32_t kPhantom1 = 3;
constexpr uint32_t kPhantom2 = 4;
constexpr uint32_t kPhase1 = 5;
constexpr uint32_t kPhase2 = 6;
constexpr uint32_t kNominal1 = 7;
constexpr uint32_t kNominal2 = 8;
constexpr uint32_t kOutputVolume = 9;
constexpr uint32_t kOutputMute = 10;
constexpr uint32_t kCrosspointFirst = 11;
constexpr uint32_t kCrosspointLast = 18;
constexpr uint32_t kInputSource1 = 19;
constexpr uint32_t kInputSource2 = 20;
constexpr uint32_t kOutputSource = 21;
constexpr uint32_t kOutputNominalLevel = 22;
constexpr uint32_t kStereoLink = 23;
constexpr uint32_t kHardwareKnobTarget = 24;
constexpr uint32_t kMainMuteFollow = 25;
constexpr uint32_t kHeadphoneMuteFollow = 26;
constexpr int32_t kNormalizedOne = 1'000'000;
// The original control path updates its live meter/knob state at about 33 Hz.
// A FireBug export can omit transactions, so its sparse visible records are
// not a cadence measurement. Keep one complete optional telemetry bundle per
// 30 ms; this is also the upper bound on observable meter latency.
constexpr uint64_t kMeterPollNs = 30ULL * 1000ULL * 1000ULL;
// A failed sample should recover promptly. Metering is explicitly opt-in, and
// a bounded 100 ms retry avoids turning a transient bus error into seconds of
// frozen UI while still keeping an absent route from spinning at 33 Hz.
constexpr uint64_t kMeterRetryNs = 100ULL * 1000ULL * 1000ULL;

[[nodiscard]] int16_t MeterMagnitude(int32_t raw) noexcept {
    constexpr int32_t kMaximum = 0x3fff;
    return static_cast<int16_t>(std::clamp(raw >> 17U, 0, kMaximum));
}

[[nodiscard]] int32_t NormalizedCoefficient(uint16_t coefficient) noexcept {
    return static_cast<int32_t>((static_cast<uint64_t>(coefficient) * kNormalizedOne + 8191U) / 16383U);
}

[[nodiscard]] uint16_t CoefficientFromNormalized(int32_t value) noexcept {
    const auto bounded = std::clamp(value, 0, kNormalizedOne);
    return static_cast<uint16_t>((static_cast<uint64_t>(bounded) * 16383U +
                                  static_cast<uint64_t>(kNormalizedOne / 2)) /
                                 kNormalizedOne);
}

[[nodiscard]] InputGainContext GainContext(const InputParams& input, size_t index) noexcept {
    if (input.sources[index] == InputSource::Phone) return InputGainContext::kPhone;
    return input.xlrNominalLevels[index] == InputXlrNominalLevel::Microphone
        ? InputGainContext::kXlrMicrophone
        : InputGainContext::kXlrFixedLevel;
}

} // namespace

ApogeeDuetProtocol::ApogeeDuetProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                       Protocols::Ports::FireWireBusInfo& busInfo,
                                       Discovery::DeviceRouteToken route,
                                       Discovery::DeviceRegistry* routeRegistry,
                                       Protocols::AVC::FCPTransport* fcpTransport,
                                       IRM::IRMClient* irmClient,
                                       CMP::CMPClient* cmpClient,
                                       uint32_t formatSettleDelayMs,
                                       Scheduling::ITimerScheduler* timerScheduler)
    : runtime_{
          .busOps = busOps,
          .busInfo = busInfo,
          .route = route,
          .routeRegistry = routeRegistry,
          .fcpTransport = fcpTransport,
          .irmClient = irmClient,
          .cmpClient = cmpClient,
          .timerScheduler = timerScheduler,
          .formatSettleDelayMs = formatSettleDelayMs,
      }
    , duplex_(runtime_)
    , semanticControlState_(std::make_shared<SemanticControlState>())
    , meteringState_(std::make_shared<MeteringState>()) {
}

ApogeeDuetProtocol::~ApogeeDuetProtocol() {
    (void)SetAudioMeteringEnabled(false);
    if (!semanticControlState_ || !semanticControlState_->lock) return;
    IOLockLock(semanticControlState_->lock);
    ++semanticControlState_->epoch;
    semanticControlState_->valid = false;
    semanticControlState_->writeInFlight = false;
    IOLockUnlock(semanticControlState_->lock);
}

IOReturn ApogeeDuetProtocol::Initialize() {
    RefreshSemanticControlState();
    return kIOReturnSuccess;
}

IOReturn ApogeeDuetProtocol::Shutdown() {
    (void)SetAudioMeteringEnabled(false);
    duplex_.Shutdown();
    if (semanticControlState_ && semanticControlState_->lock) {
        IOLockLock(semanticControlState_->lock);
        ++semanticControlState_->epoch;
        semanticControlState_->valid = false;
        semanticControlState_->writeInFlight = false;
        IOLockUnlock(semanticControlState_->lock);
    }
    return kIOReturnSuccess;
}

void ApogeeDuetProtocol::UpdateRuntimeContext(
    const Discovery::DeviceRouteToken& route, Protocols::AVC::FCPTransport* transport) {
    duplex_.UpdateRuntimeContext(route, transport);
    (void)SetAudioMeteringEnabled(false);
    if (semanticControlState_ && semanticControlState_->lock) {
        IOLockLock(semanticControlState_->lock);
        ++semanticControlState_->epoch;
        semanticControlState_->valid = false;
        semanticControlState_->writeInFlight = false;
        IOLockUnlock(semanticControlState_->lock);
    }
    RefreshSemanticControlState();
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

void ApogeeDuetProtocol::GetMicsGrouped(ResultCallback<bool> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        {VendorCommand::Bool(VendorCommand::Code::MicsGrouped, false)}, true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess || responses.size() != 1U) {
                Common::InvokeSharedCallback(callbackState,
                                             status != kIOReturnSuccess ? status : kIOReturnError,
                                             false);
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, responses[0].boolValue);
        });
}

void ApogeeDuetProtocol::SetMicsGrouped(bool enabled, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        {VendorCommand::Bool(VendorCommand::Code::MicsGrouped, enabled)}, false,
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

void ApogeeDuetProtocol::RefreshSemanticControlState() noexcept {
    if (!semanticControlState_ || !semanticControlState_->lock || !runtime_.fcpTransport) return;

    struct Refresh final {
        std::shared_ptr<SemanticControlState> state;
        uint64_t epoch{0};
        std::atomic<uint32_t> pending{5};
        std::atomic<int32_t> status{kIOReturnSuccess};
        OutputParams output{};
        InputParams input{};
        MixerParams mixer{};
        KnobState knob{};
        bool micsGrouped{false};
    };

    const auto refresh = std::make_shared<Refresh>();
    refresh->state = semanticControlState_;
    IOLockLock(refresh->state->lock);
    refresh->epoch = refresh->state->epoch;
    refresh->state->valid = false;
    IOLockUnlock(refresh->state->lock);

    const auto finish = [refresh](IOReturn status) {
        if (status != kIOReturnSuccess) {
            int32_t expected = kIOReturnSuccess;
            (void)refresh->status.compare_exchange_strong(expected, status);
        }
        if (refresh->pending.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        if (refresh->status.load(std::memory_order_acquire) != kIOReturnSuccess ||
            !refresh->state->lock) return;

        IOLockLock(refresh->state->lock);
        if (refresh->state->epoch == refresh->epoch) {
            refresh->state->output = refresh->output;
            refresh->state->input = refresh->input;
            refresh->state->mixer = refresh->mixer;
            refresh->state->knob = refresh->knob;
            refresh->state->micsGrouped = refresh->micsGrouped;
            refresh->state->valid = true;
            ++refresh->state->revision;
        }
        IOLockUnlock(refresh->state->lock);
    };

    auto* transport = runtime_.fcpTransport;
    VendorFcp::ExecuteSequence(
        transport, ParamsSerdes::BuildOutputParamsQuery(), true,
        [refresh, finish](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status == kIOReturnSuccess) refresh->output = ParamsSerdes::ParseOutputParams(responses);
            finish(status);
        });
    VendorFcp::ExecuteSequence(
        transport, ParamsSerdes::BuildInputParamsQuery(), true,
        [refresh, finish](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status == kIOReturnSuccess) refresh->input = ParamsSerdes::ParseInputParams(responses);
            finish(status);
        });
    VendorFcp::ExecuteSequence(
        transport, ParamsSerdes::BuildMixerParamsQuery(), true,
        [refresh, finish](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status == kIOReturnSuccess) refresh->mixer = ParamsSerdes::ParseMixerParams(responses);
            finish(status);
        });
    VendorFcp::ExecuteSequence(
        transport, ParamsSerdes::BuildKnobStateQuery(), true,
        [refresh, finish](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status == kIOReturnSuccess && responses.size() == 1U) {
                refresh->knob = ParamsSerdes::ParseKnobState(responses[0]);
            } else if (status == kIOReturnSuccess) {
                status = kIOReturnError;
            }
            finish(status);
        });
    VendorFcp::ExecuteSequence(
        transport, {VendorCommand::Bool(VendorCommand::Code::MicsGrouped, false)}, true,
        [refresh, finish](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status == kIOReturnSuccess && responses.size() == 1U) {
                refresh->micsGrouped = responses[0].boolValue;
            } else if (status == kIOReturnSuccess) {
                status = kIOReturnError;
            }
            finish(status);
        });
}

bool ApogeeDuetProtocol::CopyAudioControlSurfaceSnapshot(
    AudioControlSurfaceSnapshot& outSnapshot) const noexcept {
    outSnapshot = {};
    const auto state = semanticControlState_;
    if (!state || !state->lock) return false;

    IOLockLock(state->lock);
    if (!state->valid || state->writeInFlight) {
        IOLockUnlock(state->lock);
        return false;
    }

    outSnapshot.kind = AudioControlSurfaceKind::ApogeeDuet;
    outSnapshot.stateRevision = state->revision;
    auto emit = [&outSnapshot](uint32_t id, int32_t value) {
        outSnapshot.values[outSnapshot.valueCount++] = {.id = id, .value = value};
    };
    for (size_t index = 0; index < 2; ++index) {
        // A fixed-level XLR selection has no gain in dB.  Preserve the native
        // value here; the semantic topology's future availability projection
        // will hide the gain parameter for that route rather than fabricate a
        // calibrated value.
        const auto context = GainContext(state->input, index);
        const int32_t gain = ApogeeDuetSpec::InputGainHasDbMeaning(context)
            ? ApogeeDuetSpec::InputGainToDb(state->input.gains[index], context)
            : state->input.gains[index];
        emit(index == 0 ? kInputGain1 : kInputGain2, gain);
        emit(index == 0 ? kPhantom1 : kPhantom2, state->input.phantomPowerings[index] ? 1 : 0);
        emit(index == 0 ? kPhase1 : kPhase2, state->input.polarities[index] ? 1 : 0);
        emit(index == 0 ? kNominal1 : kNominal2,
             static_cast<int32_t>(state->input.xlrNominalLevels[index]));
    }
    emit(kOutputVolume, ApogeeDuetSpec::OutputVolumeToDb(state->output.volume));
    emit(kOutputMute, state->output.mute ? 1 : 0);
    uint32_t control = kCrosspointFirst;
    for (const auto& output : state->mixer.outputs) {
        for (const auto coefficient : output.analogInputs) emit(control++, NormalizedCoefficient(coefficient));
        for (const auto coefficient : output.streamInputs) emit(control++, NormalizedCoefficient(coefficient));
    }
    emit(kInputSource1, static_cast<int32_t>(state->input.sources[0]));
    emit(kInputSource2, static_cast<int32_t>(state->input.sources[1]));
    emit(kOutputSource, static_cast<int32_t>(state->output.source));
    emit(kOutputNominalLevel, static_cast<int32_t>(state->output.nominalLevel));
    emit(kStereoLink, state->micsGrouped ? 1 : 0);
    emit(kHardwareKnobTarget, static_cast<int32_t>(state->knob.target));
    emit(kMainMuteFollow, static_cast<int32_t>(state->output.lineMuteMode));
    emit(kHeadphoneMuteFollow, static_cast<int32_t>(state->output.hpMuteMode));
    IOLockUnlock(state->lock);
    return outSnapshot.valueCount == 26;
}

bool ApogeeDuetProtocol::SupportsConfiguration(
    const Configuration::DeviceConfiguration& configuration) const noexcept {
    // Duet has no optical transport selector.  Do not smuggle a made-up
    // S/PDIF/ADAT setting through the generic configuration model.
    return !configuration.opticalInput && !configuration.opticalOutput &&
           (configuration.sampleRate == 44100U ||
            configuration.sampleRate == 48000U ||
            configuration.sampleRate == 96000U);
}

void ApogeeDuetProtocol::ApplyConfiguration(
    const Configuration::DeviceConfiguration& configuration,
    IAudioConfigurationControl::ApplyCallback callback) {
    if (!callback) return;
    if (!SupportsConfiguration(configuration)) {
        callback(kIOReturnUnsupported, {});
        return;
    }

    // This is deliberately the same AV/C signal-format state machine used by
    // PrepareDuplex.  Its successful result carries the selected sample rate
    // into runtime caps, which makes the ADK packetizer reconfigure both AM824
    // FDF and the rational 44.1-family cadence before streaming resumes.
    duplex_.ApplyClockConfig(
        {.sampleRateHz = configuration.sampleRate},
        [configuration, callback = std::move(callback)](
            IOReturn status, ClockApplyResult result) mutable {
            if (status != kIOReturnSuccess) {
                callback(status, {});
                return;
            }
            callback(kIOReturnSuccess,
                     {.configuration = configuration, .runtimeCaps = result.runtimeCaps});
        });
}

AudioConfigurationApplyResult ApogeeDuetProtocol::CurrentConfiguration() const noexcept {
    AudioStreamRuntimeCaps caps{};
    (void)duplex_.GetRuntimeAudioStreamCaps(caps);
    const uint32_t rate = caps.sampleRateHz != 0 ? caps.sampleRateHz : 48000U;
    return {
        .configuration = {.sampleRate = rate},
        .runtimeCaps = caps,
    };
}

void ApogeeDuetProtocol::ApplyAudioControlValue(
    uint32_t controlId, int32_t value, IAudioControlSurface::ApplyCallback callback) {
    if (!callback) return;
    const auto state = semanticControlState_;
    if (!state || !state->lock) {
        callback(kIOReturnNotReady);
        return;
    }

    OutputParams output{};
    InputParams input{};
    MixerParams mixer{};
    bool micsGrouped{false};
    enum class Group { Input, Output, Mixer, StereoLink } group{Group::Input};
    uint64_t epoch = 0;
    IOReturn rejected = kIOReturnSuccess;
    IOLockLock(state->lock);
    if (!state->valid || state->writeInFlight) {
        rejected = state->valid ? kIOReturnBusy : kIOReturnNotReady;
    } else {
        output = state->output;
        input = state->input;
        mixer = state->mixer;
        micsGrouped = state->micsGrouped;
        epoch = state->epoch;
        switch (controlId) {
            case kInputGain1:
            case kInputGain2: {
                const size_t index = controlId - kInputGain1;
                const auto context = GainContext(input, index);
                if (!ApogeeDuetSpec::InputGainHasDbMeaning(context)) {
                    rejected = kIOReturnUnsupported;
                    break;
                }
                const int32_t raw = context == InputGainContext::kPhone ? value + 10 : value;
                if (raw < InputParams::kGainMin || raw > InputParams::kGainMax) {
                    rejected = kIOReturnBadArgument;
                    break;
                }
                input.gains[index] = static_cast<uint8_t>(raw);
                group = Group::Input;
                break;
            }
            case kPhantom1:
            case kPhantom2:
                if (value != 0 && value != 1) rejected = kIOReturnBadArgument;
                else {
                    input.phantomPowerings[controlId - kPhantom1] = value != 0;
                    group = Group::Input;
                }
                break;
            case kPhase1:
            case kPhase2:
                if (value != 0 && value != 1) rejected = kIOReturnBadArgument;
                else {
                    input.polarities[controlId - kPhase1] = value != 0;
                    group = Group::Input;
                }
                break;
            case kNominal1:
            case kNominal2:
                if (value < 0 || value > 2) rejected = kIOReturnBadArgument;
                else {
                    input.xlrNominalLevels[controlId - kNominal1] =
                        static_cast<InputXlrNominalLevel>(value);
                    group = Group::Input;
                }
                break;
            case kOutputVolume:
                if (value < -64 || value > 0) rejected = kIOReturnBadArgument;
                else {
                    output.volume = ApogeeDuetSpec::OutputVolumeFromDb(value);
                    group = Group::Output;
                }
                break;
            case kOutputMute:
                if (value != 0 && value != 1) rejected = kIOReturnBadArgument;
                else {
                    output.mute = value != 0;
                    group = Group::Output;
                }
                break;
            case kMainMuteFollow:
            case kHeadphoneMuteFollow:
                if (value < static_cast<int32_t>(OutputMuteMode::Never) ||
                    value > static_cast<int32_t>(OutputMuteMode::Swapped)) {
                    rejected = kIOReturnBadArgument;
                } else {
                    const auto mode = static_cast<OutputMuteMode>(value);
                    if (controlId == kMainMuteFollow) output.lineMuteMode = mode;
                    else output.hpMuteMode = mode;
                    group = Group::Output;
                }
                break;
            case kInputSource1:
            case kInputSource2:
                if (value < static_cast<int32_t>(InputSource::Xlr) ||
                    value > static_cast<int32_t>(InputSource::Phone)) {
                    rejected = kIOReturnBadArgument;
                } else {
                    input.sources[controlId - kInputSource1] = static_cast<InputSource>(value);
                    group = Group::Input;
                }
                break;
            case kOutputSource:
                if (value < static_cast<int32_t>(OutputSource::StreamInputPair0) ||
                    value > static_cast<int32_t>(OutputSource::MixerOutputPair0)) {
                    rejected = kIOReturnBadArgument;
                } else {
                    output.source = static_cast<OutputSource>(value);
                    group = Group::Output;
                }
                break;
            case kOutputNominalLevel:
                if (value < static_cast<int32_t>(OutputNominalLevel::Instrument) ||
                    value > static_cast<int32_t>(OutputNominalLevel::Consumer)) {
                    rejected = kIOReturnBadArgument;
                } else {
                    output.nominalLevel = static_cast<OutputNominalLevel>(value);
                    group = Group::Output;
                }
                break;
            case kStereoLink:
                if (value != 0 && value != 1) rejected = kIOReturnBadArgument;
                else {
                    micsGrouped = value != 0;
                    group = Group::StereoLink;
                }
                break;
            case kHardwareKnobTarget:
                rejected = kIOReturnUnsupported;
                break;
            default:
                if (controlId < kCrosspointFirst || controlId > kCrosspointLast ||
                    value < 0 || value > kNormalizedOne) {
                    rejected = kIOReturnBadArgument;
                    break;
                }
                {
                    const size_t offset = controlId - kCrosspointFirst;
                    auto& coefficients = mixer.outputs[offset / 4];
                    if (offset % 4 < 2) coefficients.analogInputs[offset % 4] = CoefficientFromNormalized(value);
                    else coefficients.streamInputs[offset % 4 - 2] = CoefficientFromNormalized(value);
                    group = Group::Mixer;
                }
                break;
        }
        if (rejected == kIOReturnSuccess) state->writeInFlight = true;
    }
    IOLockUnlock(state->lock);
    if (rejected != kIOReturnSuccess) {
        callback(rejected);
        return;
    }

    const auto complete = [state, epoch, callback = std::move(callback)](
                              IOReturn status, const OutputParams& nextOutput,
                              const InputParams& nextInput, const MixerParams& nextMixer,
                              bool nextMicsGrouped) {
        if (!state->lock) {
            callback(kIOReturnAborted);
            return;
        }
        IOLockLock(state->lock);
        if (state->epoch != epoch) status = kIOReturnAborted;
        if (status == kIOReturnSuccess) {
            state->output = nextOutput;
            state->input = nextInput;
            state->mixer = nextMixer;
            state->micsGrouped = nextMicsGrouped;
            ++state->revision;
        }
        state->writeInFlight = false;
        IOLockUnlock(state->lock);
        callback(status);
    };

    switch (group) {
        case Group::Input:
            SetInputParams(input, [complete, output, input, mixer, micsGrouped](IOReturn status) mutable {
                complete(status, output, input, mixer, micsGrouped);
            });
            break;
        case Group::Output:
            SetOutputParams(output, [complete, output, input, mixer, micsGrouped](IOReturn status) mutable {
                complete(status, output, input, mixer, micsGrouped);
            });
            break;
        case Group::Mixer:
            SetMixerParams(mixer, [complete, output, input, mixer, micsGrouped](IOReturn status) mutable {
                complete(status, output, input, mixer, micsGrouped);
            });
            break;
        case Group::StereoLink:
            SetMicsGrouped(micsGrouped, [complete, output, input, mixer, micsGrouped](IOReturn status) mutable {
                complete(status, output, input, mixer, micsGrouped);
            });
            break;
    }
}

bool ApogeeDuetProtocol::CopyAudioMeterSnapshot(
    AudioMeterSnapshot& outSnapshot) const noexcept {
    outSnapshot = {};
    const auto state = meteringState_;
    if (!state || !state->lock) return false;

    IOLockLock(state->lock);
    outSnapshot.telemetrySequence = state->revision;
    outSnapshot.valueCount = static_cast<uint32_t>(state->values.size());
    outSnapshot.enabled = state->enabled;
    for (size_t index = 0; index < state->values.size(); ++index) {
        outSnapshot.values[index] = state->values[index];
    }
    IOLockUnlock(state->lock);
    return true;
}

IOReturn ApogeeDuetProtocol::SetAudioMeteringEnabled(bool enabled) noexcept {
    const auto state = meteringState_;
    if (!state || !state->lock) return kIOReturnUnsupported;

    Scheduling::TimerToken cancelled{Scheduling::kInvalidTimerToken};
    uint64_t epoch{0};
    IOLockLock(state->lock);
    if (state->enabled == enabled) {
        IOLockUnlock(state->lock);
        return kIOReturnSuccess;
    }
    state->enabled = enabled;
    epoch = ++state->epoch;
    cancelled = std::exchange(state->timer, Scheduling::kInvalidTimerToken);
    state->readInFlight = false;
    state->readEpoch = 0;
    if (!enabled) {
        state->values.fill(0);
        ++state->revision;
    }
    IOLockUnlock(state->lock);

    if (cancelled != Scheduling::kInvalidTimerToken && runtime_.timerScheduler) {
        runtime_.timerScheduler->Cancel(cancelled);
    }
    if (enabled) ScheduleMeterRead(0, epoch);
    return kIOReturnSuccess;
}

void ApogeeDuetProtocol::ScheduleMeterRead(uint64_t delayNs, uint64_t epoch) noexcept {
    const auto state = meteringState_;
    if (!state || !state->lock || !runtime_.timerScheduler) {
        ASFW_LOG_ERROR(Oxfw, "[DuetMeter] unavailable: no timer scheduler");
        return;
    }
    const auto timer = runtime_.timerScheduler->ScheduleAfter(delayNs, [this, epoch] {
        PollMeter(epoch);
    });
    if (timer == Scheduling::kInvalidTimerToken) {
        ASFW_LOG_ERROR(Oxfw, "[DuetMeter] timer scheduling failed");
        return;
    }

    bool keep{false};
    IOLockLock(state->lock);
    if (state->enabled && state->epoch == epoch &&
        state->timer == Scheduling::kInvalidTimerToken) {
        state->timer = timer;
        keep = true;
    }
    IOLockUnlock(state->lock);
    if (!keep) runtime_.timerScheduler->Cancel(timer);
}

void ApogeeDuetProtocol::PollMeter(uint64_t epoch) noexcept {
    const auto state = meteringState_;
    if (!state || !state->lock) return;

    IOLockLock(state->lock);
    state->timer = Scheduling::kInvalidTimerToken;
    if (!state->enabled || state->epoch != epoch || state->readInFlight) {
        IOLockUnlock(state->lock);
        return;
    }
    state->readInFlight = true;
    state->readEpoch = epoch;
    IOLockUnlock(state->lock);

    struct Poll final {
        std::atomic<uint32_t> pending{2};
        std::atomic<int32_t> status{kIOReturnSuccess};
        InputMeterState input{};
        MixerMeterState mixer{};
    };
    const auto poll = std::make_shared<Poll>();
    const auto finish = [this, epoch, poll](IOReturn status) {
        if (status != kIOReturnSuccess) {
            int32_t expected{kIOReturnSuccess};
            (void)poll->status.compare_exchange_strong(expected, status);
        }
        if (poll->pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            CompleteMeterRead(epoch, poll->status.load(std::memory_order_acquire),
                              poll->input, poll->mixer);
        }
    };
    GetInputMeter([poll, finish](IOReturn status, InputMeterState value) {
        if (status == kIOReturnSuccess) poll->input = value;
        finish(status);
    });
    GetMixerMeter([poll, finish](IOReturn status, MixerMeterState value) {
        if (status == kIOReturnSuccess) poll->mixer = value;
        finish(status);
    });

    // The encoder state is a separate vendor status read, not meter payload.
    // It shares this optional telemetry lifecycle so disabling metering also
    // stops all periodic Duet traffic.
    GetKnobState([this, epoch](IOReturn status, KnobState knob) {
        if (status == kIOReturnSuccess) ApplyPolledKnobState(epoch, knob);
    });
}

void ApogeeDuetProtocol::CompleteMeterRead(uint64_t epoch, IOReturn status,
                                            const InputMeterState& input,
                                            const MixerMeterState& mixer) noexcept {
    const auto state = meteringState_;
    if (!state || !state->lock) return;

    uint64_t nextDelay{kMeterRetryNs};
    bool reschedule{false};
    IOLockLock(state->lock);
    if (state->readInFlight && state->readEpoch == epoch) {
        state->readInFlight = false;
        state->readEpoch = 0;
    }
    if (state->enabled && state->epoch == epoch && status == kIOReturnSuccess) {
        state->values = {
            MeterMagnitude(input.levels[0]), MeterMagnitude(input.levels[1]),
            MeterMagnitude(mixer.streamInputs[0]), MeterMagnitude(mixer.streamInputs[1]),
            MeterMagnitude(mixer.mixerOutputs[0]), MeterMagnitude(mixer.mixerOutputs[1]),
        };
        ++state->revision;
        nextDelay = kMeterPollNs;
    }
    reschedule = state->enabled && state->epoch == epoch;
    IOLockUnlock(state->lock);

    if (status != kIOReturnSuccess && reschedule) {
        ASFW_LOG_ERROR(Oxfw, "[DuetMeter] read rejected kr=0x%08x; retry=1s",
                       static_cast<unsigned>(status));
    }
    if (reschedule) ScheduleMeterRead(nextDelay, epoch);
}

void ApogeeDuetProtocol::ApplyPolledKnobState(uint64_t epoch,
                                               const KnobState& knob) noexcept {
    const auto meter = meteringState_;
    const auto semantic = semanticControlState_;
    if (!meter || !meter->lock || !semantic || !semantic->lock) return;

    IOLockLock(meter->lock);
    const bool active = meter->enabled && meter->epoch == epoch;
    IOLockUnlock(meter->lock);
    if (!active) return;

    IOLockLock(semantic->lock);
    if (semantic->valid && !semantic->writeInFlight) {
        semantic->knob = knob;
        semantic->output.mute = knob.outputMute;
        semantic->output.volume = knob.outputVolume;
        semantic->input.gains = knob.inputGains;
        ++semantic->revision;
    }
    IOLockUnlock(semantic->lock);
}

void ApogeeDuetProtocol::GetInputMeter(ResultCallback<InputMeterState> callback) {
    MeterRegisters::ReadInput(runtime_.busOps, MakeRouteProvider(), std::move(callback));
}

void ApogeeDuetProtocol::GetMixerMeter(ResultCallback<MixerMeterState> callback) {
    MeterRegisters::ReadMixer(runtime_.busOps, MakeRouteProvider(), std::move(callback));
}

// The Oxford ASIC registers are chip-common, not Duet-specific (FW-137), so
// the register map and decode live in Oxford/OxfordCsr. What stays here is the
// Duet's own route policy, expressed as the provider that layer resolves through.
//
// FW-142: resolve the route per use, straight from the registry.
//
// This originally validated `runtime_.route` — a token snapshotted at
// construction — through `runtime_.cmpClient`. That failed for a blunter reason
// than a stale epoch: the two construction sites bind *different* optional
// clients. The discovery prefetch path, which is where the CSR and meter reads
// run, passes a real FCP transport and **null** IRM/CMP clients
// (AVCDiscovery.cpp), so the validator short-circuited on the null check and
// reported "route not current" without ever comparing a route. Node and
// generation matched in the log because nothing examined them.
//
// The registry is the only route authority both sites can supply, which is why
// it is now its own constructor parameter rather than something borrowed from a
// client that may not exist. FCPTransport re-resolves the same way per
// submission (FCPTransport.cpp:206), which is why vendor commands never failed.
//
// The lambda captures the registry pointer and runtime instance by value rather than `this`,
// so an in-flight read cannot outlive the protocol and dereference it.
Oxford::RouteProvider ApogeeDuetProtocol::MakeRouteProvider() const {
    if (runtime_.routeRegistry == nullptr) {
        // Distinct from "the device has no live route": this is a wiring bug,
        // and returning an empty provider is what makes the reads say so.
        return {};
    }
    return [registry = runtime_.routeRegistry,
            instanceId = runtime_.route.deviceInstanceId]()
               -> std::optional<Discovery::DeviceRouteToken> {
        return registry->CurrentRoute(instanceId);
    };
}

void ApogeeDuetProtocol::GetFirmwareId(ResultCallback<uint32_t> callback) {
    Oxford::ReadFirmwareId(runtime_.busOps, MakeRouteProvider(), std::move(callback));
}

void ApogeeDuetProtocol::GetHardwareId(ResultCallback<uint32_t> callback) {
    Oxford::ReadHardwareId(runtime_.busOps, MakeRouteProvider(), std::move(callback));
}

} // namespace ASFW::Audio::Oxford::Apogee
