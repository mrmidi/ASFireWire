//
// AudioSubunit.cpp
// ASFWDriver - AV/C Protocol Layer
//
// Audio Subunit implementation
//

#include "AudioSubunit.hpp"
#include "../AVCUnit.hpp"
#include "../AVCCommands.hpp"
#include "../AudioFunctionBlockCommand.hpp"
#include "../../../Logging/Logging.hpp"

using namespace ASFW::Protocols::AVC::Audio;

void AudioSubunit::ParseCapabilities(AVCUnit& unit, std::function<void(bool)> completion) {
    ASFW_LOG_INFO(Discovery, "AudioSubunit: Parsing capabilities for Audio subunit (id=%d)", GetID());
    
    auto unitPtr = unit.shared_from_this();
    
    QueryPlugCounts(unit, [this, unitPtr, completion](bool success) {
        if (!success) {
            ASFW_LOG_WARNING(Discovery, "AudioSubunit: Failed to query plug counts");
            completion(false);
            return;
        }
        
        ASFW_LOG_INFO(Discovery, "AudioSubunit: Found %d input plugs, %d output plugs",
                     numInputPlugs_, numOutputPlugs_);
        
        inputPlugs_.clear();
        outputPlugs_.clear();
        
        if (numInputPlugs_ > 0) {
            inputPlugs_.resize(numInputPlugs_);
            for (size_t i = 0; i < numInputPlugs_; ++i) {
                inputPlugs_[i].plugNumber = i;
                inputPlugs_[i].isInput = true;
            }
            QueryPlugFormats(*unitPtr, 0, true, completion);
        } else if (numOutputPlugs_ > 0) {
            outputPlugs_.resize(numOutputPlugs_);
            for (size_t i = 0; i < numOutputPlugs_; ++i) {
                outputPlugs_[i].plugNumber = i;
                outputPlugs_[i].isInput = false;
            }
            QueryPlugFormats(*unitPtr, 0, false, completion);
        } else {
            ASFW_LOG_INFO(Discovery, "AudioSubunit: No plugs to query");
            completion(true);
        }
    });
}

void AudioSubunit::QueryPlugCounts(AVCUnit& unit, std::function<void(bool)> completion) {
    uint8_t subunitAddr = (static_cast<uint8_t>(GetType()) << 3) | (GetID() & 0x07);
    
    auto cmd = std::make_shared<AVCPlugInfoCommand>(unit.GetFCPTransport(), subunitAddr);
    
    cmd->Submit([this, completion, cmd](AVCResult result, const AVCPlugInfoCommand::PlugInfo& info) {
        if (IsSuccess(result)) {
            numInputPlugs_ = info.numDestPlugs;
            numOutputPlugs_ = info.numSrcPlugs;
            completion(true);
        } else {
            ASFW_LOG_ERROR(Discovery, "AudioSubunit: PLUG_INFO failed: result=%d",
                          static_cast<int>(result));
            completion(false);
        }
    });
}

void AudioSubunit::QueryPlugFormats(AVCUnit& unit, size_t plugIndex, bool isInput,
                                   std::function<void(bool)> completion) {
    auto& plugs = isInput ? inputPlugs_ : outputPlugs_;
    
    if (plugIndex >= plugs.size()) {
        if (isInput && numOutputPlugs_ > 0) {
            outputPlugs_.resize(numOutputPlugs_);
            for (size_t i = 0; i < numOutputPlugs_; ++i) {
                outputPlugs_[i].plugNumber = i;
                outputPlugs_[i].isInput = false;
            }
            QueryPlugFormats(unit, 0, false, completion);
        } else {
            ASFW_LOG_INFO(Discovery, "AudioSubunit: Finished querying all plug formats");
            completion(true);
        }
        return;
    }
    
    auto unitPtr = unit.shared_from_this();
    
    uint8_t subunitAddr = (static_cast<uint8_t>(GetType()) << 3) | (GetID() & 0x07);
    uint8_t plugNum = plugs[plugIndex].plugNumber;
    
    auto cmd = std::make_shared<AVCStreamFormatCommand>(unit.GetFCPTransport(),
                                                        subunitAddr, plugNum, isInput);
    
    cmd->Submit([this, unitPtr, plugIndex, isInput, completion, cmd](
                AVCResult result, const std::optional<StreamFormat>& format) {
        auto& plugs = isInput ? inputPlugs_ : outputPlugs_;
        
        if (IsSuccess(result) && format) {
            plugs[plugIndex].currentFormat = *format;
            ASFW_LOG_INFO(Discovery, "AudioSubunit: Plug %d (%{public}s) current format: type=0x%02x",
                         plugs[plugIndex].plugNumber,
                         isInput ? "input" : "output",
                         format->formatType);
        } else {
            ASFW_LOG_WARNING(Discovery, "AudioSubunit: Failed to query current format for plug %d (%{public}s)",
                           plugs[plugIndex].plugNumber, isInput ? "input" : "output");
        }
        
        QueryPlugFormats(*unitPtr, plugIndex + 1, isInput, completion);
    });
}

void AudioSubunit::SetAudioVolume(AVCUnit& unit, uint8_t plugId, int16_t volume, std::function<void(bool)> completion) {
    uint8_t subunitAddr = (static_cast<uint8_t>(GetType()) << 3) | (GetID() & 0x07);
    
    // Volume data: 2 bytes, big endian
    std::vector<uint8_t> data;
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
    
    cmd->Submit([completion, cmd](AVCResult result, const std::vector<uint8_t>&) {
        if (IsSuccess(result)) {
            ASFW_LOG_V1(AVC, "AudioSubunit: Set volume success");
            completion(true);
        } else {
            ASFW_LOG_ERROR(AVC, "AudioSubunit: Set volume failed: result=%d", static_cast<int>(result));
            completion(false);
        }
    });
}

void AudioSubunit::SetAudioMute(AVCUnit& unit, uint8_t plugId, bool mute, std::function<void(bool)> completion) {
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
    
    cmd->Submit([completion, cmd](AVCResult result, const std::vector<uint8_t>&) {
        if (IsSuccess(result)) {
            ASFW_LOG_V1(AVC, "AudioSubunit: Set mute success");
            completion(true);
        } else {
            ASFW_LOG_ERROR(AVC, "AudioSubunit: Set mute failed: result=%d", static_cast<int>(result));
            completion(false);
        }
    });
}

