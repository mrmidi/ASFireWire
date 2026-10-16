// SPDX-License-Identifier: Apache-2.0
// Lab only: private inherited pipes; source supplies its real kernel audit token.
// Executable selection and policy are controlled by the local test harness.
#include "NativePeerIdentity.hpp"
#include "NativeAuditTrailer.hpp"
#include "NativeMachOwnership.hpp"
#include "NativeIdentityCodec.hpp"
#include <cerrno>
#include <cstdlib>
#include <cassert>
#include <csignal>
#include <string>
#include <unistd.h>
using namespace Pro40Audio;
static void hang(){for(;;)pause();}
static bool writeAll(const uint8_t *data,size_t size){
    while(size){const auto n=write(STDOUT_FILENO,data,size);if(n<0 && errno==EINTR)continue;
        if(n<=0)return false;data+=n;size-=n;}return true;
}
int main(int argc,char **argv){
    assert(argc==3);const std::string mode=argv[1],pin=argv[2];
    assert(pin.size()==40 && pin.find_first_not_of("0123456789abcdef")==std::string::npos);
    if(mode=="hang"){signal(SIGTERM,SIG_IGN);hang();}
    if(mode=="crash")_Exit(17);
    if(mode=="truncated"){assert(write(STDOUT_FILENO,"x",1)==1);return 0;}
    if(mode=="oversized"){std::array<uint8_t,identityResultSize+1> bytes{};assert(writeAll(bytes.data(),bytes.size()));hang();}
    if(mode=="source"){
        NativeMachPort port(MACH_PORT_RIGHT_RECEIVE);assert(port.allocate()==KERN_SUCCESS);
        assert(mach_port_insert_right(mach_task_self(),port.get(),port.get(),MACH_MSG_TYPE_MAKE_SEND)==KERN_SUCCESS);
        NativeMachPort sender(MACH_PORT_RIGHT_SEND);assert(sender.adopt(port.get())==KERN_SUCCESS);
        mach_msg_header_t h{};h.msgh_size=sizeof(h);h.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,0);
        h.msgh_remote_port=sender.get();
        assert(mach_msg(&h,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(h),0,MACH_PORT_NULL,100,MACH_PORT_NULL)==KERN_SUCCESS);
        NativeMachReceived<> received;assert(received.read(port.get(),100,nativeAuditReceiveOptions));
        const auto token=nativeAuditToken(received.bytes());assert(token);
        NativeIdentityRequest request;request.token=*token;request.role=1;request.guid=42;request.epoch=7;
        arc4random_buf(request.requestID.data(),request.requestID.size());
        for(size_t i=0;i<20;++i)request.policyHash[i]=std::stoul(pin.substr(i*2,2),nullptr,16);
        const auto bytes=encodeIdentityRequest(request);assert(writeAll(bytes.data(),bytes.size()));
        char c;while(read(STDIN_FILENO,&c,1)<0 && errno==EINTR){}
        assert(sender.close()==KERN_SUCCESS && port.close()==KERN_SUCCESS);return 0;
    }
    // Read at most size+1, requiring EOF so trailing/duplicated requests fail.
    // Blocking input is confined to this killable process, never the controller.
    std::array<uint8_t,identityRequestSize+1> input{};size_t used=0;
    while(used<input.size()){
        const auto n=read(STDIN_FILENO,input.data()+used,input.size()-used);
        if(n<0 && errno==EINTR)continue;if(n<0)return 2;if(!n)break;used+=n;
    }
    const auto request=decodeIdentityRequest(std::span(input).first(used));if(!request)return 2;
    std::string hash;constexpr char hex[]="0123456789abcdef";
    for(auto byte:request->policyHash){hash+=hex[byte>>4];hash+=hex[byte&15];}
    const auto expression="cdhash H\""+hash+"\"";
    IdentityDetail::Owned<CFStringRef> text(CFStringCreateWithCString(nullptr,expression.c_str(),kCFStringEncodingUTF8));
    SecRequirementRef raw=nullptr;
    assert(SecRequirementCreateWithString(text.get(),kSecCSDefaultFlags,&raw)==errSecSuccess);
    IdentityDetail::Owned<SecRequirementRef> requirement(raw);
    NativeIdentityResult result{1,*request,verifyNativePeerIdentity(request->token,requirement.get())};
    if(mode=="wrong-session")++result.request.epoch;
    if(mode=="wrong-token")result.request.token.val[0]^=1;
    if(mode=="wrong-policy")result.request.policyHash[0]^=1;
    if(mode=="wrong-request")result.request.requestID[0]^=1;
    if(mode=="wrong-role")result.request.role=2;
    const auto bytes=encodeIdentityResult(result);assert(writeAll(bytes.data(),bytes.size()));
    if(mode=="duplicate")assert(writeAll(bytes.data(),bytes.size()));
    if(mode=="result-then-hang")hang();
    if(mode=="result-then-crash")_Exit(18);
    return 0;
}
