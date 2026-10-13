// Development diagnostics; on-demand activation remains explicitly disabled.
#include "NativeDevelopmentIdentity.hpp"
#include "DevelopmentConnectionCommand.hpp"
#include "DevelopmentServerCommand.hpp"
#include <cstring>
int main(int argc,const char **argv) {
    @autoreleasepool {
        if(argc==2 && std::strcmp(argv[1],"--check-original-peer-policy")==0)
            return Pro40Lab::developmentOriginalPolicyCommand(Pro40Lab::DevelopmentRole::helper);
        if(argc==2 && std::strcmp(argv[1],"--check-verifier-listener")==0)
            return Pro40Lab::developmentServerCommand();
        if(argc==2 && std::strcmp(argv[1],"--serve-verifier")==0){
#if defined(PRO40_ENABLE_VERIFIER_EXPERIMENT) || defined(PRO40_ENABLE_ORIGINAL_PEER_DIAGNOSTIC)
            return Pro40Lab::developmentServeVerifierCommand();
#else
            // The package profile may be inspected now, but no unowned listener
            // may activate before lifecycle/failure cleanup integration is ready.
            fputs("on-demand verifier lifecycle is not enabled\n",stderr);return 78;
#endif
        }
        if(argc==2 && std::strcmp(argv[1],"--check-verifier-preparation")==0)
            return Pro40Lab::developmentConnectionCommand(true);
        if(argc==2 && std::strcmp(argv[1],"--check-development-identity")==0)
            return Pro40Lab::developmentIdentityCommand(Pro40Lab::DevelopmentRole::helper);
        return argc==1?0:64;
    }
}
