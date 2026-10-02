//
// AudioSubunit.cpp
// ASFWDriver - AV/C Protocol Layer
//
// Audio Subunit implementation
//

#include "AudioSubunit.hpp"

void ASFW::Protocols::AVC::Audio::AudioSubunit::LoadSnapshot(const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot& snapshot) {
    const ASFW::AVC::SubunitId id{ASFW::AVC::SubunitType::kAudio, GetID()};
    for (const auto& contents : snapshot.contents) if (contents.id == id) identifier_ = contents.audio;
    for (const auto& blob : snapshot.descriptors)
        if (blob.subunit == id && blob.specifier == ASFW::AVC::Cmd::DescriptorSpecifier::SubunitIdentifier() && !blob.primaryError)
            descriptorData_ = blob.bytes;
    if (auto subunit = snapshot.unit.FindSubunit(id.type, id.id)) {
        numInputPlugs_ = subunit->plugs.destinationPlugs; numOutputPlugs_ = subunit->plugs.sourcePlugs;
    }
    inputPlugs_.clear(); outputPlugs_.clear();
    for (const auto& fact : snapshot.plugs) if (fact.address == id.ToAddress()) {
        const bool input = fact.direction == ASFW::AVC::Cmd::PlugDirection::kInput;
        (input ? inputPlugs_ : outputPlugs_).push_back({fact.id.value, input, fact.current, fact.formations});
    }
}
