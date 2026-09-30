//
// AudioSubunit.cpp
// ASFWDriver - AV/C Protocol Layer
//
// Audio Subunit implementation
//

#include "AudioSubunit.hpp"
#include "../AVCUnit.hpp"
#include "../Commands/GeneralCommands.hpp"
#include "../Commands/StreamFormatCommand.hpp"
#include "../Descriptors/DescriptorAccessor.hpp"
#include "../Descriptors/AudioSubunitDescriptor.hpp"
#include "../Commands/FunctionBlockCommand.hpp"
#include "../../../Common/CallbackUtils.hpp"
#include "../../../Logging/Logging.hpp"

#include <set>

using namespace ASFW::Protocols::AVC::Audio;
using ASFW::AVC::SubunitAddress;
using ASFW::AVC::Expected;
using ASFW::AVC::ResponseCode;
using ASFW::Protocols::AVC::DescriptorAccessor;
using ASFW::Protocols::AVC::DescriptorSpecifier;
using ASFW::Protocols::AVC::DescriptorSpecifierType;
namespace Descriptors = ASFW::Protocols::AVC::Descriptors;
namespace Common = ASFW::Common;
namespace Cmd = ASFW::AVC::Cmd;

namespace {
struct AudioTextListTraversal : std::enable_shared_from_this<AudioTextListTraversal> {
    Descriptors::AudioSubunitIdentifier* identifier{};
    std::shared_ptr<DescriptorAccessor> accessor;
    std::shared_ptr<std::function<void(bool)>> completion;
    std::vector<uint16_t> pendingListIds;
    std::set<uint16_t> visited;
    Descriptors::TextDatabase database;
    size_t nextList{0};

    void Advance() {
        if (nextList == pendingListIds.size()) {
            ASFW_LOG(Discovery, "[AvcDescriptor] text complete lists=%zu entries=%zu functionBlocks=%zu",
                visited.size(), database.size(), identifier->functionBlocks.size());
            Descriptors::AudioSubunitDescriptorParser::ResolveNames(*identifier, database);
            Common::InvokeSharedCallback(completion, true);
            return;
        }
        const uint16_t listId = pendingListIds[nextList++];
        if (!visited.insert(listId).second || visited.size() > 32) {
            Common::InvokeSharedCallback(completion, false);
            return;
        }

        DescriptorSpecifier specifier{.type = DescriptorSpecifierType::kListID,
            .typeSpecificFields = {static_cast<uint8_t>(listId >> 8), static_cast<uint8_t>(listId)}};
        auto self = shared_from_this();
        accessor->readWithOpenCloseSequence(specifier, [self, listId](const DescriptorAccessor::ReadDescriptorResult& result) {
            if (!result.success) {
                ASFW_LOG_WARNING(Discovery, "[AvcDescriptor] list=%04x read failed result=%u", listId,
                    static_cast<unsigned>(result.avcResult));
                Common::InvokeSharedCallback(self->completion, false);
                return;
            }
            if (result.data.size() < 3 || result.data[2] != 0x86) {
                Common::InvokeSharedCallback(self->completion, false);
                return;
            }
            const auto& identifier = *self->identifier;
            auto children = Descriptors::AudioSubunitDescriptorParser::ParseChildListIds(
                result.data, identifier.sizeOfListId, identifier.sizeOfObjectId);
            if (!children) {
                Common::InvokeSharedCallback(self->completion, false);
                return;
            }
            auto text = Descriptors::AudioSubunitDescriptorParser::ParseTextDatabaseListChecked(result.data);
            if (!text) {
                Common::InvokeSharedCallback(self->completion, false);
                return;
            }
            ASFW_LOG(Discovery, "[AvcDescriptor] list=%04x bytes=%zu children=%zu textEntries=%zu",
                listId, result.data.size(), children->size(), text->size());
            self->database.insert(text->begin(), text->end());
            self->pendingListIds.insert(self->pendingListIds.end(), children->begin(), children->end());
            self->Advance();
        });
    }
};
} // namespace

void AudioSubunit::ParseCapabilities(AVCUnit& unit, std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    ASFW_LOG_INFO(Discovery, "AudioSubunit: Parsing capabilities for Audio subunit (id=%d)", GetID());
    
    auto unitPtr = unit.shared_from_this();

    ReadIdentifierDescriptor(unit, [this, unitPtr, completionState](bool /*descOk*/) {
        QueryPlugCounts(*unitPtr, [this, unitPtr, completionState](bool success) {
            if (!success) {
                ASFW_LOG_WARNING(Discovery, "AudioSubunit: Failed to query plug counts");
                Common::InvokeSharedCallback(completionState, false);
                return;
            }
            
            ASFW_LOG_INFO(Discovery, "AudioSubunit: Found %d input plugs, %d output plugs",
                         numInputPlugs_, numOutputPlugs_);
            
            inputPlugs_.clear();
            outputPlugs_.clear();
            
            if (numInputPlugs_ > 0) {
                inputPlugs_.resize(numInputPlugs_);
                for (size_t i = 0; i < numInputPlugs_; ++i) {
                    inputPlugs_[i].plugNumber = static_cast<uint8_t>(i);
                    inputPlugs_[i].isInput = true;
                }
                QueryPlugFormats(*unitPtr, 0, true, *completionState);
            } else if (numOutputPlugs_ > 0) {
                outputPlugs_.resize(numOutputPlugs_);
                for (size_t i = 0; i < numOutputPlugs_; ++i) {
                    outputPlugs_[i].plugNumber = static_cast<uint8_t>(i);
                    outputPlugs_[i].isInput = false;
                }
                QueryPlugFormats(*unitPtr, 0, false, *completionState);
            } else {
                ASFW_LOG_INFO(Discovery, "AudioSubunit: No plugs to query");
                Common::InvokeSharedCallback(completionState, true);
            }
        });
    });
}

void AudioSubunit::ReadIdentifierDescriptor(ASFW::AVC::IAvcUnit& unit, std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    auto accessor = std::make_shared<DescriptorAccessor>(unit, SubunitAddress::FromByte(GetAddress()));

    accessor->readUnitIdentifier([this, accessor, completionState](const DescriptorAccessor::ReadDescriptorResult& res) {
        if (!res.success || res.data.empty()) {
            ASFW_LOG_V1(Discovery, "AudioSubunit: No Identifier Descriptor found");
            Common::InvokeSharedCallback(completionState, false);
            return;
        }

        descriptorData_ = res.data;
        auto parsed = Descriptors::AudioSubunitDescriptorParser::ParseIdentifierDescriptor(res.data);
        if (!parsed) {
            ASFW_LOG_WARNING(Discovery, "AudioSubunit: Failed to parse Identifier Descriptor (%zu bytes)", res.data.size());
            Common::InvokeSharedCallback(completionState, false);
            return;
        }

        identifier_ = std::move(parsed);
        ASFW_LOG_INFO(Discovery, "AudioSubunit: Parsed Identifier Descriptor: %zu function blocks, %zu root lists",
                     identifier_->functionBlocks.size(), identifier_->rootListIds.size());

        if (identifier_->rootListIds.empty()) {
            Common::InvokeSharedCallback(completionState, true);
            return;
        }

        auto traversal = std::make_shared<AudioTextListTraversal>();
        traversal->identifier = &*identifier_;
        traversal->accessor = accessor;
        traversal->completion = completionState;
        traversal->pendingListIds = identifier_->rootListIds;
        traversal->Advance();
    });
}

void AudioSubunit::QueryPlugCounts(AVCUnit& unit, std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));

    auto cmd = Cmd::PlugInfoCommand{
        .address = SubunitAddress::FromByte(GetAddress()),
        .operands = Cmd::PlugInfoOperands{
            .form = Cmd::PlugInfoForm::kSubunit,
        },
    };

    unit.Status(cmd, [this, completionState](Expected<Cmd::PlugInfoReply> reply) {
        if (reply) {
            numInputPlugs_ = reply->subunit.destinationPlugs;
            numOutputPlugs_ = reply->subunit.sourcePlugs;
            Common::InvokeSharedCallback(completionState, true);
        } else {
            ASFW_LOG_ERROR(Discovery, "AudioSubunit: PLUG_INFO failed: error=%d",
                          static_cast<int>(reply.error().kind));
            Common::InvokeSharedCallback(completionState, false);
        }
    });
}

void AudioSubunit::QueryPlugFormats(AVCUnit& unit, size_t plugIndex, bool isInput,
                                   std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    auto& plugs = isInput ? inputPlugs_ : outputPlugs_;
    
    if (plugIndex >= plugs.size()) {
        if (isInput && numOutputPlugs_ > 0) {
            outputPlugs_.resize(numOutputPlugs_);
            for (size_t i = 0; i < numOutputPlugs_; ++i) {
                outputPlugs_[i].plugNumber = i;
                outputPlugs_[i].isInput = false;
            }
            QueryPlugFormats(unit, 0, false, *completionState);
        } else {
            ASFW_LOG_INFO(Discovery, "AudioSubunit: Finished querying all plug formats");
            Common::InvokeSharedCallback(completionState, true);
        }
        return;
    }
    
    auto unitPtr = unit.shared_from_this();
    uint8_t plugNum = plugs[plugIndex].plugNumber;
    
    // Intentional design choice (not a bug, not a plan violation):
    // Apple AppleFWAudio and AVCVideoServices try 0xBF first and fall back blindly to 0x2F
    // on NOT IMPLEMENTED. FireWire FCP roundtrips for NOT IMPLEMENTED take < 1 ms, so
    // blind probing with fallback is cheap, standard, and eliminates persistent opcode state.
    Cmd::StreamFormatCommand cmd{
        .address = SubunitAddress::FromByte(MakeSubunitAddress(GetType(), GetID())),
        .operands = {
            .form = Cmd::StreamFormatSubfunction::kSingle,
            .opcode = Cmd::StreamFormatOpcode::kExtendedStreamFormat,
            .plug = Cmd::PlugAddress::SubunitPlug(isInput ? Cmd::PlugDirection::kInput : Cmd::PlugDirection::kOutput, plugNum),
        }
    };
    
    unit.Status(cmd, [this, unitPtr, plugIndex, isInput, completionState, cmd](
                Expected<Cmd::StreamFormatReply> reply) mutable {
        if (!reply && reply.error().response == ResponseCode::kNotImplemented) {
            cmd.operands.opcode = Cmd::StreamFormatOpcode::kStreamFormatSupport;
            unitPtr->Status(cmd, [this, unitPtr, plugIndex, isInput, completionState](
                        Expected<Cmd::StreamFormatReply> fallbackReply) {
                auto& plugs = isInput ? inputPlugs_ : outputPlugs_;
                if (fallbackReply) {
                    plugs[plugIndex].currentFormat = fallbackReply->format;
                    ASFW_LOG_INFO(Discovery, "AudioSubunit: Plug %d (%{public}s) current format",
                                 plugs[plugIndex].plugNumber,
                                 isInput ? "input" : "output");
                } else {
                    ASFW_LOG_WARNING(Discovery, "AudioSubunit: Failed to query current format for plug %d (%{public}s)",
                                   plugs[plugIndex].plugNumber, isInput ? "input" : "output");
                }
                QueryPlugFormats(*unitPtr, plugIndex + 1, isInput, *completionState);
            });
            return;
        }

        auto& plugs = isInput ? inputPlugs_ : outputPlugs_;
        
        if (reply) {
            plugs[plugIndex].currentFormat = reply->format;
            ASFW_LOG_INFO(Discovery, "AudioSubunit: Plug %d (%{public}s) current format",
                         plugs[plugIndex].plugNumber,
                         isInput ? "input" : "output");
        } else {
            ASFW_LOG_WARNING(Discovery, "AudioSubunit: Failed to query current format for plug %d (%{public}s)",
                           plugs[plugIndex].plugNumber, isInput ? "input" : "output");
        }
        
        QueryPlugFormats(*unitPtr, plugIndex + 1, isInput, *completionState);
    });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void AudioSubunit::SetAudioVolume(AVCUnit& unit, uint8_t plugId, int16_t volume, std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    Cmd::FeatureCommand cmd{
        .address = SubunitAddress::Of(static_cast<ASFW::AVC::SubunitType>(GetType()), GetID()),
        .operands = Cmd::FeatureOperands::Volume(
            plugId, Cmd::kMasterChannel, ASFW::AVC::AvcVolume::FromRaw(volume)),
    };
    unit.Control(cmd, [completionState](Expected<Cmd::FeatureReply> result) {
        if (result) {
            ASFW_LOG_V1(AVC, "AudioSubunit: Set volume success");
            Common::InvokeSharedCallback(completionState, true);
        } else {
            ASFW_LOG_ERROR(AVC, "AudioSubunit: Set volume failed: error=%u",
                           static_cast<unsigned>(result.error().kind));
            Common::InvokeSharedCallback(completionState, false);
        }
    });
}

void AudioSubunit::SetAudioMute(AVCUnit& unit, uint8_t plugId, bool mute, std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    Cmd::FeatureCommand cmd{
        .address = SubunitAddress::Of(static_cast<ASFW::AVC::SubunitType>(GetType()), GetID()),
        .operands = Cmd::FeatureOperands::Mute(plugId, Cmd::kMasterChannel, mute),
    };
    unit.Control(cmd, [completionState](Expected<Cmd::FeatureReply> result) {
        if (result) {
            ASFW_LOG_V1(AVC, "AudioSubunit: Set mute success");
            Common::InvokeSharedCallback(completionState, true);
        } else {
            ASFW_LOG_ERROR(AVC, "AudioSubunit: Set mute failed: error=%u",
                           static_cast<unsigned>(result.error().kind));
            Common::InvokeSharedCallback(completionState, false);
        }
    });
}
