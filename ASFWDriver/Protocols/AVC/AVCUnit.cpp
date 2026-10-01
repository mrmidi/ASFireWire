//
// AVCUnit.cpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Unit implementation
//

#include "AVCUnit.hpp"
#include "Graph/AvcGraphBuilder.hpp"
#include "Graph/AvcStreamGeometry.hpp"
#include <algorithm>
#include "../../Common/CallbackUtils.hpp"
#include "../../Logging/Logging.hpp"
#include "Descriptors/DescriptorAccessor.hpp"
#include "Commands/GeneralCommands.hpp"
#include "Commands/SignalSourceCommand.hpp"
#include "Commands/StreamFormatCommand.hpp"
#include "Core/RateCodes.hpp"

using namespace ASFW::Protocols::AVC;

//==============================================================================
// Constructor / Destructor
//==============================================================================

AVCUnit::AVCUnit(std::shared_ptr<Discovery::FWDevice> device,
                 std::shared_ptr<Discovery::FWUnit> unit,
                 Discovery::DeviceRegistry& routeRegistry,
                 Protocols::Ports::FireWireBusOps& busOps,
                 Protocols::Ports::FireWireBusInfo& busInfo,
                 Scheduling::ITimerScheduler& timerScheduler,
                 DiscoveryOptions options)
    : device_(device),
      unit_(unit),
      routeRegistry_(routeRegistry),
      busOps_(busOps),
      busInfo_(busInfo),
      timerScheduler_(timerScheduler),
      options_(options) {

    // Check for custom FCP addresses in Config ROM (optional)
    // For now, use standard addresses
    FCPTransportConfig config;
    config.commandAddress = kFCPCommandAddress;
    config.responseAddress = kFCPResponseAddress;
    config.timeoutMs = kFCPTimeoutInitial;
    config.interimTimeoutMs = kFCPTimeoutAfterInterim;
    config.maxRetries = kFCPMaxRetries;
    config.allowBusResetRetry = false;  // Default: generation-locked

    // The allowlist for firmware that hangs on unimplemented AV/C. Empty for
    // every ordinary device, which is unrestricted. Decided from Config ROM
    // before this transport exists, so the very first frame is already bounded.
    config.permittedFrames =
        PermittedFramesFor(device ? device->GetAvcCommandFilter()
                                  : Discovery::AvcCommandFilterId::Unrestricted);

    // Create FCP transport
    fcpTransport_ = std::make_shared<FCPTransport>();
    if (fcpTransport_) {
        if (!fcpTransport_->init(&busOps_, &busInfo_, device.get(), routeRegistry_, timerScheduler_, config)) {
            ASFW_LOG_ERROR(AVC, "AVCUnit: Failed to initialize FCPTransport");
            fcpTransport_.reset();
            return;
        }

        // Create DescriptorAccessor for unit-level descriptors (Phase 5)
        descriptorAccessor_ = std::make_shared<DescriptorAccessor>(*this, kAVCSubunitUnit);

        if (!descriptorAccessor_) {
            ASFW_LOG_ERROR(Discovery, "AVCUnit: Failed to allocate DescriptorAccessor");
        }
    } else {
        ASFW_LOG_V1(AVC, "AVCUnit: Failed to allocate FCPTransport");
    }

    ASFW_LOG_V1(AVC,
                "AVCUnit: Created for device GUID=%llx, specID=0x%06x",
                GetGUID(), GetSpecID());
}

void AVCUnit::Submit(const ASFW::AVC::CommandFrame& frame,
                     FW::Generation generation,
                     ResponseCallback completion) {
    if (fcpTransport_) {
        fcpTransport_->Submit(frame, generation, std::move(completion));
    } else {
        completion(std::unexpected(ASFW::AVC::AvcError::Of(ASFW::AVC::AvcErrorKind::kTransportError)));
    }
}

ASFW::FW::NodeId AVCUnit::NodeId() const noexcept {
    return fcpTransport_ ? fcpTransport_->NodeId() : ASFW::FW::NodeId{0};
}

ASFW::FW::Generation AVCUnit::CurrentGeneration() const noexcept {
    return fcpTransport_ ? fcpTransport_->CurrentGeneration() : ASFW::FW::Generation{0};
}

uint64_t AVCUnit::Guid() const noexcept {
    return fcpTransport_ ? fcpTransport_->Guid() : GetGUID();
}

void AVCUnit::ProbeUnitInfo(std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    ASFW::AVC::Cmd::UnitInfoCommand cmd{};
    Status(cmd, [this, completionState](ASFW::AVC::Expected<ASFW::AVC::Cmd::UnitInfo> info) {
        if (!info) {
            ASFW_LOG_V1(AVC, "AVCUnit: UNIT_INFO failed");
            Common::InvokeSharedCallback(completionState, false);
            return;
        }
        model_.info = *info;
        ASFW_LOG_V2(AVC, "AVCUnit: UNIT_INFO succeeded: type=0x%02x id=%u company=0x%06x",
                    static_cast<uint8_t>(info->unitType), info->unitId,
                    ASFW::AVC::ToOui(info->companyId));
        Common::InvokeSharedCallback(completionState, true);
    });
}

AVCUnit::~AVCUnit() {
    Shutdown();
    ASFW_LOG_V1(AVC, "AVCUnit: Destroyed (GUID=%llx)", GetGUID());
}

void AVCUnit::Shutdown() {
    initialized_ = false;
    if (fcpTransport_) {
        fcpTransport_->Shutdown();
    }
}

//==============================================================================
// Initialization
//==============================================================================

void AVCUnit::Initialize(std::function<void(bool)> completion) {
    if (!TryBeginRescan()) {
        if (completion) completion(false);
        return;
    }
    InitializeAlreadyBegun(std::move(completion));
}

void AVCUnit::InitializeAlreadyBegun(std::function<void(bool)> completion) {
    auto finish = Common::ShareCallback(
        [this, completion = std::move(completion)](bool success) mutable {
            discoveryStatus_.store(success ? AVCDiscoveryStatus::Completed : AVCDiscoveryStatus::Failed,
                                   std::memory_order_release);
            rescanInProgress_.store(false, std::memory_order_release);
            if (completion) completion(success);
        });
    // The extension inventory reads more of the same device whatever the
    // generic result, so a partial unit still reports everything it answers.
    auto completionState = Common::ShareCallback([this, finish](bool success) {
        if (!options_.extensionInventory) {
            Common::InvokeSharedCallback(finish, success);
            return;
        }
        options_.extensionInventory(*this, [finish, success] { Common::InvokeSharedCallback(finish, success); });
    });
    if (initialized_) {
        ASFW_LOG_V2(AVC, "AVCUnit: Already initialized");
        Common::InvokeSharedCallback(completionState, true);
        return;
    }

    model_.identity = Identity();
    ASFW_LOG_V1(AVC, "AVCUnit: Initializing...");

    ProbeDescriptorMechanism([this, completionState](bool descriptorOk) {
        ProbeSignalFormat([this, completionState](bool signalFormatOk) {
            ProbeUnitInfo([this, completionState](bool unitOk) {
                if (!unitOk) {
                    // UNIT_INFO is an optional AV/C discovery hint, not a prerequisite
                    // for the independent SUBUNIT_INFO and PLUG_INFO probes below.
                    // TerraTec PHASE 88 Rack FW acknowledges the FCP request but does
                    // not return an FCP response for this opcode (FireBug capture,
                    // 2026-07-16). Continue so its BridgeCo-specific probe can run.
                    ASFW_LOG_V1(AVC,
                                "AVCUnit: UNIT_INFO unavailable; continuing with subunit/plug discovery");
                }

            ProbeSubunits([this, completionState](bool subunitOk) {
                if (!subunitOk) {
                    ASFW_LOG_V1(AVC, "AVCUnit: Subunit probe failed");
                    Common::InvokeSharedCallback(completionState, false);
                    return;
                }

                ProbePlugs([this, completionState](bool plugsOk) {
                    initialized_ = plugsOk;

                    if (plugsOk) { // NOSONAR(cpp:S3923): branches log different diagnostic messages
                        ASFW_LOG_V1(AVC,
                                   "AVCUnit: Initialized - "
                                   "%zu subunits, %u/%u ISO plugs, "
                                   "descriptor support: %{public}s",
                                   subunits_.size(),
                                   model_.unitPlugs.isochronousInputs,
                                   model_.unitPlugs.isochronousOutputs,
                                   descriptorInfo_.descriptorMechanismSupported ?
                                       "YES" : "NO");
                    } else {
                        ASFW_LOG_V1(AVC, "AVCUnit: Plug probe failed");
                    }

                    Common::InvokeSharedCallback(completionState, plugsOk);
                });
            });
        });
    });
    });
}

void AVCUnit::ReScan(std::function<void(bool)> completion) {
    ASFW_LOG_V1(AVC, "AVCUnit: Re-scan requested (GUID=%llx)", GetGUID());
    if (!TryBeginRescan()) {
        if (completion) completion(false);
        return;
    }
    ReScanAlreadyBegun(std::move(completion));
}

void AVCUnit::ReScanAlreadyBegun(std::function<void(bool)> completion) {
    
    // Reset state
    initialized_ = false;
    subunits_.clear();
    model_ = {};
    discoveredGraph_.reset();
    descriptorInfo_ = {};
    model_.identity = Identity();
    
    // Re-initialize
    InitializeAlreadyBegun(std::move(completion));
}

//==============================================================================
// Subunit Probing
//==============================================================================

#include "Music/MusicSubunit.hpp"
#include "Camera/CameraSubunit.hpp"
#include "Audio/AudioSubunit.hpp"

//==============================================================================
// Subunit Probing
//==============================================================================

void AVCUnit::ProbeSubunits(std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    ASFW::AVC::Cmd::SubunitInfoCommand cmd{
        .operands = ASFW::AVC::Cmd::SubunitInfoOperands{.page = 0, .extensionCode = 0x07}
    };

    Status(cmd, [this, completionState](ASFW::AVC::Expected<ASFW::AVC::Cmd::SubunitInfo> info) {
        if (!info) {
            ASFW_LOG_V1(AVC, "AVCUnit: SUBUNIT_INFO failed");
            Common::InvokeSharedCallback(completionState, false);
            return;
        }

        // Store subunit info
        StoreSubunitInfo(*info);

        ASFW_LOG_V1(AVC, "AVCUnit: Found %zu subunits", subunits_.size());

        // Now parse capabilities for each subunit
        ParseSubunitCapabilities(0, *completionState);
    });
}

void AVCUnit::StoreSubunitInfo(const ASFW::AVC::Cmd::SubunitInfo& info) {
    subunits_.clear();
    model_.subunits.clear();

    for (uint8_t i = 0; i < info.entryCount; ++i) {
        const auto& entry = info.entries[i];
        for (uint8_t id = 0; id <= entry.maximumId; ++id) {
            model_.subunits.push_back(ASFW::AVC::SubunitModel{
                .id = ASFW::AVC::SubunitId{entry.type, id},
                .plugs = {},
            });

            std::shared_ptr<Subunit> subunit;
            auto legacyType = static_cast<AVCSubunitType>(entry.type);

            // Factory logic
            if (entry.type == ASFW::AVC::SubunitType::kMusic) {
                subunit = std::make_shared<Music::MusicSubunit>(legacyType, id);
            } else if (entry.type == ASFW::AVC::SubunitType::kCamera) {
                subunit = std::make_shared<Camera::CameraSubunit>(legacyType, id);
            } else if (entry.type == ASFW::AVC::SubunitType::kAudio) {
                // Subunit existence is independent of Apple's device-matching
                // preference. Keep both runtime objects for mixed units.
                subunit = std::make_shared<Audio::AudioSubunit>(legacyType, id);
            } else {
                class GenericSubunit : public Subunit {
                public:
                    GenericSubunit(AVCSubunitType type, uint8_t id) : Subunit(type, id) {}
                    std::string GetName() const override { return "Generic"; }
                };
                subunit = std::make_shared<GenericSubunit>(legacyType, id);
            }

            if (subunit) {
                subunits_.push_back(subunit);
                ASFW_LOG_V2(AVC, "AVCUnit: Subunit %zu: type=0x%02x, id=%d (%{public}s)",
                            subunits_.size() - 1, static_cast<uint8_t>(entry.type), id, subunit->GetName().c_str());
            }
        }
    }
}


void AVCUnit::ParseSubunitCapabilities(size_t index, std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    if (index >= subunits_.size()) {
        PopulateKnownSubunitPlugCounts();
        ResolveDiscoveredGraph([completionState](bool) {
            Common::InvokeSharedCallback(completionState, true);
        });
        return;
    }

    auto subunit = subunits_[index];
    subunit->ParseCapabilities(*this, [this, subunit, index, completionState](bool success) {
        if (success && subunit->GetType() == AVCSubunitType::kAudio) {
            const ASFW::AVC::SubunitId id{ASFW::AVC::SubunitType::kAudio, subunit->GetID()};
            const auto model = std::find_if(model_.subunits.begin(), model_.subunits.end(),
                [&id](const auto& item) { return item.id == id; });
            if (model != model_.subunits.end()) model->plugsDiscovered = true;
        }
        if (!success) {
            ASFW_LOG_V2(AVC, "AVCUnit: Failed to parse capabilities for subunit %zu", index);
            // Continue anyway? Yes, partial success is better than failure.
        }
        // Next
        ParseSubunitCapabilities(index + 1, *completionState);
    });
}


//==============================================================================
// Plug Probing
//==============================================================================

void AVCUnit::PopulateKnownSubunitPlugCounts() {
    for (const auto& subunit : subunits_) {
        ASFW::AVC::SubunitId id{
            .type = static_cast<ASFW::AVC::SubunitType>(subunit->GetType()),
            .id = subunit->GetID(),
        };
        const auto model = std::find_if(model_.subunits.begin(), model_.subunits.end(),
            [&id](const auto& item) { return item.id == id; });
        if (model == model_.subunits.end()) continue;

        if (subunit->GetType() == AVCSubunitType::kMusic) {
            const auto* typed = static_cast<const Music::MusicSubunit*>(subunit.get());
            const auto status = typed->GetParsedStatus();
            if (!status) continue;
            const uint8_t destinations = static_cast<uint8_t>(std::count_if(
                status->plugs.begin(), status->plugs.end(), [](const auto& plug) { return plug.isDestination; }));
            const uint8_t sources = static_cast<uint8_t>(status->plugs.size() - destinations);
            model->plugs = {destinations, sources};
            model->plugsDiscovered = true;
            subunit->SetPlugCounts(Subunit::PlugCounts{destinations, sources});
        } else if (subunit->GetType() == AVCSubunitType::kAudio && model->plugsDiscovered) {
            // ParseCapabilities reports success only after its existing PLUG_INFO
            // query completes. The cached values are read below from AudioSubunit.
            const auto* typed = static_cast<const Audio::AudioSubunit*>(subunit.get());
            const ASFW::AVC::Cmd::SubunitPlugCounts counts{
                typed->GetNumInputPlugs(), typed->GetNumOutputPlugs()};
            model->plugs = counts;
            model->plugsDiscovered = true;
            subunit->SetPlugCounts(Subunit::PlugCounts{counts.destinationPlugs, counts.sourcePlugs});
        }
        // Camera and generic subunits remain explicitly unknown until their
        // capability paths provide counts; do not add speculative wire probes.
    }
}

void AVCUnit::ResolveDiscoveredGraph(std::function<void(bool)> completion) {
    auto done = Common::ShareCallback(std::move(completion));
    std::shared_ptr<Music::MusicSubunit> music;
    const Audio::AudioSubunit* audio = nullptr;
    for (const auto& subunit : subunits_) {
        if (subunit->GetType() == AVCSubunitType::kMusic && !music) {
            music = std::static_pointer_cast<Music::MusicSubunit>(subunit);
        } else if (subunit->GetType() == AVCSubunitType::kAudio && !audio) {
            audio = static_cast<const Audio::AudioSubunit*>(subunit.get());
        }
    }
    if (!music || !music->GetParsedStatus()) {
        if (music || audio) {
            ResolveUnitStreamGraph([done](bool success) { Common::InvokeSharedCallback(done, success); });
            return;
        }
        discoveredGraph_.reset();
        ASFW_LOG_WARNING(AVC, "[AvcGraph] guid=%llx unavailable reason=music-descriptor", Guid());
        Common::InvokeSharedCallback(done, false);
        return;
    }
    Graph::GraphBuildOptions options;
    options.allowDefaultPlugSelection = false;
    if (audio) options.audioSubunitId = audio->GetID();
    if (auto device = device_.lock()) options.modelName = std::string(device->GetModelName());
    for (const auto& plug : music->GetPlugs()) {
        if (plug.IsInput() && plug.connectionInfo && plug.connectionInfo->IsUnitConnection() &&
            !plug.connectionInfo->sourceIsExternalUnitPlug &&
            plug.connectionInfo->sourcePlugNumber == 0) {
            if (options.playbackSubunitDestPlugId) {
                ASFW_LOG_WARNING(AVC, "[AvcGraph] guid=%llx unavailable reason=ambiguous-playback-route", Guid());
                discoveredGraph_.reset();
                Common::InvokeSharedCallback(done, false);
                return;
            }
            options.playbackSubunitDestPlugId = plug.plugID;
        }
    }
    const auto identifier = audio ? audio->GetIdentifier() : std::nullopt;
    const auto generation = CurrentGeneration();
    // Capture is the source feeding unit ISO output 0, not source plug 0 by convention.
    // Cross-validated: FFADO libavc/ccm/avc_signal_source.cpp:45-95;
    // docs/avc-rebuild/fixtures/graph_build.py:133-139.
    ASFW::AVC::Cmd::SignalSourceCommand command{
        .address = ASFW::AVC::SubunitAddress::Unit(),
        .operands = {.destination = ASFW::AVC::Cmd::SignalAddress::UnitIsochronousPlug(0)}};
    Status(command, [this, music, identifier, options = std::move(options), generation, done]
        (ASFW::AVC::Expected<ASFW::AVC::Cmd::SignalSource> reply) mutable {
        if (CurrentGeneration() != generation) {
            Common::InvokeSharedCallback(done, false);
            return;
        }
        const auto destination = ASFW::AVC::Cmd::SignalAddress::UnitIsochronousPlug(0);
        const auto sourceAddress = ASFW::AVC::SubunitAddress::FromByte(
            static_cast<uint8_t>((static_cast<uint8_t>(AVCSubunitType::kMusic) << 3) | music->GetID()));
        if (reply && reply->destination == destination && !reply->source.IsUnit() &&
            reply->source.Subunit() == sourceAddress) {
            options.captureSubunitSourcePlugId = reply->source.PlugId();
        }
        auto graph = Graph::AvcGraphBuilder::BuildGraph(*music->GetParsedStatus(),
            identifier ? &*identifier : nullptr, options);
        auto completeStream = [&](Graph::StreamGraph& stream) {
            if (stream.selectionEvidence == Graph::StreamSelectionEvidence::kUnresolved) return;
            const auto found = std::find_if(music->GetPlugs().begin(), music->GetPlugs().end(),
                [&stream](const auto& plug) { return plug.plugID == stream.subunitPlugId &&
                    plug.IsInput() == stream.isDestination; });
            if (found == music->GetPlugs().end() || !found->currentFormat) return;
            const auto formation = ASFW::AVC::Cmd::DecodeStreamFormatBlock(found->currentFormat->rawFormatBlock);
            if (!formation || formation->kind != ASFW::AVC::Cmd::StreamFormat::Kind::kCompoundAm824 ||
                !formation->compound.OnlyPcmAndMidi() || formation->compound.PcmChannels() != stream.channelCount) return;
            const auto& compound = formation->compound;
            const auto rate = ASFW::AVC::ToHz(compound.rate);
            if (!rate) return;
            stream.dataBlockSize = compound.PcmChannels() + compound.MidiChannels();
            stream.currentSampleRate = *rate;
            const auto* descriptor = music->GetParsedStatus()->FindPlug(stream.subunitPlugId, stream.isDestination);
            auto validated = Graph::AvcGraphBuilder::BuildStreamGraph(*descriptor, *music->GetParsedStatus(), stream.dataBlockSize);
            stream.slotMap = validated.slotMap;
            stream.slotMapValidation = validated.slotMapValidation;
            stream.usingFallbackMap = validated.usingFallbackMap;
            for (const auto& format : found->supportedFormats) {
                const auto parsed = ASFW::AVC::Cmd::DecodeStreamFormatBlock(format.rawFormatBlock);
                if (!parsed || parsed->kind != ASFW::AVC::Cmd::StreamFormat::Kind::kCompoundAm824 ||
                    !parsed->compound.OnlyPcmAndMidi() || parsed->compound.PcmChannels() != stream.channelCount ||
                    parsed->compound.MidiChannels() != compound.MidiChannels()) continue;
                if (auto hz = ASFW::AVC::ToHz(parsed->compound.rate); hz &&
                    std::find(stream.supportedSampleRates.begin(), stream.supportedSampleRates.end(), *hz) == stream.supportedSampleRates.end())
                    stream.supportedSampleRates.push_back(*hz);
            }
            if (stream.supportedSampleRates.empty()) stream.supportedSampleRates.push_back(*rate);
        };
        completeStream(graph.playback);
        completeStream(graph.capture);
        ASFW_LOG(AVC, "[AvcGraph] guid=%llx gen=%u playback=plug%u pcm=%u dbs=%u capture=plug%u pcm=%u dbs=%u controls=%zu",
            Guid(), generation.value, graph.playback.subunitPlugId, graph.playback.channelCount,
            graph.playback.dataBlockSize, graph.capture.subunitPlugId, graph.capture.channelCount,
            graph.capture.dataBlockSize, graph.controls.size());
        for (const auto* stream : {&graph.playback, &graph.capture}) {
            ASFW_LOG(AVC, "[AvcGraphStream] guid=%llx direction=%{public}s selected=%u plug=%u rate=%u pcm=%u dbs=%u map=%u rates=%zu",
                Guid(), stream->isDestination ? "playback" : "capture",
                static_cast<unsigned>(stream->selectionEvidence), stream->subunitPlugId,
                stream->currentSampleRate, stream->channelCount, stream->dataBlockSize,
                static_cast<unsigned>(stream->slotMapValidation), stream->supportedSampleRates.size());
        }
        discoveredGraph_ = std::make_shared<Graph::DeviceGraph>(std::move(graph));
        if (discoveredGraph_->playback.dataBlockSize == 0 || discoveredGraph_->capture.dataBlockSize == 0) {
            ResolveUnitStreamGraph([done](bool success) { Common::InvokeSharedCallback(done, success); });
            return;
        }
        Common::InvokeSharedCallback(done, true);
    });
}

void AVCUnit::ResolveUnitStreamGraph(std::function<void(bool)> completion) {
    auto done = Common::ShareCallback(std::move(completion));
    const auto generation = CurrentGeneration();
    // Read the current formats without changing clock or routing. Cross-validated
    // with Linux sound/firewire/oxfw/oxfw-stream.c:637-644 (format SINGLE).
    const auto command = [](ASFW::AVC::Cmd::PlugDirection direction) {
        return ASFW::AVC::Cmd::StreamFormatCommand{
            .operands = {.form = ASFW::AVC::Cmd::StreamFormatSubfunction::kSingle,
                .opcode = ASFW::AVC::Cmd::StreamFormatOpcode::kExtendedStreamFormat,
                .plug = ASFW::AVC::Cmd::PlugAddress::UnitPlug(direction,
                    ASFW::AVC::Cmd::UnitPlugType::kPcr, 0)}};
    };
    Status(command(ASFW::AVC::Cmd::PlugDirection::kInput),
        [this, generation, command, done](ASFW::AVC::Expected<ASFW::AVC::Cmd::StreamFormatReply> playback) {
        if (!playback || CurrentGeneration() != generation) {
            Common::InvokeSharedCallback(done, false);
            return;
        }
        auto playbackStream = Graph::BuildUnitStreamGeometry(playback->format, true);
        if (!playbackStream) { Common::InvokeSharedCallback(done, false); return; }
        Status(command(ASFW::AVC::Cmd::PlugDirection::kOutput),
            [this, generation, playbackStream = std::move(*playbackStream), done]
            (ASFW::AVC::Expected<ASFW::AVC::Cmd::StreamFormatReply> capture) mutable {
            auto captureStream = capture ? Graph::BuildUnitStreamGeometry(capture->format, false) : std::nullopt;
            if (!captureStream || CurrentGeneration() != generation ||
                playbackStream.currentSampleRate != captureStream->currentSampleRate) {
                Common::InvokeSharedCallback(done, false);
                return;
            }
            // Streams the descriptors selected keep their names and slot order;
            // without a selection the plug formats are the whole graph.
            if (discoveredGraph_) {
                auto completed = *discoveredGraph_;
                const auto midi = [](const Graph::StreamGraph& s) { return s.dataBlockSize - s.channelCount; };
                if (CompleteStream(completed.playback, playbackStream.channelCount, midi(playbackStream),
                                   playbackStream.currentSampleRate, playbackStream.supportedSampleRates) &&
                    CompleteStream(completed.capture, captureStream->channelCount, midi(*captureStream),
                                   captureStream->currentSampleRate, captureStream->supportedSampleRates)) {
                    discoveredGraph_ = std::make_shared<Graph::DeviceGraph>(std::move(completed));
                    ASFW_LOG(AVC, "[AvcGeometry] guid=%llx source=unit-plug0 completes=descriptor-graph rate=%u",
                             Guid(), discoveredGraph_->playback.currentSampleRate);
                    Common::InvokeSharedCallback(done, true);
                    return;
                }
            }
            Graph::DeviceGraph graph;
            graph.playback = std::move(playbackStream);
            graph.capture = std::move(*captureStream);
            discoveredGraph_ = std::make_shared<Graph::DeviceGraph>(std::move(graph));
            ASFW_LOG(AVC, "[AvcGeometry] guid=%llx source=unit-plug0 rate=%u playback=%u/%u capture=%u/%u",
                Guid(), discoveredGraph_->playback.currentSampleRate,
                discoveredGraph_->playback.channelCount, discoveredGraph_->playback.dataBlockSize,
                discoveredGraph_->capture.channelCount, discoveredGraph_->capture.dataBlockSize);
            Common::InvokeSharedCallback(done, true);
        });
    });
}

bool AVCUnit::CompleteStream(Graph::StreamGraph& stream, uint32_t pcmChannels, uint32_t midiChannels,
                             uint32_t rateHz, std::vector<uint32_t> rates) const {
    if (stream.selectionEvidence == Graph::StreamSelectionEvidence::kUnresolved ||
        stream.channelCount != pcmChannels || pcmChannels == 0 || rateHz == 0) {
        return false;
    }
    const auto music = std::find_if(subunits_.begin(), subunits_.end(),
        [](const auto& subunit) { return subunit->GetType() == AVCSubunitType::kMusic; });
    if (music == subunits_.end()) return false;
    const auto& status = static_cast<const Music::MusicSubunit&>(**music).GetParsedStatus();
    if (!status) return false;
    const auto* descriptor = status->FindPlug(stream.subunitPlugId, stream.isDestination);
    if (descriptor == nullptr) return false;
    stream.dataBlockSize = pcmChannels + midiChannels;
    stream.midiStreamCount = midiChannels;
    stream.currentSampleRate = rateHz;
    stream.supportedSampleRates = rates.empty() ? std::vector<uint32_t>{rateHz} : std::move(rates);
    const auto validated = Graph::AvcGraphBuilder::BuildStreamGraph(*descriptor, *status, stream.dataBlockSize);
    stream.slotMap = validated.slotMap;
    stream.slotMapValidation = validated.slotMapValidation;
    stream.usingFallbackMap = validated.usingFallbackMap;
    return true;
}

void AVCUnit::CompleteGraphFromUnitPlugFormations(std::span<const UnitPlugFormation> playback,
                                                  std::span<const UnitPlugFormation> capture,
                                                  uint32_t currentRateHz) {
    if (!discoveredGraph_ || currentRateHz == 0) return;
    auto graph = *discoveredGraph_;
    // The unit's live formations are current by definition; a size from the
    // music subunit may not be (the Phase 88 descriptor is identical at every
    // rate). Keep the existing size only when no live formation matches.
    const auto complete = [&](Graph::StreamGraph& stream, std::span<const UnitPlugFormation> formations) {
        const auto current = std::find_if(formations.begin(), formations.end(), [&](const auto& f) {
            return f.rateHz == currentRateHz && f.pcmChannels == stream.channelCount;
        });
        if (current == formations.end()) return stream.dataBlockSize != 0;
        std::vector<uint32_t> rates;
        for (const auto& f : formations) {
            if (f.pcmChannels == current->pcmChannels && f.midiChannels == current->midiChannels &&
                std::find(rates.begin(), rates.end(), f.rateHz) == rates.end()) {
                rates.push_back(f.rateHz);
            }
        }
        return CompleteStream(stream, current->pcmChannels, current->midiChannels, currentRateHz, std::move(rates));
    };
    const bool playbackOk = complete(graph.playback, playback);
    const bool captureOk = complete(graph.capture, capture);
    ASFW_LOG(AVC, "[AvcGeometry] guid=%llx source=unit-plug-formations rate=%u playback=%{public}s capture=%{public}s",
             Guid(), currentRateHz, playbackOk ? "sized" : "unsized", captureOk ? "sized" : "unsized");
    discoveredGraph_ = std::make_shared<Graph::DeviceGraph>(std::move(graph));
}

void AVCUnit::ProbePlugs(std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    ASFW::AVC::Cmd::PlugInfoCommand cmd{
        .operands = ASFW::AVC::Cmd::PlugInfoOperands{
            .form = ASFW::AVC::Cmd::PlugInfoForm::kUnitIsoExternal
        }
    };

    Status(cmd, [this, completionState](ASFW::AVC::Expected<ASFW::AVC::Cmd::PlugInfoReply> reply) {
        if (!reply) {
            ASFW_LOG_V1(AVC, "AVCUnit: PLUG_INFO failed");
            Common::InvokeSharedCallback(completionState, false);
            return;
        }

        // Store plug info
        model_.unitPlugs = reply->unit;

        ASFW_LOG_V2(AVC,
                    "AVCUnit: Unit plugs: %u iso in, %u iso out, %u ext in, %u ext out",
                    reply->unit.isochronousInputs, reply->unit.isochronousOutputs,
                    reply->unit.externalInputs, reply->unit.externalOutputs);

        Common::InvokeSharedCallback(completionState, true);
    });
}

void AVCUnit::ProbeSignalFormat(std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    ASFW::AVC::Cmd::PlugSignalFormatCommand cmd{
        .operands = ASFW::AVC::Cmd::PlugSignalFormatOperands{
            .direction = ASFW::AVC::Cmd::PlugSignalDirection::kOutput,
            .plugId = 0,
            .format = std::nullopt,
            .query = ASFW::AVC::Cmd::SignalFormatQuery::kAllWildcard,
        }
    };

    Status(cmd, [this, completionState](ASFW::AVC::Expected<ASFW::AVC::Cmd::PlugSignalFormat> fmt) {
        if (fmt) {
            ASFW_LOG_INFO(Discovery, "Received Signal Format: Format=0x%02x, RateCode=0x%02x",
                          fmt->fmt, fmt->fdf[0]);

            if (fmt->fmt == ASFW::AVC::Cmd::kFmtAm824) {
                ASFW_LOG_INFO(Discovery, "Detected Apogee AM824 Format (0x90).");
                auto sfc = ASFW::AVC::Cmd::SfcOf(*fmt);
                if (sfc.has_value()) {
                    auto freqHz = ASFW::AVC::ToHz(*sfc);
                    if (freqHz.has_value() && *freqHz > 0) {
                        ASFW_LOG_INFO(Discovery, "Device is locked to %u Hz (Code 0x%02x).", *freqHz, fmt->fdf[0]);
                    } else {
                        ASFW_LOG_INFO(Discovery, "Device is locked to Unknown Rate (Code 0x%02x).", fmt->fdf[0]);
                    }
                }
            }
        } else {
            ASFW_LOG_ERROR(Discovery, "Failed to send Signal Format Query");
        }
        // Always continue
        Common::InvokeSharedCallback(completionState, true);
    });
}

bool AVCUnit::ParseUnitIdentifier(const std::vector<uint8_t>& data) {
    // Minimum size check: descriptor_length(2) + generation_ID(1) + 3 size fields = 6
    if (data.size() < 6) {
        ASFW_LOG_V1(AVC, "AVCUnit: Unit Identifier too short (need at least 6 bytes)");
        return false;
    }

    // Parse descriptor_length (bytes 0-1)
    // Note: DescriptorAccessor includes this in the returned data
    uint16_t descriptorLength = (data[0] << 8) | data[1];
    ASFW_LOG_V3(AVC, "AVCUnit: Unit Identifier length = %d bytes", descriptorLength);

    // Validate length matches actual data size
    if (descriptorLength + 2 != data.size()) {
        ASFW_LOG_V2(AVC,
                        "AVCUnit: Descriptor length mismatch (declared=%d, actual=%zu)",
                        descriptorLength, data.size() - 2);
        // Continue anyway - some devices may have padding
    }

    // Parse fields (Section 6.2.1 of TA 2002013)
    descriptorInfo_.generationID = data[2];
    descriptorInfo_.sizeOfListID = data[3];
    descriptorInfo_.sizeOfObjectID = data[4];
    descriptorInfo_.sizeOfEntryPosition = data[5];

    // Validate sizes are reasonable (spec says 0-8 bytes typical)
    if (descriptorInfo_.sizeOfListID > 8 ||
        descriptorInfo_.sizeOfObjectID > 8 ||
        descriptorInfo_.sizeOfEntryPosition > 8) {
        ASFW_LOG_V1(AVC, "AVCUnit: Suspicious descriptor sizes (one or more > 8 bytes)");
        return false;
    }

    // Parse number_of_root_object_lists (offset 6, 2 bytes)
    if (data.size() < 8) {
        // No root lists section present
        descriptorInfo_.numberOfRootObjectLists = 0;
        descriptorInfo_.rootListIDs.clear();
        return true;
    }

    descriptorInfo_.numberOfRootObjectLists = (data[6] << 8) | data[7];

    // Parse root_list_ID array
    size_t listIdSize = (descriptorInfo_.sizeOfListID > 0) ?
        descriptorInfo_.sizeOfListID : 2;  // Default to 2 bytes if size is 0

    size_t arraySize = descriptorInfo_.numberOfRootObjectLists * listIdSize;
    size_t arrayOffset = 8;

    if (data.size() < arrayOffset + arraySize) {
        ASFW_LOG_V1(AVC, "AVCUnit: Data too short for root_list_ID array");
        return false;
    }

    // Extract root list IDs (MSB first encoding)
    descriptorInfo_.rootListIDs.clear();
    descriptorInfo_.rootListIDs.reserve(descriptorInfo_.numberOfRootObjectLists);

    const uint8_t* arrayPtr = data.data() + arrayOffset;
    for (uint16_t i = 0; i < descriptorInfo_.numberOfRootObjectLists; ++i) {
        uint64_t listId = 0;
        // Read listIdSize bytes in MSB-first order
        for (size_t byteIdx = 0; byteIdx < listIdSize; ++byteIdx) {
            listId = (listId << 8) | arrayPtr[byteIdx];
        }
        descriptorInfo_.rootListIDs.push_back(listId);
        arrayPtr += listIdSize;

        ASFW_LOG_V3(AVC, "AVCUnit: Root list [%d] = 0x%llx", i, listId);
    }

    return true;
}

void AVCUnit::ProbeDescriptorMechanism(std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    ASFW_LOG_V2(AVC, "AVCUnit: Probing descriptor mechanism (Status Descriptor 0x80)...");

    if (!descriptorAccessor_) {
        ASFW_LOG_V2(AVC, "AVCUnit: No DescriptorAccessor, skipping descriptors");
        descriptorInfo_.descriptorMechanismSupported = false;
        Common::InvokeSharedCallback(completionState, true);
        return;
    }

    // Use 0x80 (Status Descriptor) as Apple does for Music Subunits
    auto specifier = DescriptorSpecifier();
    specifier.type = static_cast<DescriptorSpecifierType>(0x80);
    auto self = shared_from_this();

    descriptorAccessor_->readWithOpenCloseSequence(
        specifier,
        [this, self, completionState](const DescriptorAccessor::ReadDescriptorResult& result) {
            if (!result.success) {
                ASFW_LOG_V2(AVC, "AVCUnit: Status Descriptor read failed: %d",
                             static_cast<int>(result.avcResult));
                descriptorInfo_.descriptorMechanismSupported = false;
                Common::InvokeSharedCallback(completionState, true);  // Continue despite failure
                return;
            }

            // Note: The response is a Status Descriptor, not a Unit Identifier.
            // Standard ParseUnitIdentifier won't work here because the format is different.
            // We just mark support as true if we got data.
            // The specific parsing (Info Blocks) is handled by MusicSubunit.
            
            if (!result.data.empty()) {
                descriptorInfo_.descriptorMechanismSupported = true;
                ASFW_LOG_V1(AVC, "AVCUnit: Descriptor mechanism SUPPORTED (Status Descriptor 0x80 read success, %zu bytes)", result.data.size());
            } else {
                descriptorInfo_.descriptorMechanismSupported = false;
            }
            
            // Skip TraverseRootLists for Music Subunits using Status Descriptor model
            Common::InvokeSharedCallback(completionState, true);
        });
}

void AVCUnit::TraverseRootLists(size_t listIndex,
                                std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    if (listIndex >= descriptorInfo_.rootListIDs.size()) {
        // All lists traversed
        ASFW_LOG_V2(AVC,
                     "AVCUnit: Traversed all %zu root object lists",
                     descriptorInfo_.rootListContents.size());
        Common::InvokeSharedCallback(completionState, true);
        return;
    }

    uint64_t listID = descriptorInfo_.rootListIDs[listIndex];
    ASFW_LOG_V3(AVC,
                  "AVCUnit: Traversing root list [%zu]: ID=0x%llx",
                  listIndex, listID);

    auto self = shared_from_this();
    ReadRootObjectList(listID,
        [this, self, listIndex, listID, completionState]
        (bool success, std::vector<uint64_t> objectIDs) {

            if (success) {
                UnitDescriptorInfo::RootListContents contents;
                contents.listID = listID;
                contents.objectIDs = std::move(objectIDs);
                descriptorInfo_.rootListContents.push_back(std::move(contents));

                ASFW_LOG_V3(AVC,
                              "AVCUnit: Root list 0x%llx contains %zu objects",
                              listID,
                              descriptorInfo_.rootListContents.back().objectIDs.size());
            } else {
                ASFW_LOG_V2(AVC,
                                "AVCUnit: Failed to read root list 0x%llx (continuing)",
                                listID);
            }

            // Continue to next list (graceful degradation)
            TraverseRootLists(listIndex + 1, *completionState);
        });
}

void AVCUnit::ReadRootObjectList(
    uint64_t listID,
    std::function<void(bool success, std::vector<uint64_t> objectIDs)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));

    if (!descriptorAccessor_) {
        Common::InvokeSharedCallback(completionState, false, std::vector<uint64_t>{});
        return;
    }

    // Build descriptor specifier for list_ID (type 0x10)
    size_t listIdSize = descriptorInfo_.sizeOfListID > 0 ?
        descriptorInfo_.sizeOfListID : 2;

    std::vector<uint8_t> operands;
    operands.reserve(listIdSize);

    // Encode listID as MSB-first bytes
    for (size_t i = 0; i < listIdSize; ++i) {
        size_t shiftAmount = (listIdSize - 1 - i) * 8;
        operands.push_back(static_cast<uint8_t>((listID >> shiftAmount) & 0xFF));
    }

    auto specifier = DescriptorSpecifier::forListID(operands);
    auto self = shared_from_this();

    descriptorAccessor_->readWithOpenCloseSequence(
        specifier,
        [this, self, listID, completionState]
        (const DescriptorAccessor::ReadDescriptorResult& result) {

            if (!result.success) {
                ASFW_LOG_V2(AVC,
                                "AVCUnit: Failed to read list 0x%llx: result=%d",
                                listID, static_cast<int>(result.avcResult));
                Common::InvokeSharedCallback(completionState, false, std::vector<uint64_t>{});
                return;
            }

            // Parse object list descriptor
            const auto& data = result.data;
            if (data.size() < 4) {
                ASFW_LOG_V1(AVC, "AVCUnit: List descriptor too short");
                Common::InvokeSharedCallback(completionState, false, std::vector<uint64_t>{});
                return;
            }

            uint16_t descriptorLength = (data[0] << 8) | data[1];
            uint16_t numEntries = (data[2] << 8) | data[3];

            ASFW_LOG_V3(AVC,
                          "AVCUnit: List 0x%llx: length=%d, entries=%d",
                          listID, descriptorLength, numEntries);

            // Parse object IDs
            size_t objectIdSize = descriptorInfo_.sizeOfObjectID > 0 ?
                descriptorInfo_.sizeOfObjectID : 2;
            size_t arrayOffset = 4;
            size_t expectedSize = arrayOffset + (numEntries * objectIdSize);

            if (data.size() < expectedSize) {
                ASFW_LOG_V1(AVC, "AVCUnit: List data too short for entries");
                Common::InvokeSharedCallback(completionState, false, std::vector<uint64_t>{});
                return;
            }

            std::vector<uint64_t> objectIDs;
            objectIDs.reserve(numEntries);

            const uint8_t* ptr = data.data() + arrayOffset;
            for (uint16_t i = 0; i < numEntries; ++i) {
                uint64_t objectID = 0;
                for (size_t b = 0; b < objectIdSize; ++b) {
                    objectID = (objectID << 8) | ptr[b];
                }
                objectIDs.push_back(objectID);
                ptr += objectIdSize;
            }

            Common::InvokeSharedCallback(completionState, true, std::move(objectIDs));
        });
}

//==============================================================================
// Command Submission
//==============================================================================

// Implement IAVCCommandSubmitter
void AVCUnit::SubmitCommand(const AVCCdb& cdb, AVCCompletion completion) {
    if (!fcpTransport_) {
        completion(AVCResult::kTransportError, cdb);
        return;
    }

    // Create AVCCommand to handle the transaction
    // Note: AVCCommand manages its own lifetime via shared_from_this during the transaction
    auto cmd = std::make_shared<AVCCommand>(*fcpTransport_, cdb);
    cmd->Submit(completion);
}


void AVCUnit::GetPlugInfo(std::function<void(AVCResult, const ASFW::AVC::Cmd::UnitPlugCounts&)> completion) {
    if (initialized_) {
        // Return cached result
        completion(AVCResult::kImplementedStable, model_.unitPlugs);
        return;
    }

    auto completionState = Common::ShareCallback(std::move(completion));
    ASFW::AVC::Cmd::PlugInfoCommand cmd{
        .operands = ASFW::AVC::Cmd::PlugInfoOperands{
            .form = ASFW::AVC::Cmd::PlugInfoForm::kUnitIsoExternal
        }
    };

    Status(cmd, [this, completionState](ASFW::AVC::Expected<ASFW::AVC::Cmd::PlugInfoReply> reply) {
        if (!reply) {
            Common::InvokeSharedCallback(completionState, AVCResult::kNotImplemented, ASFW::AVC::Cmd::UnitPlugCounts{});
            return;
        }
        model_.unitPlugs = reply->unit;
        Common::InvokeSharedCallback(completionState, AVCResult::kImplementedStable, model_.unitPlugs);
    });
}

//==============================================================================
// Bus Reset Handling
//==============================================================================

void AVCUnit::OnBusReset(uint32_t newGeneration) {
    ASFW_LOG_V2(AVC,
                "AVCUnit: Bus reset (generation %u)",
                newGeneration);

    // Forward to FCP transport (will handle pending commands)
    if (fcpTransport_) {
        fcpTransport_->OnBusReset(newGeneration);
    }
    model_.identity = Identity();

    // v1: Keep cached state (subunits, plugs rarely change)
    // Caller can re-Initialize() if topology changed

    // v2 improvement: Could invalidate cache on topology change
    // and re-probe automatically
}

void AVCUnit::OnRouteRevalidated() {
    const auto route = routeRegistry_.CurrentRoute(GetGUID());
    if (fcpTransport_ && route.has_value()) {
        fcpTransport_->OnRouteRevalidated(*route);
    }
    model_.identity = Identity();
}

//==============================================================================
// Accessors
//==============================================================================

uint64_t AVCUnit::GetGUID() const {
    auto device = device_.lock();
    if (!device) {
        return 0;
    }
    return device->GetGUID();
}

uint32_t AVCUnit::GetSpecID() const {
    auto unit = unit_.lock();
    if (!unit) {
        return 0;
    }
    return unit->GetUnitSpecID();
}
