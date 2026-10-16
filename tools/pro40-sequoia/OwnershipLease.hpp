// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace Pro40Resources {
// Linux dice-interface.h:49-57: OWNER changes by owner CAS and clears on reset.
// dice-transaction.c:170-237 binds claim/release to a single bus generation.
// This is not a defense against a client violating that register contract.
class OwnershipLease {
    uint32_t generation_=0;
    bool attempted_=false,held_=false,revoked_=false;
public:
    bool grant(uint32_t generation,bool verified) {
        if(attempted_ || revoked_) {revoke();return false;}
        attempted_=true;generation_=generation;held_=verified;
        if(!verified)revoked_=true;
        return held_;
    }
    bool check(uint32_t generation,bool healthy) {
        if(!held_ || revoked_ || !healthy || generation!=generation_) {
            revoke();return false;
        }
        return true;
    }
    void revoke() {held_=false;revoked_=true;}
};
}
