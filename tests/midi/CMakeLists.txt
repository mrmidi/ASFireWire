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
