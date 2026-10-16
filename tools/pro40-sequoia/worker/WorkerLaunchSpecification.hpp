// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../NativeSessionRunner.hpp"
#include <cstdio>
#include <optional>
#include <vector>
#include <array>
#include <sys/un.h>
namespace Pro40 {
// Owned controller-local values, not an authenticated wire format. Child-owned
// buffers and callbacks must never be placed in this cross-process description.
class WorkerLaunchSpecification final {
public:
    struct Duplex {
        std::string service;
        uint64_t epoch=0,nonce=0,seed=0;
        uint32_t generation=0;
    };
    enum class Operation {session,recordInput,recordTiming,recordPostRelease,recordQuietHold,recordOwnedIdle,recordAssignedIdle,recordExistingClock,recordEnableFirst,recordClockExcursion};
private:
    uint64_t guid_;
    std::string logPath_;
    Operation operation_;
    std::optional<Duplex> duplex_;
    WorkerLaunchSpecification(uint64_t guid,std::string path,Operation operation):
        guid_(guid),logPath_(std::move(path)),operation_(operation){}
public:
    [[nodiscard]] static std::optional<WorkerLaunchSpecification> create(
        uint64_t guid,std::string path,Operation operation=Operation::session){
        if(!guid || path.empty() || path.front()!='/' || path.back()=='/' ||
           path.find('\0')!=std::string::npos)return std::nullopt;
        switch(operation){
        case Operation::session:case Operation::recordInput:case Operation::recordTiming:case Operation::recordPostRelease:case Operation::recordQuietHold:case Operation::recordOwnedIdle:case Operation::recordAssignedIdle:case Operation::recordExistingClock:case Operation::recordEnableFirst:case Operation::recordClockExcursion:break;
        default:return std::nullopt;
        }
        return WorkerLaunchSpecification(guid,std::move(path),operation);
    }
    [[nodiscard]] static std::optional<WorkerLaunchSpecification> createDuplex(uint64_t guid,std::string path,Duplex duplex){
        auto result=create(guid,std::move(path));
        if(!result || duplex.service.empty() || duplex.service.size()>=128 || duplex.service.find('\0')!=std::string::npos ||
           !duplex.epoch || !duplex.nonce || !duplex.seed)return {};
        result->duplex_=std::move(duplex);return result;
    }
    [[nodiscard]] std::vector<std::string> arguments()const{
        char guid[17]{};std::snprintf(guid,sizeof(guid),"%016llx",static_cast<unsigned long long>(guid_));
        std::vector<std::string> result;
        if(duplex_)result={"--duplex-session",duplex_->service,std::to_string(duplex_->epoch),std::to_string(duplex_->nonce),
            std::to_string(duplex_->seed),std::to_string(duplex_->generation)};
        if(operation_==Operation::recordInput)result.emplace_back("--record-input");
        if(operation_==Operation::recordTiming)result.emplace_back("--record-timing");
        if(operation_==Operation::recordClockExcursion)result.emplace_back("--record-clock-excursion");
        if(operation_==Operation::recordEnableFirst)result.emplace_back("--record-enable-first");
        if(operation_==Operation::recordExistingClock)result.emplace_back("--record-existing-clock");
        if(operation_==Operation::recordAssignedIdle)result.emplace_back("--record-assigned-idle");
        if(operation_==Operation::recordOwnedIdle)result.emplace_back("--record-owned-idle");
        if(operation_==Operation::recordQuietHold)result.emplace_back("--record-quiet-hold");
        if(operation_==Operation::recordPostRelease)result.emplace_back("--record-post-release");
        result.insert(result.end(),{"--guid",guid,logPath_});return result;
    }
    [[nodiscard]] std::optional<std::vector<std::string>> managedArguments(
        const std::array<uint8_t,16>& nonce,const std::string& endpoint)const{
        bool nonzero=false;for(auto byte:nonce)nonzero|=byte!=0;
        sockaddr_un address{};
        if(!nonzero || endpoint.empty() || endpoint.front()!='/' ||
           endpoint.size()>=sizeof(address.sun_path) || endpoint.find('\0')!=std::string::npos)return {};
        char encoded[33]{};for(size_t i=0;i<nonce.size();++i)std::snprintf(encoded+2*i,3,"%02x",nonce[i]);
        std::vector<std::string> result{"--managed",encoded,endpoint};
        auto tail=arguments();result.insert(result.end(),tail.begin(),tail.end());return result;
    }
    // Metadata for controller-side identity binding, never a runnable substitute
    // for child-side recording/timing allocation. Stop/context remain child-owned.
    [[nodiscard]] NativeSessionOptions identityOptions()const{
        NativeSessionOptions result;result.guid=guid_;result.logPath=logPath_;
        result.retainHistory=false;return result;
    }
    Operation operation()const noexcept{return operation_;}
    const auto &duplex()const noexcept{return duplex_;}
};
}
