// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>
#include "AvcTestRig.hpp"
#include "ASFWDriver/Audio/Protocols/Prism/Orpheus/OrpheusProtocol.hpp"
#include "ASFWDriver/Audio/Controls/ControlNotifications.hpp"

using namespace ASFW::Audio::Prism::Orpheus;
using ASFW::Testing::AvcTestRig;
using ASFW::Testing::AvcReply;
namespace AM = ASFW::AudioModel;
namespace C = ASFW::Audio::Controls;

namespace {

struct OrpheusDeviceModel {
    OrpheusUnitState unitState{};
    std::array<OrpheusAnalogChannelState, 8> channels{};
    std::array<OrpheusMixBusState, 6> buses{};
    OrpheusSyncSource syncSource{OrpheusSyncSource::Local};

    std::vector<std::vector<uint8_t>> receivedCommands;
    bool rejectNext{false};

    OrpheusDeviceModel() {
        unitState.masterVolume = 0; // 0.0 dB
        unitState.masterMute = false;
        unitState.adatMode = 0; // disabled
        unitState.clockOutput = 0;
        unitState.sampleRateConverter = 0;

        for (uint32_t i = 0; i < 8; ++i) {
            channels[i].inputType = (i < 4) ? 0 : 1; // 0 = mic on 0..3, 1 = line on 4..7
            channels[i].lineInputLevel = 0;
            channels[i].filter = 0;
            channels[i].micGain = 20;
            channels[i].instrumentGain = 15;
            channels[i].phantom = (i == 0); // Phantom on Ch 1
            channels[i].phase = false;
            channels[i].overkiller = false;
        }

        for (uint32_t b = 0; b < 6; ++b) {
            buses[b].busIndex = static_cast<uint8_t>(b);
            buses[b].masterGain = 0;
            buses[b].masterMute = false;
            buses[b].soloMask = 0;
            for (size_t in = 0; in < Wire::kMixInputCount; ++in) {
                buses[b].inputGains[in] = 0;
                buses[b].inputMutes[in] = false;
                buses[b].inputPans[in] = 0;
            }
        }
    }

    std::optional<AvcReply> operator()(std::span<const uint8_t> cmd) {
        if (cmd.size() < 3) return AvcReply::NotImplemented();
        receivedCommands.emplace_back(cmd.begin(), cmd.end());

        if (rejectNext) {
            rejectNext = false;
            return AvcReply::Rejected();
        }

        const uint8_t ctype = cmd[0];
        const uint8_t opcode = cmd[2];

        // 1. SIGNAL SOURCE (0x1A)
        if (opcode == Wire::kOpcodeSignalSource) {
            const bool hasAdat = unitState.HasAdatInput();
            if (ctype == Wire::kCtypeStatus) {
                const auto statusFrame = OrpheusVendorCodec::BuildSignalSourceControl(syncSource, hasAdat);
                if (!statusFrame) return AvcReply::NotImplemented();
                std::vector<uint8_t> reply = *statusFrame;
                reply[0] = Wire::kResponseStable;
                return AvcReply::RawBytes(std::move(reply));
            } else if (ctype == Wire::kCtypeControl) {
                const auto parsed = OrpheusVendorCodec::ParseSignalSourceStatus(cmd, hasAdat);
                if (parsed) syncSource = *parsed;
                std::vector<uint8_t> reply(cmd.begin(), cmd.end());
                reply[0] = Wire::kResponseAccepted;
                return AvcReply::RawBytes(std::move(reply));
            }
        }

        // 2. VENDOR DEPENDENT (0xEF)
        if (opcode == Wire::kOpcodeVendorDependent) {
            if (cmd.size() < Wire::kPayloadOffset) return AvcReply::NotImplemented();
            if (cmd[3] != Wire::kCompanyId[0] || cmd[4] != Wire::kCompanyId[1] || cmd[5] != Wire::kCompanyId[2]) {
                return AvcReply::NotImplemented();
            }

            const auto command = static_cast<OrpheusCommand>(cmd[Wire::kCommandOffset]);

            if (ctype == Wire::kCtypeStatus) {
                if (command == OrpheusCommand::UnitState) {
                    std::vector<uint8_t> reply(Wire::kPayloadOffset + 8, 0);
                    reply[0] = Wire::kResponseStable;
                    reply[1] = Wire::kSubunitAudio;
                    reply[2] = Wire::kOpcodeVendorDependent;
                    reply[3] = Wire::kCompanyId[0];
                    reply[4] = Wire::kCompanyId[1];
                    reply[5] = Wire::kCompanyId[2];
                    reply[6] = static_cast<uint8_t>(OrpheusCommand::UnitState);
                    reply[7] = static_cast<uint8_t>(static_cast<uint16_t>(unitState.masterVolume) >> 8);
                    reply[8] = static_cast<uint8_t>(static_cast<uint16_t>(unitState.masterVolume) & 0xFF);
                    reply[9] = static_cast<uint8_t>(unitState.volumeSelectMask >> 8);
                    reply[10] = static_cast<uint8_t>(unitState.volumeSelectMask & 0xFF);
                    uint8_t flags = 0;
                    if (unitState.masterMute) flags |= 0x01;
                    if (unitState.masterLock) flags |= 0x02;
                    if (unitState.identify) flags |= 0x04;
                    flags |= (unitState.frontPanelMeterMode << 4);
                    reply[11] = flags;
                    reply[12] = static_cast<uint8_t>((unitState.clockOutput << 4) | (unitState.sampleRateConverter & 0x0F));
                    reply[13] = unitState.adatMode;
                    reply[14] = static_cast<uint8_t>((unitState.meterBrightness << 4) | (unitState.headphoneSource & 0x0F));
                    return AvcReply::RawBytes(std::move(reply));
                }
                if (command == OrpheusCommand::AnalogChannelState) {
                    const uint8_t ch = cmd[Wire::kPayloadOffset];
                    if (ch >= 8) return AvcReply::Rejected();
                    std::vector<uint8_t> reply(Wire::kPayloadOffset + 6, 0);
                    reply[0] = Wire::kResponseStable;
                    reply[1] = Wire::kSubunitAudio;
                    reply[2] = Wire::kOpcodeVendorDependent;
                    reply[3] = Wire::kCompanyId[0];
                    reply[4] = Wire::kCompanyId[1];
                    reply[5] = Wire::kCompanyId[2];
                    reply[6] = static_cast<uint8_t>(OrpheusCommand::AnalogChannelState);
                    reply[7] = ch;
                    reply[8] = channels[ch].inputType;
                    uint8_t flags = 0;
                    if (channels[ch].phantom) flags |= 0x20;
                    if (channels[ch].phase) flags |= 0x40;
                    if (channels[ch].overkiller) flags |= 0x04;
                    reply[9] = flags;
                    reply[10] = channels[ch].filter;
                    reply[11] = channels[ch].micGain;
                    reply[12] = channels[ch].instrumentGain;
                    return AvcReply::RawBytes(std::move(reply));
                }
                if (command == OrpheusCommand::MixBusState) {
                    const uint8_t bus = cmd[Wire::kPayloadOffset];
                    if (bus >= 6) return AvcReply::Rejected();
                    std::vector<uint8_t> reply(Wire::kPayloadOffset + 67, 0);
                    reply[0] = Wire::kResponseStable;
                    reply[1] = Wire::kSubunitAudio;
                    reply[2] = Wire::kOpcodeVendorDependent;
                    reply[3] = Wire::kCompanyId[0];
                    reply[4] = Wire::kCompanyId[1];
                    reply[5] = Wire::kCompanyId[2];
                    reply[6] = static_cast<uint8_t>(OrpheusCommand::MixBusState);
                    reply[7] = bus;
                    reply[8] = static_cast<uint8_t>(buses[bus].masterGain >> 8);
                    reply[9] = static_cast<uint8_t>(buses[bus].masterGain & 0xFF);
                    reply[10] = buses[bus].masterMute ? 1 : 0;
                    reply[11] = static_cast<uint8_t>(buses[bus].soloMask >> 8);
                    reply[12] = static_cast<uint8_t>(buses[bus].soloMask & 0xFF);
                    for (size_t in = 0; in < Wire::kMixInputCount; ++in) {
                        const size_t base = 7 + in * 5;
                        reply[7 + base] = static_cast<uint8_t>(buses[bus].inputGains[in] >> 8);
                        reply[7 + base + 1] = static_cast<uint8_t>(buses[bus].inputGains[in] & 0xFF);
                        reply[7 + base + 2] = buses[bus].inputMutes[in] ? 1 : 0;
                        reply[7 + base + 3] = 0;
                        reply[7 + base + 4] = static_cast<uint8_t>(buses[bus].inputPans[in]);
                    }
                    return AvcReply::RawBytes(std::move(reply));
                }
                return AvcReply::NotImplemented();
            } else if (ctype == Wire::kCtypeControl) {
                if (command == OrpheusCommand::MasterVolume) {
                    unitState.masterVolume = static_cast<int16_t>((static_cast<uint16_t>(cmd[Wire::kPayloadOffset]) << 8) | cmd[Wire::kPayloadOffset + 1]);
                } else if (command == OrpheusCommand::MasterMute) {
                    unitState.masterMute = (cmd[Wire::kPayloadOffset] != 0);
                } else if (command == OrpheusCommand::AdatMode) {
                    unitState.adatMode = cmd[Wire::kPayloadOffset];
                } else if (command == OrpheusCommand::ClockOutput) {
                    unitState.clockOutput = cmd[Wire::kPayloadOffset];
                } else if (command == OrpheusCommand::SampleRateConverter) {
                    unitState.sampleRateConverter = cmd[Wire::kPayloadOffset];
                } else if (command == OrpheusCommand::MicGain) {
                    const uint8_t ch = cmd[Wire::kPayloadOffset];
                    if (ch < 8) channels[ch].micGain = cmd[Wire::kPayloadOffset + 1];
                } else if (command == OrpheusCommand::InstrumentGain) {
                    const uint8_t ch = cmd[Wire::kPayloadOffset];
                    if (ch < 8) channels[ch].instrumentGain = cmd[Wire::kPayloadOffset + 1];
                } else if (command == OrpheusCommand::Phantom) {
                    const uint8_t ch = cmd[Wire::kPayloadOffset];
                    if (ch < 8) channels[ch].phantom = (cmd[Wire::kPayloadOffset + 1] != 0);
                } else if (command == OrpheusCommand::Phase) {
                    const uint8_t ch = cmd[Wire::kPayloadOffset];
                    if (ch < 8) channels[ch].phase = (cmd[Wire::kPayloadOffset + 1] != 0);
                } else if (command == OrpheusCommand::Filter) {
                    const uint8_t ch = cmd[Wire::kPayloadOffset];
                    if (ch < 8) channels[ch].filter = cmd[Wire::kPayloadOffset + 1];
                } else if (command == OrpheusCommand::Overkiller) {
                    const uint8_t ch = cmd[Wire::kPayloadOffset];
                    if (ch < 8) channels[ch].overkiller = (cmd[Wire::kPayloadOffset + 1] != 0);
                } else if (command == OrpheusCommand::LineInputLevel) {
                    const uint8_t ch = cmd[Wire::kPayloadOffset];
                    if (ch < 8) channels[ch].lineInputLevel = cmd[Wire::kPayloadOffset + 1];
                } else if (command == OrpheusCommand::InputType) {
                    const uint8_t ch = cmd[Wire::kPayloadOffset];
                    if (ch < 8) channels[ch].inputType = cmd[Wire::kPayloadOffset + 1];
                } else if (command == OrpheusCommand::MixInputGain) {
                    const uint8_t bus = cmd[Wire::kPayloadOffset];
                    const uint8_t in = cmd[Wire::kPayloadOffset + 1];
                    if (bus < 6 && in < Wire::kMixInputCount) {
                        buses[bus].inputGains[in] = static_cast<int16_t>((static_cast<uint16_t>(cmd[Wire::kPayloadOffset + 2]) << 8) | cmd[Wire::kPayloadOffset + 3]);
                    }
                }
                std::vector<uint8_t> reply(cmd.begin(), cmd.end());
                reply[0] = Wire::kResponseAccepted;
                return AvcReply::RawBytes(std::move(reply));
            }
        }
        return AvcReply::NotImplemented();
    }
};

struct OrpheusControlRig {
    AvcTestRig rig;
    OrpheusDeviceModel device;
    std::shared_ptr<OrpheusProtocol> protocol;

    OrpheusControlRig() {
        rig.Target().SetDeviceModel(std::ref(device));
        protocol = std::make_shared<OrpheusProtocol>(rig.Bus(), rig.Bus(), rig.Route(),
                                                     nullptr, nullptr, &rig.Timers());
        protocol->UpdateRuntimeContext(rig.Route(), rig.Transport());
    }

    void Prepare() {
        IOReturn status = kIOReturnNotReady;
        protocol->Prepare([&](IOReturn result) { status = result; });
        rig.Drain(2048);
        ASSERT_EQ(status, kIOReturnSuccess);
        ASSERT_TRUE(protocol->CopySnapshot().has_value());
    }

    ~OrpheusControlRig() {
        protocol->Shutdown();
    }
};

} // namespace

TEST(OrpheusProtocolTests, PrepareQueriesAllGroupsAndPublishesSnapshot) {
    OrpheusControlRig r;
    r.Prepare();

    const auto snapshot = r.protocol->CopySnapshot();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->family, "orpheus");

    // Check Master Volume parameter (28 in big knob values is index 28: 0.0 dB)
    const auto volFound = snapshot->state.parameters.find(AM::ParameterId{OrpheusControl::OutputVolume});
    ASSERT_NE(volFound, snapshot->state.parameters.end());
    EXPECT_EQ(std::get<double>(volFound->second.value), 0.0);

    // Check Channel 1 Phantom parameter (ch 0 has phantom = true in mock)
    const auto phanFound = snapshot->state.parameters.find(AM::ParameterId{OrpheusControl::Phantom + 0});
    ASSERT_NE(phanFound, snapshot->state.parameters.end());
    EXPECT_TRUE(std::get<bool>(phanFound->second.value));

    // Check Channel 2 Phantom parameter (ch 1 has phantom = false in mock)
    const auto phan2Found = snapshot->state.parameters.find(AM::ParameterId{OrpheusControl::Phantom + 1});
    ASSERT_NE(phan2Found, snapshot->state.parameters.end());
    EXPECT_FALSE(std::get<bool>(phan2Found->second.value));

    // Check Channel 1 Gain (ch 0 micGain is 20 in mock)
    const auto gainFound = snapshot->state.parameters.find(AM::ParameterId{OrpheusControl::Gain + 0});
    ASSERT_NE(gainFound, snapshot->state.parameters.end());
    EXPECT_EQ(std::get<double>(gainFound->second.value), 20.0);

    // Total commands during prepare: 1 UnitState + 8 Channels + 6 MixBuses + 1 SignalSource = 16
    EXPECT_EQ(r.device.receivedCommands.size(), 16U);
}

TEST(OrpheusProtocolTests, PrepareIsIdempotent) {
    OrpheusControlRig r;
    r.Prepare();
    const size_t countAfterFirst = r.device.receivedCommands.size();

    IOReturn secondStatus = kIOReturnNotReady;
    r.protocol->Prepare([&](IOReturn result) { secondStatus = result; });
    r.rig.Drain(256);

    EXPECT_EQ(secondStatus, kIOReturnSuccess);
    EXPECT_EQ(r.device.receivedCommands.size(), countAfterFirst);
}

TEST(OrpheusProtocolTests, SetParameterMasterVolume) {
    OrpheusControlRig r;
    r.Prepare();

    IOReturn status = kIOReturnNotReady;
    AM::ParameterValue confirmed = false;
    r.protocol->SetParameter(AM::ParameterId{OrpheusControl::OutputVolume}, -6.0,
        [&](IOReturn res, AM::ParameterValue val) { status = res; confirmed = val; });
    r.rig.Drain(2048);

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_NEAR(std::get<double>(confirmed), -6.0, 0.5);

    const auto snapshot = r.protocol->CopySnapshot();
    ASSERT_TRUE(snapshot.has_value());
    const auto volFound = snapshot->state.parameters.find(AM::ParameterId{OrpheusControl::OutputVolume});
    ASSERT_NE(volFound, snapshot->state.parameters.end());
    EXPECT_NEAR(std::get<double>(volFound->second.value), -6.0, 0.5);
}

TEST(OrpheusProtocolTests, SetParameterPhantomPower) {
    OrpheusControlRig r;
    r.Prepare();

    // Channel 2 (index 1) currently phantom = false. Set to true.
    IOReturn status = kIOReturnNotReady;
    AM::ParameterValue confirmed = false;
    r.protocol->SetParameter(AM::ParameterId{OrpheusControl::Phantom + 1}, true,
        [&](IOReturn res, AM::ParameterValue val) { status = res; confirmed = val; });
    r.rig.Drain(2048);

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_TRUE(std::get<bool>(confirmed));
    EXPECT_TRUE(r.device.channels[1].phantom);

    const auto snapshot = r.protocol->CopySnapshot();
    ASSERT_TRUE(snapshot.has_value());
    const auto phanFound = snapshot->state.parameters.find(AM::ParameterId{OrpheusControl::Phantom + 1});
    ASSERT_NE(phanFound, snapshot->state.parameters.end());
    EXPECT_TRUE(std::get<bool>(phanFound->second.value));
}

TEST(OrpheusProtocolTests, SetParameterMixInputGain) {
    OrpheusControlRig r;
    r.Prepare();

    // Bus 0, Input 0 is parameter OrpheusControl::Mixer + 0
    IOReturn status = kIOReturnNotReady;
    AM::ParameterValue confirmed = false;
    r.protocol->SetParameter(AM::ParameterId{OrpheusControl::Mixer + 0}, -12.0,
        [&](IOReturn res, AM::ParameterValue val) { status = res; confirmed = val; });
    r.rig.Drain(2048);

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_NEAR(std::get<double>(confirmed), -12.0, 0.5);

    const auto snapshot = r.protocol->CopySnapshot();
    ASSERT_TRUE(snapshot.has_value());
    const auto mixFound = snapshot->state.parameters.find(AM::ParameterId{OrpheusControl::Mixer + 0});
    ASSERT_NE(mixFound, snapshot->state.parameters.end());
    EXPECT_NEAR(std::get<double>(mixFound->second.value), -12.0, 0.5);
}

TEST(OrpheusProtocolTests, SetRouteSwitchesInputType) {
    OrpheusControlRig r;
    r.Prepare();

    const auto before = *r.protocol->CopySnapshot();
    const auto router = AM::NodeId{OrpheusControl::InputRouter + 0}; // Channel 1
    const auto node = std::ranges::find(before.topology.nodes, router, &AM::Node::id);
    ASSERT_NE(node, before.topology.nodes.end());
    const auto& body = std::get<AM::RouterNode>(node->body);
    ASSERT_EQ(body.inputs.size(), 2U);

    // Switch to input 1 (Jack/Instrument)
    IOReturn status = kIOReturnNotReady;
    r.protocol->SetRoute(router, body.outputs.front(), body.inputs[1],
        [&](IOReturn res, AM::ParameterValue) { status = res; });
    r.rig.Drain(2048);

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_EQ(r.device.channels[0].inputType, 2U); // Instrument = 2

    const auto after = *r.protocol->CopySnapshot();
    const auto& active = after.state.routers.at(router).activeBundles;
    ASSERT_EQ(active.size(), 1U);
    EXPECT_EQ(active.front().value, 2U); // Bundle 2
}

TEST(OrpheusProtocolTests, QueueControlMaintainsStrictFifoOrder) {
    OrpheusControlRig r;
    r.Prepare();

    std::vector<int> completedOrder;
    r.protocol->SetParameter(AM::ParameterId{OrpheusControl::Phantom + 1}, true,
        [&](IOReturn, AM::ParameterValue) { completedOrder.push_back(1); });
    r.protocol->SetParameter(AM::ParameterId{OrpheusControl::Phantom + 2}, true,
        [&](IOReturn, AM::ParameterValue) { completedOrder.push_back(2); });
    r.protocol->SetParameter(AM::ParameterId{OrpheusControl::Phantom + 3}, true,
        [&](IOReturn, AM::ParameterValue) { completedOrder.push_back(3); });

    r.rig.Drain(4096);

    ASSERT_EQ(completedOrder.size(), 3U);
    EXPECT_EQ(completedOrder[0], 1);
    EXPECT_EQ(completedOrder[1], 2);
    EXPECT_EQ(completedOrder[2], 3);
}

TEST(OrpheusProtocolTests, WriteErrorRollsBackSpeculativeState) {
    OrpheusControlRig r;
    r.Prepare();

    const auto beforeSnapshot = *r.protocol->CopySnapshot();
    const double originalVol = std::get<double>(beforeSnapshot.state.parameters.at(AM::ParameterId{OrpheusControl::OutputVolume}).value);

    // Script mock rejection on next command
    r.device.rejectNext = true;

    IOReturn status = kIOReturnSuccess;
    r.protocol->SetParameter(AM::ParameterId{OrpheusControl::OutputVolume}, -30.0,
        [&](IOReturn res, AM::ParameterValue) { status = res; });
    r.rig.Drain(2048);

    EXPECT_NE(status, kIOReturnSuccess);

    const auto afterSnapshot = *r.protocol->CopySnapshot();
    const double rolledBackVol = std::get<double>(afterSnapshot.state.parameters.at(AM::ParameterId{OrpheusControl::OutputVolume}).value);
    EXPECT_EQ(rolledBackVol, originalVol);
}

TEST(OrpheusProtocolTests, UpdateRuntimeContextAbortsPendingAndResetsReady) {
    OrpheusControlRig r;
    r.Prepare();
    EXPECT_TRUE(r.protocol->CopySnapshot().has_value());

    // Rebind to new route
    ASFW::Discovery::DeviceRouteToken newRoute{};
    newRoute.guid = 0x0011980000000001ULL;
    newRoute.routeEpoch = 2;
    r.protocol->UpdateRuntimeContext(newRoute, r.rig.Transport());

    // Snapshot is now unavailable until re-prepared
    EXPECT_FALSE(r.protocol->CopySnapshot().has_value());

    // Re-prepare succeeds under new route
    IOReturn status = kIOReturnNotReady;
    r.protocol->Prepare([&](IOReturn res) { status = res; });
    r.rig.Drain(2048);

    EXPECT_EQ(status, kIOReturnSuccess);
    EXPECT_TRUE(r.protocol->CopySnapshot().has_value());
}
