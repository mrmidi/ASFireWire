// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../NativeChildProcess.hpp"
#include "../SessionRunResult.hpp"
#include <array>
#include <charconv>
#include <span>
#include <string_view>
namespace Pro40 {
// Bounded reader for NativeWorker's two JSON lines. Not a general JSON parser:
// field order/JSON whitespace may vary, but strings use the producer's literal
// ASCII spelling (no escapes). No IO, authentication, allocation or recursion.
// Controller must feed only its owned worker's protocol channel, call end only
// on observed EOF, and pass that same retained child's wait record to result.
class WorkerResultProtocol final {
    struct Reader {
        std::string_view text;
        void space(){while(!text.empty() && (text[0]==' '||text[0]=='\t'||text[0]=='\r'||text[0]=='\n'))text.remove_prefix(1);}
        bool take(char c){space();if(text.empty()||text[0]!=c)return false;text.remove_prefix(1);return true;}
        bool string(std::string_view &value){
            if(!take('"'))return false;size_t n=0;
            while(n<text.size() && text[n]!='"'){
                const auto c=static_cast<unsigned char>(text[n]);if(c<32||c>126||c=='\\')return false;++n;
            }
            if(n==text.size())return false;value=text.substr(0,n);text.remove_prefix(n+1);return true;
        }
        bool integer(int32_t &value){
            space();if(text.empty())return false;
            const auto original=text;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
            if(parsed.ec!=std::errc{} || parsed.ptr==text.data())return false;
            const size_t n=size_t(parsed.ptr-text.data());
            const size_t digits=original[0]=='-'?1:0;
            if(n>digits+1 && original[digits]=='0')return false;
            text.remove_prefix(n);return true;
        }
        bool outcome(std::optional<SessionOutcome>& value){
            space();if(text.starts_with("null")){text.remove_prefix(4);value.reset();return true;}
            if(!take('{'))return false;SessionOutcome result;unsigned seen=0;
            for(;;){
                std::string_view key;if(!string(key)||!take(':'))return false;unsigned bit=0;
                if(key=="reason"){
                    bit=1;std::string_view reason;if(!string(reason))return false;
                    if(reason=="completed")result.reason=StopReason::completed;
                    else if(reason=="interrupted")result.reason=StopReason::interrupted;
                    else if(reason=="error")result.reason=StopReason::error;
                    else if(reason=="watchdog")result.reason=StopReason::watchdog;
                    else if(reason=="invalid-notification")result.reason=StopReason::invalidNotification;
                    else return false;
                }else if(key=="transfer"){bit=2;if(!integer(result.transfer))return false;}
                else if(key=="evidence"){bit=4;if(!integer(result.evidence))return false;}
                else if(key=="cleanup"){bit=8;if(!integer(result.cleanup))return false;}
                else return false;
                if(seen&bit)return false;seen|=bit;
                if(take('}'))break;if(!take(','))return false;
            }
            if(seen!=15)return false;value=result;return true;
        }
    };
    std::array<char,4096> line_{};
    size_t used_=0,total_=0;
    unsigned frames_=0;
    bool failed_=false,eof_=false;
    NativeSessionResult result_;
    bool frame(std::string_view line){
        Reader reader{line};if(!reader.take('{'))return false;
        unsigned seen=0;int32_t version=0,code=0;std::string_view type;
        std::optional<SessionOutcome> outcome;
        for(;;){
            std::string_view key;if(!reader.string(key)||!reader.take(':'))return false;unsigned bit=0;
            if(key=="version"){bit=1;if(!reader.integer(version))return false;}
            else if(key=="type"){bit=2;if(!reader.string(type))return false;}
            else if(key=="exit"){bit=4;if(!reader.integer(code))return false;}
            else if(key=="outcome"){bit=8;if(!reader.outcome(outcome))return false;}
            else return false;
            if(seen&bit)return false;seen|=bit;
            if(reader.take('}'))break;if(!reader.take(','))return false;
        }
        reader.space();if(!reader.text.empty()||version!=1)return false;
        if(frames_==0)return type=="ready" && seen==3;
        if(frames_!=1 || type!="result" || seen!=15)return false;
        if(outcome){if(code!=(outcome->accepted()?0:2))return false;}
        // Gated worker pre-entry cancellation/IO errors can also be 2/74.
        // A null outcome never establishes native cleanup or accepted audio.
        else if(code!=1 && code!=2 && code!=64 && code!=74 && code!=75)return false;
        result_={code,outcome};return true;
    }
public:
    void feed(std::span<const char> bytes)noexcept{
        if(bytes.empty())return;
        if(eof_ || bytes.size()>line_.size()-total_){failed_=true;return;}
        total_+=bytes.size();if(failed_)return;
        for(char c:bytes){
            if(c=='\n'){
                if(!frame({line_.data(),used_})){failed_=true;return;}
                ++frames_;used_=0;
            }else line_[used_++]=c;
        }
    }
    void end()noexcept{eof_=true;if(used_ || frames_!=2)failed_=true;}
    bool failed()const noexcept{return failed_;}
    [[nodiscard]] std::optional<NativeSessionResult> result(const Pro40Audio::NativeChildProcess &child)const noexcept{
        const int status=child.exitStatus();
        if(failed_ || !eof_ || frames_!=2 || child.state()!=Pro40Audio::NativeChildProcess::State::reaped ||
           child.waitError() || child.signalError() || !WIFEXITED(status) ||
           WEXITSTATUS(status)!=result_.exitCode)return {};
        return result_;
    }
};
}
