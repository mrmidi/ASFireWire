// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "ExistingFamilyProviders.hpp"

#include "../Protocols/BeBoB/GenericBeBoBProtocol.hpp"
#include "../Protocols/BeBoB/MAudioSpecialProtocol.hpp"
#include "../Protocols/BeBoB/Phase88Protocol.hpp"
#include "../Protocols/DICE/Focusrite/SPro24DspProtocol.hpp"
#include "../Protocols/DICE/TCAT/DICETcatProtocol.hpp"
#include "../Protocols/Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Protocols/AVC/IAVCDiscovery.hpp"
#include "../../Protocols/AVC/RemoteAvcSession.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"

#include <DriverKit/IOLib.h>

#include <algorithm>
#include <atomic>
#include <expected>
#include <utility>

namespace ASFW::Audio::Families {
namespace {

using FamilyId = DeviceProfiles::Audio::AudioFamilyProviderId;
using ProbePolicyId = DeviceProfiles::Audio::ProbePolicyId;
using ProfileBuilderId = DeviceProfiles::Audio::ProfileBuilderId;

class Lock final {
public:
    explicit Lock(IOLock* lock) noexcept : lock_(lock) {
        if (lock_) IOLockLock(lock_);
    }
    ~Lock() {
        if (lock_) IOLockUnlock(lock_);
    }
private:
    IOLock* lock_{nullptr};
};

[[nodiscard]] Devices::ProbeError MapProbeStatus(IOReturn status) noexcept {
    switch (status) {
        case kIOReturnAborted:
            return Devices::ProbeError::Cancelled;
        case kIOReturnNoDevice:
        case kIOReturnNotResponding:
        case kIOReturnOffline:
            return Devices::ProbeError::StaleRoute;
        case kIOReturnUnsupported:
            return Devices::ProbeError::Unsupported;
        default:
            return Devices::ProbeError::Transport;
    }
}

[[nodiscard]] bool HasUsableDuplexGeometry(
    const AudioStreamRuntimeCaps& caps) noexcept {
    return caps.sampleRateHz != 0 &&
           caps.deviceToHostStreamCount != 0 &&
           caps.hostToDeviceStreamCount != 0 &&
           caps.deviceToHostAm824Slots != 0 &&
           caps.hostToDeviceAm824Slots != 0;
}

[[nodiscard]] std::vector<uint32_t> IntersectRates(
    std::vector<uint32_t> observed,
    const std::vector<uint32_t>& implemented) {
    std::erase_if(observed, [&implemented](uint32_t rate) {
        return std::find(implemented.begin(), implemented.end(), rate) ==
               implemented.end();
    });
    return observed;
}

[[nodiscard]] bool PolicyMatchesFamily(FamilyId family,
                                       ProbePolicyId policy) noexcept {
    switch (family) {
        case FamilyId::GenericAvc:
            return policy == ProbePolicyId::GenericAvc;
        case FamilyId::BeBoB:
            // BeBoBFilteredCommandSet is a BeBoB device that must never be
            // probed. It takes the same family provider and a different route
            // through it — see StartProbe.
            return policy == ProbePolicyId::BeBoBPlug0 ||
                   policy == ProbePolicyId::BeBoBFilteredCommandSet;
        case FamilyId::DICE:
            return policy == ProbePolicyId::DiceTcat;
        case FamilyId::OXFW:
            return policy == ProbePolicyId::OxfwAvc;
        case FamilyId::None:
        default:
            return false;
    }
}

class AdapterState final : public std::enable_shared_from_this<AdapterState> {
public:
    AdapterState(FamilyId family,
                 ExistingFamilyProviderDependencies dependencies,
                 Devices::AudioFamilyProviderContext context) noexcept
        : family_(family), dependencies_(dependencies), context_(std::move(context)),
          lock_(IOLockAlloc()) {}

    ~AdapterState() {
        (void)Shutdown();
        if (lock_) IOLockFree(lock_);
    }

    [[nodiscard]] bool Probe(Devices::IAudioDeviceAdapter::ProbeCompletion completion) {
        if (!completion || !lock_) return false;

        uint64_t epoch = 0;
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        {
            Lock guard(lock_);
            if (shuttingDown_ || pendingCompletion_) return false;
            epoch = ++probeEpoch_;
            pendingCompletion_ = std::move(completion);
            activeCancel_ = cancel;
        }

        const auto token = dependencies_.scheduler.ScheduleAfter(
            0, [self = shared_from_this(), epoch, cancel] {
                self->StartProbe(epoch, std::move(cancel));
            });
        if (token == Scheduling::kInvalidTimerToken) {
            Complete(epoch, std::unexpected(Devices::ProbeError::Transport));
            return true;
        }

        bool stillActive = false;
        {
            Lock guard(lock_);
            stillActive = IsActiveLocked(epoch);
            if (stillActive) kickoffTimer_ = token;
        }
        if (!stillActive) dependencies_.scheduler.Cancel(token);
        return true;
    }

    void CancelProbe() noexcept {
        Devices::IAudioDeviceAdapter::ProbeCompletion completion;
        Scheduling::TimerToken timer = Scheduling::kInvalidTimerToken;
        std::shared_ptr<std::atomic<bool>> activeCancel;
        std::shared_ptr<std::atomic<bool>> protocolCancel;
        {
            Lock guard(lock_);
            if (!pendingCompletion_ && !activeCancel_ && !protocolCancel_ &&
                kickoffTimer_ == Scheduling::kInvalidTimerToken) {
                return;
            }
            activeCancel = activeCancel_;
            protocolCancel = protocolCancel_;
            timer = std::exchange(kickoffTimer_, Scheduling::kInvalidTimerToken);
            completion = std::move(pendingCompletion_);
            ++probeEpoch_;
        }
        // A completed probe can already have installed its device protocol. Its
        // teardown token remains live until the replacement protocol is
        // installed or the adapter shuts down, so reset cancellation must
        // invalidate both in-flight discovery and the installed protocol.
        if (activeCancel) activeCancel->store(true, std::memory_order_release);
        if (protocolCancel) protocolCancel->store(true, std::memory_order_release);
        if (timer != Scheduling::kInvalidTimerToken) {
            dependencies_.scheduler.Cancel(timer);
        }
        if (completion) {
            completion(std::unexpected(Devices::ProbeError::Cancelled));
        }
    }

    [[nodiscard]] IOReturn Shutdown() noexcept {
        Devices::IAudioDeviceAdapter::ProbeCompletion completion;
        Scheduling::TimerToken timer = Scheduling::kInvalidTimerToken;
        std::shared_ptr<std::atomic<bool>> activeCancel;
        std::shared_ptr<std::atomic<bool>> protocolCancel;
        std::shared_ptr<IDeviceProtocol> protocol;
        {
            Lock guard(lock_);
            if (shuttingDown_) return kIOReturnSuccess;
            shuttingDown_ = true;
            ++probeEpoch_;
            timer = std::exchange(kickoffTimer_, Scheduling::kInvalidTimerToken);
            completion = std::move(pendingCompletion_);
            activeCancel = std::move(activeCancel_);
            protocolCancel = std::move(protocolCancel_);
            protocol = std::move(protocol_);
            avcSession_.reset();
        }
        if (activeCancel) activeCancel->store(true, std::memory_order_release);
        if (protocolCancel) protocolCancel->store(true, std::memory_order_release);
        if (timer != Scheduling::kInvalidTimerToken) {
            dependencies_.scheduler.Cancel(timer);
        }
        if (completion) {
            completion(std::unexpected(Devices::ProbeError::Cancelled));
        }
        return protocol ? protocol->Shutdown() : kIOReturnSuccess;
    }

    [[nodiscard]] IDuplexDeviceControl* DuplexControl() noexcept {
        Lock guard(lock_);
        return protocol_ ? protocol_->AsDuplexDeviceControl() : nullptr;
    }

    [[nodiscard]] std::shared_ptr<IDeviceProtocol> ProtocolHold() noexcept {
        Lock guard(lock_);
        return protocol_;
    }

private:
    [[nodiscard]] bool IsActiveLocked(uint64_t epoch) const noexcept {
        return !shuttingDown_ && pendingCompletion_ && probeEpoch_ == epoch;
    }

    [[nodiscard]] bool IsActive(uint64_t epoch) const noexcept {
        Lock guard(lock_);
        return IsActiveLocked(epoch);
    }

    [[nodiscard]] std::optional<Discovery::DeviceRouteToken>
    CurrentRoute(uint64_t epoch) const noexcept {
        if (!IsActive(epoch)) return std::nullopt;
        auto route = context_.routes.CurrentRoute(context_.unit.device);
        if (!route || !context_.routes.IsCurrent(*route)) return std::nullopt;
        return route;
    }

    void Complete(
        uint64_t epoch,
        std::expected<Devices::FamilyProbeFacts, Devices::ProbeError> result) {
        Devices::IAudioDeviceAdapter::ProbeCompletion completion;
        {
            Lock guard(lock_);
            if (!IsActiveLocked(epoch)) return;
            kickoffTimer_ = Scheduling::kInvalidTimerToken;
            completion = std::move(pendingCompletion_);
        }
        completion(std::move(result));
    }

    void InstallProtocol(std::shared_ptr<IDeviceProtocol> replacement,
                         std::shared_ptr<std::atomic<bool>> cancel) {
        std::shared_ptr<IDeviceProtocol> previous;
        std::shared_ptr<std::atomic<bool>> previousCancel;
        {
            Lock guard(lock_);
            previous = std::exchange(protocol_, std::move(replacement));
            previousCancel = std::exchange(protocolCancel_, std::move(cancel));
        }
        if (previousCancel) previousCancel->store(true, std::memory_order_release);
        if (previous) (void)previous->Shutdown();
    }

    void StartProbe(uint64_t epoch,
                    std::shared_ptr<std::atomic<bool>> cancel) {
        {
            Lock guard(lock_);
            if (!IsActiveLocked(epoch)) return;
            kickoffTimer_ = Scheduling::kInvalidTimerToken;
        }
        if (family_ == FamilyId::DICE) {
            StartDiceProbe(epoch, std::move(cancel));
        } else if (context_.staticPlan.probePolicy ==
                   ProbePolicyId::BeBoBFilteredCommandSet) {
            StartUnprobedInstall(epoch, std::move(cancel));
        } else {
            StartAvcProbe(epoch, std::move(cancel));
        }
    }

    // Bring a device up with no probe at all.
    //
    // StartAvcProbe cannot be used here and no amount of gating inside it would
    // help: its first two acts are AVCUnit::Initialize (UNIT_INFO, SUBUNIT_INFO)
    // and ProbePlug0 (PLUG_INFO), which are precisely the commands that hang
    // this firmware. They would now be refused by the transport's permitted-frame
    // table, so the probe would not damage the device — it would simply always
    // fail, and the device would never come up.
    //
    // Everything StartAvcProbe would have learned is instead asserted by the
    // catalog and the protocol class: geometry from the hardcoded formation
    // table, rates from the model. There is no observed evidence to intersect
    // with, so there is no IntersectRates step — what the protocol says is all
    // there is. That is the trade H5 forces.
    void StartUnprobedInstall(uint64_t epoch,
                              std::shared_ptr<std::atomic<bool>> cancel) {
        auto route = CurrentRoute(epoch);
        if (!route) {
            Complete(epoch, std::unexpected(Devices::ProbeError::StaleRoute));
            return;
        }
        // Acquired for its transport only. Initialize() is deliberately never
        // called — that is the probe.
        auto session = dependencies_.avc.AcquireRemoteSession(context_.unit);
        auto transport = session ? session->Transport() : nullptr;
        if (!transport) {
            Complete(epoch, std::unexpected(Devices::ProbeError::Unsupported));
            return;
        }
        {
            Lock guard(lock_);
            if (!IsActiveLocked(epoch)) return;
            avcSession_ = session;
        }

        auto protocol = CreateUnprobedProtocol(*route);
        if (!protocol) {
            Complete(epoch, std::unexpected(Devices::ProbeError::Unsupported));
            return;
        }
        protocol->UpdateRuntimeContext(*route, transport.get());
        const IOReturn initializeStatus = protocol->Initialize();
        if (initializeStatus != kIOReturnSuccess) {
            Complete(epoch, std::unexpected(MapProbeStatus(initializeStatus)));
            return;
        }

        // Configure the clock before publishing anything. This is Linux's
        // discover-time avc_maudio_set_special_clk, and like Linux we treat
        // failure as fatal: an unclocked device does not transmit, so publishing
        // one produces an endpoint that can only ever time out on ZTS.
        //
        // It is also why the install completes asynchronously — the settle is
        // 2500 ms, and paying it here keeps it off the stream-start path where
        // it would exceed the initial-ZTS budget by itself.
        protocol->InitializeClock(
            [self = shared_from_this(), epoch, cancel = std::move(cancel),
             protocol](IOReturn clockStatus) mutable {
                if (!self->IsActive(epoch)) return;
                if (clockStatus != kIOReturnSuccess) {
                    (void)protocol->Shutdown();
                    self->Complete(epoch,
                        std::unexpected(Devices::ProbeError::Transport));
                    return;
                }
                self->FinishUnprobedInstall(epoch, std::move(cancel), protocol);
            });
    }

    void FinishUnprobedInstall(uint64_t epoch,
                               std::shared_ptr<std::atomic<bool>> cancel,
                               std::shared_ptr<BeBoB::MAudioSpecialProtocol> protocol) {
        AudioStreamRuntimeCaps caps{};
        std::vector<uint32_t> rates;
        if (!protocol->GetRuntimeAudioStreamCaps(caps) ||
            !HasUsableDuplexGeometry(caps) ||
            !protocol->GetSupportedSampleRates(rates) || rates.empty()) {
            // Reachable if the formation table has no entry for the protocol's
            // current rate. Failing here is deliberate: a device published with
            // empty geometry would be silently mis-shaped on the wire.
            (void)protocol->Shutdown();
            Complete(epoch, std::unexpected(Devices::ProbeError::InvalidEvidence));
            return;
        }

        ASFW_LOG(Audio,
                 "[BeBoB] installing %{public}s without probing: %ux%u PCM @ %u Hz",
                 protocol->GetName(), caps.hostInputPcmChannels,
                 caps.hostOutputPcmChannels, caps.sampleRateHz);

        InstallProtocol(protocol, std::move(cancel));
        Complete(epoch, Devices::BeBoBProbeFacts{
            .streams = caps,
            .supportedRates = std::move(rates),
            .operationPolicyId =
                static_cast<uint32_t>(context_.staticPlan.probePolicy),
        });
    }

    // Returns the concrete type: the unprobed path needs InitializeClock, which
    // is device-specific and deliberately not on IDeviceProtocol.
    [[nodiscard]] std::shared_ptr<BeBoB::MAudioSpecialProtocol> CreateUnprobedProtocol(
        const Discovery::DeviceRouteToken& route) {
        switch (context_.staticPlan.profileBuilder) {
            case ProfileBuilderId::MAudioFireWire1814:
                return std::make_shared<BeBoB::MAudioSpecialProtocol>(
                    dependencies_.busOps, dependencies_.busInfo, route,
                    dependencies_.irm, dependencies_.cmp, &dependencies_.scheduler,
                    BeBoB::MAudioSpecialModel::FireWire1814);
            case ProfileBuilderId::MAudioProjectMix:
                return std::make_shared<BeBoB::MAudioSpecialProtocol>(
                    dependencies_.busOps, dependencies_.busInfo, route,
                    dependencies_.irm, dependencies_.cmp, &dependencies_.scheduler,
                    BeBoB::MAudioSpecialModel::ProjectMix);
            default:
                return nullptr;
        }
    }

    [[nodiscard]] std::shared_ptr<IDeviceProtocol> CreateDiceProtocol(
        const Discovery::DeviceRouteToken& route) {
        switch (context_.staticPlan.profileBuilder) {
            case ProfileBuilderId::FocusriteSPro24Dsp:
                return std::make_shared<DICE::Focusrite::SPro24DspProtocol>(
                    dependencies_.busOps, dependencies_.busInfo,
                    dependencies_.routes, route, dependencies_.irm,
                    &dependencies_.scheduler);
            case ProfileBuilderId::WeissInt202:
            case ProfileBuilderId::WeissInt203:
                return std::make_shared<DICE::TCAT::DICETcatProtocol>(
                    dependencies_.busOps, dependencies_.busInfo,
                    dependencies_.routes, route, dependencies_.irm,
                    &dependencies_.scheduler,
                    DICE::TCAT::DICETcatRuntimePolicy{
                        .exposeDeviceToHostToCoreAudio = false,
                        .requireSourceLockBeforeStreamEnable = false,
                        .requireSourceLockAtConfirm = false,
                    });
            case ProfileBuilderId::FocusriteSPro14:
            case ProfileBuilderId::FocusriteSPro24:
            case ProfileBuilderId::AlesisMultiMix:
            case ProfileBuilderId::MidasVeniceF32:
            case ProfileBuilderId::PreSonusStudioLive1602:
                return std::make_shared<DICE::TCAT::DICETcatProtocol>(
                    dependencies_.busOps, dependencies_.busInfo,
                    dependencies_.routes, route, dependencies_.irm,
                    &dependencies_.scheduler);
            default:
                return nullptr;
        }
    }

    void StartDiceProbe(uint64_t epoch,
                        std::shared_ptr<std::atomic<bool>> cancel) {
        const auto route = CurrentRoute(epoch);
        if (!route) {
            Complete(epoch, std::unexpected(Devices::ProbeError::StaleRoute));
            return;
        }
        auto protocol = CreateDiceProtocol(*route);
        if (!protocol) {
            Complete(epoch, std::unexpected(Devices::ProbeError::Unsupported));
            return;
        }
        protocol->UpdateRuntimeContext(*route, nullptr);
        if (auto* duplex = protocol->AsDuplexDeviceControl()) {
            duplex->SetTeardownCancelToken(cancel.get());
        }
        const IOReturn initializeStatus = protocol->Initialize();
        if (initializeStatus != kIOReturnSuccess) {
            Complete(epoch,
                     std::unexpected(MapProbeStatus(initializeStatus)));
            return;
        }

        InstallProtocol(protocol, cancel);
        auto* duplex = protocol->AsDuplexDeviceControl();
        if (!duplex) {
            Complete(epoch, std::unexpected(Devices::ProbeError::Unsupported));
            return;
        }
        duplex->EnsureRuntimeStreamGeometry(
            [self = shared_from_this(), epoch, route = *route,
             protocol = std::move(protocol)](IOReturn status) mutable {
                if (!self->IsActive(epoch)) return;
                if (status != kIOReturnSuccess) {
                    self->Complete(epoch,
                        std::unexpected(MapProbeStatus(status)));
                    return;
                }
                if (!self->context_.routes.IsCurrent(route)) {
                    self->Complete(epoch,
                        std::unexpected(Devices::ProbeError::StaleRoute));
                    return;
                }

                AudioStreamRuntimeCaps caps{};
                std::vector<uint32_t> rates;
                uint32_t clockCapabilities = 0;
                if (!protocol->GetRuntimeAudioStreamCaps(caps) ||
                    !HasUsableDuplexGeometry(caps) ||
                    !protocol->GetSupportedSampleRates(rates) || rates.empty()) {
                    self->Complete(epoch,
                        std::unexpected(Devices::ProbeError::InvalidEvidence));
                    return;
                }
                (void)protocol->GetClockCapabilities(clockCapabilities);
                std::vector<std::string> inputLabels;
                std::vector<std::string> outputLabels;
                (void)protocol->GetChannelLabels(inputLabels, outputLabels);
                self->Complete(epoch, Devices::DiceProbeFacts{
                    .streams = caps,
                    .supportedRates = std::move(rates),
                    .clockCapabilities = clockCapabilities,
                    .inputLabels = std::move(inputLabels),
                    .outputLabels = std::move(outputLabels),
                });
            });
    }

    [[nodiscard]] std::shared_ptr<IDeviceProtocol> CreateAvcProtocol(
        const Discovery::DeviceRouteToken& route,
        Protocols::AVC::FCPTransport* transport,
        const Protocols::AVC::Probe::DeviceModel& model) {
        if (family_ == FamilyId::OXFW) {
            if (context_.staticPlan.profileBuilder != ProfileBuilderId::ApogeeDuet) {
                return nullptr;
            }
            return std::make_shared<Oxford::Apogee::ApogeeDuetProtocol>(
                dependencies_.busOps, dependencies_.busInfo, route,
                &dependencies_.routes, transport, dependencies_.irm,
                dependencies_.cmp, 100U, &dependencies_.scheduler);
        }

        if (family_ == FamilyId::BeBoB &&
            context_.staticPlan.profileBuilder == ProfileBuilderId::TerraTecPhase88) {
            if (!model.SupportsDuplexFormation(10, 1)) return nullptr;
            return std::make_shared<BeBoB::Phase88Protocol>(
                dependencies_.busOps, dependencies_.busInfo, route,
                dependencies_.irm, dependencies_.cmp, &dependencies_.scheduler);
        }

        if ((family_ == FamilyId::BeBoB) ||
            (family_ == FamilyId::GenericAvc &&
             context_.staticPlan.profileBuilder == ProfileBuilderId::GenericAvc)) {
            return std::make_shared<BeBoB::GenericBeBoBProtocol>(
                dependencies_.busOps, dependencies_.busInfo, route,
                dependencies_.irm, dependencies_.cmp, &dependencies_.scheduler,
                model);
        }
        return nullptr;
    }

    void StartAvcProbe(uint64_t epoch,
                       std::shared_ptr<std::atomic<bool>> cancel) {
        if (!CurrentRoute(epoch)) {
            Complete(epoch, std::unexpected(Devices::ProbeError::StaleRoute));
            return;
        }
        auto session = dependencies_.avc.AcquireRemoteSession(context_.unit);
        if (!session) {
            Complete(epoch, std::unexpected(Devices::ProbeError::Unsupported));
            return;
        }
        {
            Lock guard(lock_);
            if (!IsActiveLocked(epoch)) return;
            avcSession_ = session;
        }
        const auto sessionHold = session;
        session->Initialize(
            [self = shared_from_this(), epoch, cancel = std::move(cancel),
             session = sessionHold](bool initialized) mutable {
                if (!self->IsActive(epoch)) return;
                if (!initialized) {
                    self->Complete(epoch,
                        std::unexpected(Devices::ProbeError::Transport));
                    return;
                }
                session->ProbePlug0(
                    [self, epoch, cancel = std::move(cancel), session](
                        const Protocols::AVC::Probe::DeviceModel& observedModel) mutable {
                        if (!self->IsActive(epoch)) return;
                        auto route = self->CurrentRoute(epoch);
                        auto transport = session->Transport();
                        const auto observedRates = observedModel.CommonDuplexRatesHz();
                        if (!route || !transport || observedRates.empty()) {
                            self->Complete(epoch,
                                std::unexpected(Devices::ProbeError::InvalidEvidence));
                            return;
                        }
                        if (self->family_ == FamilyId::GenericAvc &&
                            !session->HasAudioSubunit() &&
                            !session->HasMusicSubunit()) {
                            self->Complete(epoch,
                                std::unexpected(Devices::ProbeError::InvalidEvidence));
                            return;
                        }

                        auto protocol = self->CreateAvcProtocol(
                            *route, transport.get(), observedModel);
                        if (!protocol) {
                            self->Complete(epoch,
                                std::unexpected(Devices::ProbeError::InvalidEvidence));
                            return;
                        }
                        protocol->UpdateRuntimeContext(*route, transport.get());
                        const IOReturn initializeStatus = protocol->Initialize();
                        if (initializeStatus != kIOReturnSuccess) {
                            self->Complete(epoch,
                                std::unexpected(MapProbeStatus(initializeStatus)));
                            return;
                        }
                        AudioStreamRuntimeCaps caps{};
                        std::vector<uint32_t> implementedRates;
                        if (!protocol->GetRuntimeAudioStreamCaps(caps) ||
                            !HasUsableDuplexGeometry(caps) ||
                            !protocol->GetSupportedSampleRates(implementedRates)) {
                            (void)protocol->Shutdown();
                            self->Complete(epoch,
                                std::unexpected(Devices::ProbeError::InvalidEvidence));
                            return;
                        }
                        auto rates = IntersectRates(observedRates, implementedRates);
                        if (rates.empty()) {
                            (void)protocol->Shutdown();
                            self->Complete(epoch,
                                std::unexpected(Devices::ProbeError::InvalidEvidence));
                            return;
                        }
                        self->InstallProtocol(protocol, cancel);
                        const uint32_t policy = static_cast<uint32_t>(
                            self->context_.staticPlan.probePolicy);
                        if (self->family_ == FamilyId::GenericAvc) {
                            self->Complete(epoch, Devices::GenericAvcProbeFacts{
                                .streams = caps,
                                .supportedRates = std::move(rates),
                                .hasAudioSubunit = session->HasAudioSubunit(),
                                .hasMusicSubunit = session->HasMusicSubunit(),
                            });
                        } else if (self->family_ == FamilyId::BeBoB) {
                            self->Complete(epoch, Devices::BeBoBProbeFacts{
                                .streams = caps,
                                .supportedRates = std::move(rates),
                                .operationPolicyId = policy,
                            });
                        } else {
                            self->Complete(epoch, Devices::OxfwProbeFacts{
                                .streams = caps,
                                .supportedRates = std::move(rates),
                                .operationPolicyId = policy,
                            });
                        }
                    });
            });
    }

    FamilyId family_{FamilyId::None};
    ExistingFamilyProviderDependencies dependencies_;
    Devices::AudioFamilyProviderContext context_;
    mutable IOLock* lock_{nullptr};
    bool shuttingDown_{false};
    uint64_t probeEpoch_{0};
    Scheduling::TimerToken kickoffTimer_{Scheduling::kInvalidTimerToken};
    Devices::IAudioDeviceAdapter::ProbeCompletion pendingCompletion_;
    std::shared_ptr<std::atomic<bool>> activeCancel_;
    std::shared_ptr<std::atomic<bool>> protocolCancel_;
    std::shared_ptr<Protocols::AVC::RemoteAvcSession> avcSession_;
    std::shared_ptr<IDeviceProtocol> protocol_;
};

class ExistingFamilyAdapter final : public Devices::IAudioDeviceAdapter {
public:
    ExistingFamilyAdapter(FamilyId family,
                          ExistingFamilyProviderDependencies dependencies,
                          Devices::AudioFamilyProviderContext context)
        : state_(std::make_shared<AdapterState>(family, dependencies,
                                                std::move(context))) {}

    ~ExistingFamilyAdapter() override {
        if (state_) (void)state_->Shutdown();
    }

    [[nodiscard]] bool Probe(ProbeCompletion completion) noexcept override {
        return state_ && state_->Probe(std::move(completion));
    }

    void CancelProbe() noexcept override {
        if (state_) state_->CancelProbe();
    }

    [[nodiscard]] IOReturn Shutdown() noexcept override {
        return state_ ? state_->Shutdown() : kIOReturnSuccess;
    }

    [[nodiscard]] IDuplexDeviceControl* DuplexControl() noexcept override {
        return state_ ? state_->DuplexControl() : nullptr;
    }

    [[nodiscard]] std::shared_ptr<IDeviceProtocol>
    ProtocolHold() noexcept override {
        return state_ ? state_->ProtocolHold() : nullptr;
    }

    [[nodiscard]] std::vector<std::shared_ptr<Devices::IAudioDeviceFacet>>
    Facets() const override {
        return {};
    }

private:
    std::shared_ptr<AdapterState> state_;
};

class ExistingFamilyProvider final : public Devices::IAudioFamilyProvider {
public:
    ExistingFamilyProvider(FamilyId id,
                           ExistingFamilyProviderDependencies dependencies)
        : id_(id), dependencies_(dependencies) {}

    [[nodiscard]] FamilyId Id() const noexcept override { return id_; }

    [[nodiscard]] std::unique_ptr<Devices::IAudioDeviceAdapter>
    CreateAdapter(const Devices::AudioFamilyProviderContext& context) noexcept override {
        if (context.staticPlan.family != id_ || context.staticPlan.unit != context.unit ||
            !PolicyMatchesFamily(id_, context.staticPlan.probePolicy)) {
            return nullptr;
        }
        return std::make_unique<ExistingFamilyAdapter>(id_, dependencies_, context);
    }

private:
    FamilyId id_{FamilyId::None};
    ExistingFamilyProviderDependencies dependencies_;
};

} // namespace

std::unique_ptr<Devices::IAudioFamilyProvider>
MakeGenericAvcFamilyProvider(ExistingFamilyProviderDependencies dependencies) {
    return std::make_unique<ExistingFamilyProvider>(FamilyId::GenericAvc,
                                                     dependencies);
}

std::unique_ptr<Devices::IAudioFamilyProvider>
MakeBeBoBFamilyProvider(ExistingFamilyProviderDependencies dependencies) {
    return std::make_unique<ExistingFamilyProvider>(FamilyId::BeBoB,
                                                     dependencies);
}

std::unique_ptr<Devices::IAudioFamilyProvider>
MakeDiceFamilyProvider(ExistingFamilyProviderDependencies dependencies) {
    return std::make_unique<ExistingFamilyProvider>(FamilyId::DICE,
                                                     dependencies);
}

std::unique_ptr<Devices::IAudioFamilyProvider>
MakeOxfwFamilyProvider(ExistingFamilyProviderDependencies dependencies) {
    return std::make_unique<ExistingFamilyProvider>(FamilyId::OXFW,
                                                     dependencies);
}

} // namespace ASFW::Audio::Families
