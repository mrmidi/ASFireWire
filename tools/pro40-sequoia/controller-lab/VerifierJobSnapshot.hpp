#include "LabIdentity.hpp"
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "JobObservation.hpp"
namespace Pro40Lab {
struct VerifierJobSnapshot {
    bool running=false;
    uint64_t runs=0;
    std::optional<int32_t> pid;
    // Historical when running; never evidence that the current instance exited.
    std::optional<unsigned> lastExitCode;
};
// Structural parsing does not establish job ownership.
struct VerifierJobFields {
    std::map<std::string,std::string> values;
    std::vector<std::string> arguments;
    bool submittedAnnotation=false;
};
inline std::optional<VerifierJobFields> parseVerifierJobFields(std::string_view text,
    std::string_view target,bool submitted=false) {
    auto clean=[](std::string_view s){
        if(s.empty())return false;
        for(unsigned char c:s)if(c<32 || c==127)return false;
        return true;
    };
    if(!clean(target)||text.empty()||text.size()>16384||text.back()!='\n')return {};
    for(unsigned char c:text)if((c<32 && c!='\n' && c!='\t')||c==127)return {};
    auto end=text.find('\n');
    if(text.substr(0,end)!=std::string(target)+" = {")return {};
    size_t cursor=end+1,depth=1;
    bool closed=false;
    VerifierJobFields result;auto &fields=result.values;std::string section;
    while(cursor<text.size()) {
        end=text.find('\n',cursor);if(end==std::string_view::npos)return {};
        auto line=text.substr(cursor,end-cursor);cursor=end+1;
        if(line.empty())continue;
        if(closed)return {};
        size_t indent=0;while(indent<line.size() && line[indent]=='\t')++indent;
        auto body=line.substr(indent);
        if(body=="}"){
            if(!depth||indent!=depth-1)return {};
            --depth;if(!depth)closed=true;
            continue;
        }
        if(indent!=depth || body.empty())return {};
        if(depth==1 && body=="submitted job. ignore execute allowed"){
            if(!submitted || result.submittedAnnotation)return {};
            result.submittedAnnotation=true;continue;
        }
        if(depth==2 && section=="arguments")result.arguments.emplace_back(body);
        const bool block=body.ends_with(" = {");
        if(body.find('}')!=std::string_view::npos ||
            (body.find('{')!=std::string_view::npos && (!block || body.find('{')!=body.size()-1)))return {};
        if(depth==1){
            auto split=body.find(" = ");if(split==std::string_view::npos || split==0)return {};
            std::string key(body.substr(0,split)),value(body.substr(split+3));
            if(value.empty() || !fields.emplace(key,value).second)return {};
            section=block?key:"";
            // Any signal-labelled field is outside ordinary-exit evidence.
            if(key.find("signal")!=std::string::npos)return {};
        }
        if(block && ++depth>16)return {};
    }
    if(!closed || depth)return {};
    return result;
}
inline std::optional<VerifierJobSnapshot> verifierJobState(std::map<std::string,std::string> fields,bool neverExited=false){
    auto number=[](std::string_view s,uint64_t limit)->std::optional<uint64_t>{
        if(s.empty() || (s.size()>1 && s[0]=='0'))return {};
        for(char c:s)if(c<'0'||c>'9')return {};
        uint64_t n=0;auto r=std::from_chars(s.data(),s.data()+s.size(),n);
        if(r.ec!=std::errc{} || r.ptr!=s.data()+s.size() || n>limit)return {};
        return n;
    };
    VerifierJobSnapshot result;
    const auto state=fields.find("state");
    if(state==fields.end() || (state->second!="running" && state->second!="not running"))return {};
    result.running=state->second=="running";
    auto runs=number(fields["runs"],INT64_MAX);if(!runs)return {};result.runs=*runs;
    if(auto it=fields.find("pid");it!=fields.end()){
        auto pid=number(it->second,INT32_MAX);if(!pid || !*pid)return {};
        result.pid=static_cast<int32_t>(*pid);
    }
    if(result.running!=result.pid.has_value() || (result.running && !result.runs))return {};
    if(auto it=fields.find("last exit code");it!=fields.end()){
        // Native first-running evidence: pro40-diagnostic-verifier-2026-10-05/ready-job.txt.
        // No exit history on first run is not a current-process exit receipt.
        if(neverExited && it->second=="(never exited)" &&
           ((!result.running && !result.runs) || (result.running && result.runs==1)))return result;
        auto code=number(it->second,255);if(!code || !result.runs)return {};
        result.lastExitCode=static_cast<unsigned>(*code);
    }
    return result;
}
// Legacy LaunchAgent evidence retains exact absolute program/plist checks.
inline std::optional<VerifierJobSnapshot> parseVerifierJobSnapshot(std::string_view text,
    std::string_view target,std::string_view program,std::string_view plist){
    auto parsed=parseVerifierJobFields(text,target);
    if(!parsed || program.empty() || plist.empty())return {};
    for(auto path:{program,plist})for(unsigned char c:path)if(c<32 || c==127)return {};
    const auto &f=parsed->values;
    if(!f.contains("type") || f.at("type")!="LaunchAgent" || !f.contains("program") || f.at("program")!=program ||
       !f.contains("path") || f.at("path")!=plist)return {};
    return verifierJobState(f);
}
// Submitted jobs lack absolute package paths. This is an UNBOUND observation;
// never feed it into launch permission without independent BTM/package binding.
// Captured source: validation/pro40-first-native-verifier-2026-10-05/job-after.json.
// Native evidence covers zero-run dormant and first-run running "never exited" forms.
struct SubmittedVerifierJobSnapshot {
    VerifierJobSnapshot state;
    std::string btmUUID;
};
inline std::optional<SubmittedVerifierJobSnapshot> parseSubmittedVerifierJobSnapshot(std::string_view text,
    std::string_view target){
    auto parsed=parseVerifierJobFields(text,target,true);if(!parsed || !parsed->submittedAnnotation)return {};
    auto &f=parsed->values;
    if(f["type"]!="Submitted" || f["managed_by"]!="com.apple.xpc.ServiceManagement" ||
       f["parent bundle identifier"]!=PRO40_LAB_APP_ID ||
       f["program identifier"]!="Contents/MacOS/Pro40VerifierLab (mode: 2)" || f.contains("program") ||
       parsed->arguments!=std::vector<std::string>{"Pro40VerifierLab","--serve-verifier"})return {};
    const std::string prefix="(submitted by smd.";const auto &path=f["path"];
    if(!path.starts_with(prefix) || !path.ends_with(")"))return {};
    auto pid=std::string_view(path).substr(prefix.size(),path.size()-prefix.size()-1);
    uint32_t value=0;auto converted=std::from_chars(pid.data(),pid.data()+pid.size(),value);
    if(pid.empty() || pid.front()=='0' || converted.ec!=std::errc{} || converted.ptr!=pid.data()+pid.size() || !value || value>INT32_MAX)return {};
    const auto &uuid=f["BTM uuid"];if(uuid.size()!=36)return {};
    for(size_t i=0;i<36;++i){char c=uuid[i];
        if(i==8||i==13||i==18||i==23){if(c!='-')return {};}
        else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return {};
    }
    auto state=verifierJobState(f,true);if(!state)return {};
    return SubmittedVerifierJobSnapshot{*state,uuid};
}
// Only stdout from a successfully reaped, bounded observation is eligible.
// This does not establish freshness, process birth, same run or remote exit.
inline std::optional<VerifierJobSnapshot> verifierJobSnapshot(const JobProcessResult& observation,
    std::string_view target,std::string_view program,std::string_view plist) {
    if(!observation.usable() || observation.exit!=0 || !observation.err.empty())return {};
    return parseVerifierJobSnapshot(observation.out,target,program,plist);
}
}
