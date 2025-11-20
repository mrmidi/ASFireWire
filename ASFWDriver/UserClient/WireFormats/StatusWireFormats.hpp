//
//  StatusWireFormats.hpp
//  ASFWDriver
//
//  Wire format structures for controller status
//

#ifndef ASFW_USERCLIENT_STATUS_WIRE_FORMATS_HPP
#define ASFW_USERCLIENT_STATUS_WIRE_FORMATS_HPP

#include "WireFormatsCommon.hpp"

namespace ASFW::UserClient::Wire {

constexpr uint32_t kControllerStatusWireVersion = 1;

struct ControllerStatusFlags {
    static constexpr uint32_t kIsIRM = 1u << 0;
    static constexpr uint32_t kIsCycleMaster = 1u << 1;
};

struct ControllerStatusAsyncDescriptorWire {
    uint64_t descriptorVirt{0};
    uint64_t descriptorIOVA{0};
    uint32_t descriptorCount{0};
    uint32_t descriptorStride{0};
    uint32_t commandPtr{0};
    uint32_t reserved{0};
};
static_assert(sizeof(ControllerStatusAsyncDescriptorWire) == 32, "Async descriptor wire size mismatch");

struct ControllerStatusAsyncBuffersWire {
    uint64_t bufferVirt{0};
    uint64_t bufferIOVA{0};
    uint32_t bufferCount{0};
    uint32_t bufferSize{0};
};
static_assert(sizeof(ControllerStatusAsyncBuffersWire) == 24, "Async buffer wire size mismatch");

struct ControllerStatusAsyncWire {
    ControllerStatusAsyncDescriptorWire atRequest{};
    ControllerStatusAsyncDescriptorWire atResponse{};
    ControllerStatusAsyncDescriptorWire arRequest{};
    ControllerStatusAsyncDescriptorWire arResponse{};
    ControllerStatusAsyncBuffersWire arRequestBuffers{};
    ControllerStatusAsyncBuffersWire arResponseBuffers{};
    uint64_t dmaSlabVirt{0};
    uint64_t dmaSlabIOVA{0};
    uint32_t dmaSlabSize{0};
    uint32_t reserved{0};
};
static_assert(sizeof(ControllerStatusAsyncWire) == 200, "Async status wire size mismatch");

struct ControllerStatusWire {
    uint32_t version{0};
    uint32_t flags{0};
    char stateName[32]{};
    uint32_t generation{0};
    uint32_t nodeCount{0};
    uint32_t localNodeID{0xFFFFFFFFu};
    uint32_t rootNodeID{0xFFFFFFFFu};
    uint32_t irmNodeID{0xFFFFFFFFu};
    uint64_t busResetCount{0};
    uint64_t lastBusResetTime{0};
    uint64_t uptimeNanoseconds{0};
    ControllerStatusAsyncWire async{};
};
static_assert(sizeof(ControllerStatusWire) == 288, "ControllerStatusWire size mismatch");

} // namespace ASFW::UserClient::Wire

#endif // ASFW_USERCLIENT_STATUS_WIRE_FORMATS_HPP
