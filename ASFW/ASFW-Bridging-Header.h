//
//  ASFW-Bridging-Header.h
//  ASFW
//
//  Bridging header to expose shared C ABI structs to Swift.
//

#ifndef ASFW_BRIDGING_HEADER_H
#define ASFW_BRIDGING_HEADER_H

#import "ASFWDiagnosticsABI.h"
#import "DVCaptureAtomics.h"
// Round-Trip Latency: the platform-neutral core and the shared CoreAudio session
// (tools/rtl). The IOProc stays in C; Swift only creates, runs and reads sessions.
#import "rtl_session.h"

#endif /* ASFW_BRIDGING_HEADER_H */
