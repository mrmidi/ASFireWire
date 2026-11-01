//
// ASFWAudioDriverMidi.cpp
// ASFWDriver
//
// The audio driver's share of MIDI over AMDTP: compose host -> device MIDI
// bytes into the primary playback stream's MPX slot, or, for a MOTU stream,
// into each data block's message bytes (MotuMidiTxComposer).
//
// The audio driver runs the TX preparation pump and holds the mapped TX slab, so
// this is where the packets are, and it is the only reason it knows MIDI exists.
// Everything here is inert for an endpoint that published no MIDI service:
// BindMidiTx finds no seam and returns without a log line, `midiTxBlock` stays
// null, and ComposeMidiTx returns on its first test. None of it starts, stops or
// extends a stream; a MIDI byte is only ever carried by packets the audio stream
// was already sending.
//

#include "ASFWAudioDriverPrivate.hpp"

#include "../../Common/DriverKitOwnership.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>

namespace ASFW::Audio::DriverKit {

namespace {

// Map the endpoint's MIDI seam held in ivars.midiBuffer. Null, with the buffer
// and mapping released, when it cannot be used.
ASFW::Midi::MidiTransportBlock* MapMidiSeam(ASFWAudioDriver_IVars& ivars) noexcept {
    if (ASFW::Common::CreateSharedMapping(ivars.midiBuffer, ivars.midiMap) != kIOReturnSuccess ||
        !ivars.midiMap ||
        ivars.midiMap->GetLength() < sizeof(ASFW::Midi::MidiTransportBlock)) {
        ASFW_LOG_ERROR(Midi, "[MidiTx] not composing: cannot map the MIDI seam");
        ivars.midiMap.reset();
        ivars.midiBuffer.reset();
        return nullptr;
    }
    auto* block = reinterpret_cast<ASFW::Midi::MidiTransportBlock*>(
        static_cast<uintptr_t>(ivars.midiMap->GetAddress()));
    if (block->layoutVersion.load(std::memory_order_relaxed) !=
        ASFW::Midi::MidiTransportBlock::kLayoutVersion) {
        ASFW_LOG_ERROR(Midi, "[MidiTx] not composing: seam layout version %u, expected %u",
                       block->layoutVersion.load(std::memory_order_relaxed),
                       ASFW::Midi::MidiTransportBlock::kLayoutVersion);
        ivars.midiMap.reset();
        ivars.midiBuffer.reset();
        return nullptr;
    }
    return block;
}

// A MOTU stream: the seam's AM824 geometry is empty by design (MOTU reports no
// MIDI in its registers), so the slot comes from the model this run armed,
// and the data block size from the stream itself.
void BindMotuMidiTx(ASFWAudioDriver_IVars& ivars,
                    const ASFW::Isoch::Audio::AudioStreamConfig& txConfig) noexcept {
    const uint32_t version = ivars.runtime.txMotuUnitVersion;
    const auto slot = ASFW::Encoding::Motu::OffsetsOf(
        ASFW::Encoding::Motu::MidiSlotFor(version, /*capture=*/false));
    if (!slot) {
        ASFW_LOG_ERROR(Midi, "[MidiTx] not composing: MOTU 0x%x has no MIDI slot", version);
        ivars.midiBuffer.reset();
        return;
    }
    auto* block = MapMidiSeam(ivars);
    if (block == nullptr) return;
    ASFW::AudioEngine::Direct::Tx::MotuMidiTxComposerConfig config{};
    config.slot = *slot;
    config.dbs = txConfig.dbs;
    config.sampleRateHz = txConfig.sampleRate;
    config.framesPerDataPacket = txConfig.framesPerDataPacket;
    if (!ivars.runtime.motuMidiTxComposer.Configure(config)) {
        ASFW_LOG_ERROR(Midi, "[MidiTx] not composing: MOTU composer refused dbs=%u rate=%u",
                       static_cast<uint32_t>(txConfig.dbs), txConfig.sampleRate);
        ivars.midiMap.reset();
        ivars.midiBuffer.reset();
        return;
    }
    ivars.runtime.midiTxMotu = true;
    ivars.runtime.midiTxReportedBytes = 0;
    ivars.runtime.midiTxReportedAnomalies = 0;
    ivars.runtime.midiTxBlock = block;
    ASFW_LOG(Midi, "[MidiTx] composing MOTU 0x%x flag=%u byte=%u dbs=%u rate=%u",
             version, slot->flag, slot->byte, static_cast<uint32_t>(txConfig.dbs),
             txConfig.sampleRate);
}

} // namespace

void BindMidiTx(ASFWAudioDriver_IVars& ivars,
                const ASFW::Isoch::Audio::AudioStreamConfig& txConfig) noexcept {
    UnbindMidiTx(ivars);
    auto* nub = ivars.device.audioNub;
    if (nub == nullptr) return;

    IOMemoryDescriptor* raw = nullptr;
    uint32_t ports = 0;
    uint32_t slotIndex = 0;
    uint32_t dbs = 0;
    uint32_t pcmChannels = 0;
    uint32_t dbcAligned = 1;
    const kern_return_t kr =
        nub->CopyMidiTransportMemory(&raw, &ports, &slotIndex, &dbs, &pcmChannels, &dbcAligned);
    // The ordinary outcome for an endpoint without MIDI: nothing to say.
    if (kr != kIOReturnSuccess || raw == nullptr) return;
    ivars.midiBuffer = ASFW::Common::AdoptRetained(raw);
    if (ports == 0) {  // sources only: no destination to compose for
        ivars.midiBuffer.reset();
        return;
    }

    if (ivars.runtime.txMotuUnitVersion != 0) {
        BindMotuMidiTx(ivars, txConfig);
        return;
    }

    // The MIDI slot was projected from the device's stream table. The playback
    // stream this run actually framed must be the same stream, or the slot index
    // would point into a different block layout and MIDI would overwrite audio.
    const bool cip = txConfig.packetFraming == ASFW::Encoding::AudioPacketFraming::kCip;
    if (!cip || dbs != txConfig.dbs || pcmChannels != txConfig.pcmChannels ||
        slotIndex >= dbs || ports > ASFW::Encoding::kMpxMidiPorts) {
        ASFW_LOG_ERROR(Midi,
                       "[MidiTx] not composing: geometry mismatch seam(dbs=%u pcm=%u slot=%u "
                       "ports=%u) stream(dbs=%u pcm=%u cip=%u)",
                       dbs, pcmChannels, slotIndex, ports,
                       static_cast<uint32_t>(txConfig.dbs),
                       static_cast<uint32_t>(txConfig.pcmChannels), cip ? 1U : 0U);
        ivars.midiBuffer.reset();
        return;
    }

    auto* block = MapMidiSeam(ivars);
    if (block == nullptr) return;

    ASFW::AudioEngine::Direct::Tx::MidiTxComposerConfig config{};
    config.geometry = ASFW::Encoding::MpxMidiGeometry{
        .dbs = static_cast<uint8_t>(dbs),
        .midiSlotIndex = static_cast<uint8_t>(slotIndex),
        .portCount = static_cast<uint8_t>(ports),
        .dbcAligned = dbcAligned != 0,
    };
    config.sampleRateHz = txConfig.sampleRate;
    config.sytIntervalFrames = txConfig.framesPerDataPacket;
    if (!ivars.runtime.midiTxComposer.Configure(config)) {
        ASFW_LOG_ERROR(Midi, "[MidiTx] not composing: composer refused rate=%u syt=%u",
                       txConfig.sampleRate, static_cast<uint32_t>(txConfig.framesPerDataPacket));
        ivars.midiMap.reset();
        ivars.midiBuffer.reset();
        return;
    }
    ivars.runtime.midiTxMotu = false;
    ivars.runtime.midiTxReportedBytes = 0;
    ivars.runtime.midiTxReportedAnomalies = 0;
    ivars.runtime.midiTxBlock = block;
    ASFW_LOG(Midi, "[MidiTx] composing ports=%u slot=%u dbs=%u rate=%u syt=%u",
             ports, slotIndex, dbs, txConfig.sampleRate,
             static_cast<uint32_t>(txConfig.framesPerDataPacket));
}

void ComposeMidiTx(ASFWAudioDriver_IVars& ivars) noexcept {
    auto* block = ivars.runtime.midiTxBlock;
    if (block == nullptr) return;
    const auto* queue = ivars.runtime.txSlotProvider.queueControl;
    if (queue == nullptr) return;

    // The armed end is where the producer has committed to; the frontier is
    // where PCM stops being written. MIDI works in between, never outside it.
    const uint64_t armedEnd = queue->committedEnd.load(std::memory_order_acquire);
    const uint64_t firstWritable = TxFirstWritablePacket(*queue);
    if (ivars.runtime.midiTxMotu) {
        (void)ivars.runtime.motuMidiTxComposer.Compose(
            block, ivars.runtime.txStreamEngine.Timeline(), firstWritable, armedEnd);
        return;
    }
    (void)ivars.runtime.midiTxComposer.Compose(
        block, ivars.runtime.txStreamEngine.Timeline(), firstWritable, armedEnd);
}

void ReportMidiTx(ASFWAudioDriver_IVars& ivars) noexcept {
    if (ivars.runtime.midiTxBlock == nullptr) return;
    const auto report = [&](const auto& counters, const auto& reservation) {
        const uint64_t anomalies = counters.geometryMismatches + reservation.staleEpochCommits +
                                   reservation.mismatchedCommits;
        if (counters.bytesComposed == ivars.runtime.midiTxReportedBytes &&
            anomalies == ivars.runtime.midiTxReportedAnomalies) {
            return;  // a clean idle stream prints nothing
        }
        ivars.runtime.midiTxReportedBytes = counters.bytesComposed;
        ivars.runtime.midiTxReportedAnomalies = anomalies;
        ASFW_LOG(Midi,
                 "[MidiTx] bytes=%llu packets=%llu pastFrontier=%llu geometry=%llu staleCommit=%llu "
                 "mismatchCommit=%llu epochs=%llu",
                 counters.bytesComposed, counters.packetsComposed, counters.packetsPastFrontier,
                 counters.geometryMismatches, reservation.staleEpochCommits,
                 reservation.mismatchedCommits, counters.epochChanges);
    };
    if (ivars.runtime.midiTxMotu) {
        report(ivars.runtime.motuMidiTxComposer.Counters(),
               ivars.runtime.motuMidiTxComposer.ReservationCounters());
    } else {
        report(ivars.runtime.midiTxComposer.Counters(), ivars.runtime.midiTxComposer.ReservationCounters());
    }
}

} // namespace ASFW::Audio::DriverKit
