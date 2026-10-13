// SPDX-License-Identifier: Apache-2.0
#pragma once
#import <Foundation/Foundation.h>
namespace Pro40Lab {
// Captured format: validation/pro40-known-dormant-removal-2026-10-05/btm-verifier-only.txt.
inline NSArray *verifierBTMRecords(NSString *text,NSNumber *uid){
    if(!text || text.length>1048576 || [text rangeOfString:[NSString stringWithFormat:@"%C",(unichar)0]].location!=NSNotFound)return nil;
    NSRegularExpression *header=[NSRegularExpression regularExpressionWithPattern:@"^ Records for UID (-?[0-9]+) : [0-9A-Fa-f-]{36}$" options:0 error:nil];
    NSRegularExpression *item=[NSRegularExpression regularExpressionWithPattern:@"^ #[1-9][0-9]*:$" options:0 error:nil];
    NSMutableArray *records=[NSMutableArray array];NSMutableDictionary *record=nil;
    unsigned sections=0;bool selected=false;
    for(NSString *line in [text componentsSeparatedByString:@"\n"]){
        auto match=[header firstMatchInString:line options:0 range:NSMakeRange(0,line.length)];
        if(match){
            if(record){[records addObject:record];record=nil;}
            selected=[[line substringWithRange:[match rangeAtIndex:1]] isEqual:uid.stringValue];
            if(selected && ++sections>1)return nil;
            continue;
        }
        if([line hasPrefix:@" Records for UID "])return nil;
        if(!selected)continue;
        if([item firstMatchInString:line options:0 range:NSMakeRange(0,line.length)]){
            if(record)[records addObject:record];record=[NSMutableDictionary dictionary];continue;
        }
        if(!record)continue;
        NSString *trim=[line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSRange split=[trim rangeOfString:@": "];
        if(split.location==NSNotFound)continue;
        NSString *key=[trim substringToIndex:split.location],*value=[trim substringFromIndex:split.location+2];
        if(!key.length || !value.length || record[key])return nil;
        record[key]=value;
    }
    if(record)[records addObject:record];return sections==1?records:nil;
}
}
