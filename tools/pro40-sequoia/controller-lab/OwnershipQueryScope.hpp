// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "VerifierBTMRecords.hpp"
#include <optional>
#include <string>
namespace Pro40Lab {
// Validate the captured format before selecting two records. Return only fields
// consumed by the existing ownership binder; no other users/apps reach the peer.
inline std::optional<std::string> scopedOwnershipText(const std::string &raw,uid_t uid,
                                                      NSString *appID,NSString *label) {
    if(raw.size()>1048576 || !appID.length || !label.length)return {};
    NSString *text=[[NSString alloc] initWithBytes:raw.data() length:raw.size() encoding:NSUTF8StringEncoding];
    NSArray *records=verifierBTMRecords(text,@(uid));if(!records)return {};
    NSString *parentID=[@"2." stringByAppendingString:appID],*childID=[@"8." stringByAppendingString:label];
    NSDictionary *parent=nil,*child=nil;
    for(NSDictionary *record in records) {
        if([record[@"Identifier"] isEqual:parentID]){if(parent)return {};parent=record;}
        if([record[@"Identifier"] isEqual:childID]){if(child)return {};child=record;}
    }
    if(!parent || !child)return {};
    NSString *header=nil,*prefix=[NSString stringWithFormat:@" Records for UID %u : ",uid];
    for(NSString *line in [text componentsSeparatedByString:@"\n"])if([line hasPrefix:prefix]){if(header)return {};header=line;}
    if(!header)return {};
    NSMutableString *selected=[header mutableCopy];
    NSArray *keys=@[@[@"Identifier",@"Type",@"URL",@"Bundle Identifier"],
                    @[@"Identifier",@"Type",@"Parent Identifier",@"UUID",@"URL",@"Executable Path"]];
    NSArray *chosen=@[parent,child];
    for(unsigned i=0;i<2;++i) {
        [selected appendFormat:@"\n #%u:\n",i+1];
        for(NSString *key in keys[i]) {
            NSString *value=chosen[i][key];if(![value isKindOfClass:NSString.class] || !value.length)return {};
            [selected appendFormat:@"    %@: %@\n",key,value];
        }
    }
    NSData *encoded=[selected dataUsingEncoding:NSUTF8StringEncoding];
    if(!encoded || !encoded.length || encoded.length>16384)return {};
    return std::string(static_cast<const char *>(encoded.bytes),encoded.length);
}
}
