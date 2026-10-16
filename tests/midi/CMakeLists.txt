# tests/midi/CMakeLists.txt

macro(add_midi_test TARGET_NAME)
    add_executable(${TARGET_NAME} ${ARGN})
    target_link_libraries(${TARGET_NAME} PRIVATE
        asfw_test_support
        GTest::gtest_main
    )
    gtest_discover_tests(${TARGET_NAME})
endmacro()

# MIDI 1.0 byte stream <-> UMP conversion (WP-3).
# Pure logic: no DriverKit, no hardware, no transport.
add_midi_test(MidiUmpConversionTests
    MidiByteStreamToUmpTests.cpp
    UmpToMidiByteStreamTests.cpp
    UmpRoundTripTests.cpp
    "${ASFW_DRIVER_DIR}/Midi/Ump/MidiByteStreamToUmp.cpp"
    "${ASFW_DRIVER_DIR}/Midi/Ump/UmpToMidiByteStream.cpp"
)

# MIDI capability projection from discovered wire geometry (WP-1).
add_midi_test(MidiEndpointCapabilitiesTests
    MidiEndpointCapabilitiesTests.cpp
    "${ASFW_DRIVER_DIR}/Midi/Capabilities/MidiEndpointCapabilities.cpp"
)

# MIDI byte seam: SPSC rings and the packet reservation protocol (WP-4).
add_midi_test(MidiTransportBlockTests
    MidiTransportBlockTests.cpp
)

# MPX-MIDI receive extraction and the ring sink (WP-7).
add_midi_test(MpxMidiDemuxTests
    MpxMidiDemuxTests.cpp
)

# MPX-MIDI transmit composition and the UART rate model (WP-8).
add_midi_test(MpxMidiTxTests
    MpxMidiTxTests.cpp
)

# Device -> host consumer: gap-safe drain, epoch handoff.
add_midi_test(MidiRxDrainTests
    MidiRxDrainTests.cpp
    "${ASFW_DRIVER_DIR}/Midi/Ump/MidiByteStreamToUmp.cpp"
)

# Host -> device producer: whole callbacks, transactional SysEx.
add_midi_test(MidiTxIngressTests
    MidiTxIngressTests.cpp
    "${ASFW_DRIVER_DIR}/Midi/Ump/UmpToMidiByteStream.cpp"
)

# Composition into armed packets built by the production packetizer.
add_midi_test(MidiTxComposerTests
    MidiTxComposerTests.cpp
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpCadence.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpPacketTimeline.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/AmdtpTxPacketizer.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/AMDTP/PcmSlotCodec.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/CipHeader.cpp"
    "${ASFW_DRIVER_DIR}/Audio/Wire/IEC61883/DbcCounter.cpp"
)

# Nub publication order: endpoint id explicitly after Create, before Publish.
add_midi_test(MidiNubPublisherTests
    MidiNubPublisherTests.cpp
    "${ASFW_DRIVER_DIR}/Midi/Core/MidiNubPublisher.cpp"
    "${ASFW_DRIVER_DIR}/Midi/Capabilities/MidiEndpointCapabilities.cpp"
)
