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
#include "../AudioFunctionBlockCommand.hpp"
#include "../../../Common/CallbackUtils.hpp"
#include "../../../Logging/Logging.hpp"

using namespace ASFW::Protocols::AVC::Audio;
using ASFW::AVC::SubunitAddress;
using ASFW::AVC::Expected;
using ASFW::AVC::ResponseCode;
namespace Cmd = ASFW::AVC::Cmd;

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

        auto parsed = Descriptors::AudioSubunitDescriptorParser::ParseIdentifierDescriptor(res.data);
        if (!parsed) {
            ASFW_LOG_WARNING(Discovery, "AudioSubunit: Failed to parse Identifier Descriptor (%zu bytes)", res.data.size());
            Common::InvokeSharedCallback(completionState, false);
            return;
        }

        identifier_ = std::move(parsed);
        ASFW_LOG_INFO(Discovery, "AudioSubunit: Parsed Identifier Descriptor: %zu function blocks, %zu root lists",
                     identifier_->functionBlocks.size(), identifier_->rootListIds.size());

        // Check if there is a Text Database root list (0x1800 or 0x1801)
        uint16_t textDbListId = 0;
        for (uint16_t listId : identifier_->rootListIds) {
            if (listId == 0x1800 || listId == 0x1801) {
                textDbListId = listId;
                break;
            }
        }

        if (textDbListId == 0) {
            Common::InvokeSharedCallback(completionState, true);
            return;
        }

        // Read the Text Database list descriptor
        DescriptorSpecifier specifier;
        specifier.type = DescriptorSpecifierType::kListID;
        specifier.typeSpecificFields = {static_cast<uint8_t>(textDbListId >> 8), static_cast<uint8_t>(textDbListId & 0xFF)};

        accessor->readComplete(specifier, [this, completionState, textDbListId](const DescriptorAccessor::ReadDescriptorResult& textRes) {
            if (textRes.success && !textRes.data.empty()) {
                auto textDb = Descriptors::AudioSubunitDescriptorParser::ParseTextDatabaseList(textRes.data);
                Descriptors::AudioSubunitDescriptorParser::ResolveNames(*identifier_, textDb);
                ASFW_LOG_INFO(Discovery, "AudioSubunit: Resolved %zu names from Text Database 0x%04x",
                             textDb.size(), textDbListId);
            }
            Common::InvokeSharedCallback(completionState, true);
        });
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
    uint8_t subunitAddr = (static_cast<uint8_t>(GetType()) << 3) | (GetID() & 0x07);
    
    // Volume data: channel (0x00 Master), data length (0x02), and 2-byte volume
    std::vector<uint8_t> data;
    data.push_back(0x00);
    data.push_back(0x02);
    data.push_back(static_cast<uint8_t>((volume >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>(volume & 0xFF));
    
    auto cmd = std::make_shared<AudioFunctionBlockCommand>(
        unit, // AVCUnit implements IAVCCommandSubmitter
        subunitAddr,
        AudioFunctionBlockCommand::CommandType::kControl,
        plugId,
        AudioFunctionBlockCommand::ControlSelector::kVolume,
        data
    );
    
    cmd->Submit([completionState, cmd](AVCResult result, const std::vector<uint8_t>&) {
        if (IsSuccess(result)) {
            ASFW_LOG_V1(AVC, "AudioSubunit: Set volume success");
            Common::InvokeSharedCallback(completionState, true);
        } else {
            ASFW_LOG_ERROR(AVC, "AudioSubunit: Set volume failed: result=%d", static_cast<int>(result));
            Common::InvokeSharedCallback(completionState, false);
        }
    });
}

void AudioSubunit::SetAudioMute(AVCUnit& unit, uint8_t plugId, bool mute, std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    uint8_t subunitAddr = (static_cast<uint8_t>(GetType()) << 3) | (GetID() & 0x07);
    
    // Mute data: 1 byte (0x70 = Mute, 0x60 = Unmute) - typical for Audio Subunit
    // Wait, spec says:
    // Mute: 0x70 (On), 0x60 (Off)
    uint8_t muteVal = mute ? 0x70 : 0x60;
    
    auto cmd = std::make_shared<AudioFunctionBlockCommand>(
        unit,
        subunitAddr,
        AudioFunctionBlockCommand::CommandType::kControl,
        plugId,
        AudioFunctionBlockCommand::ControlSelector::kMute,
        std::vector<uint8_t>{muteVal}
    );
    
    cmd->Submit([completionState, cmd](AVCResult result, const std::vector<uint8_t>&) {
        if (IsSuccess(result)) {
            ASFW_LOG_V1(AVC, "AudioSubunit: Set mute success");
            Common::InvokeSharedCallback(completionState, true);
        } else {
            ASFW_LOG_ERROR(AVC, "AudioSubunit: Set mute failed: result=%d", static_cast<int>(result));
            Common::InvokeSharedCallback(completionState, false);
        }
    });
}
