#include "LabIdentity.hpp"
// SPDX-License-Identifier: Apache-2.0
#include "RegistrationJournal.hpp"
#include "VerifierJobSnapshot.hpp"
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>

namespace Pro40Lab {
namespace {
constexpr size_t maxRecord = 65536;
struct FD {
    int value;
    explicit FD(int fd) : value(fd) {}
    ~FD() { if (value >= 0) close(value); }
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;
};
bool safe(int fd, bool directory = false) {
    struct stat s{};
    return fstat(fd, &s) == 0 && s.st_uid == geteuid() &&
        (s.st_mode & 07777) == (directory ? 0700 : 0600) &&
        (directory ? S_ISDIR(s.st_mode) : S_ISREG(s.st_mode) && s.st_nlink == 1);
}
bool keys(id value, NSArray *expected) {
    return [value isKindOfClass:[NSDictionary class]] &&
        [[NSSet setWithArray:[value allKeys]] isEqual:[NSSet setWithArray:expected]];
}
bool text(id value) {
    return [value isKindOfClass:[NSString class]] && [value length] > 0 &&
        [value length] <= 4096 && [value rangeOfString:@"\0"].location == NSNotFound;
}
bool hex(id value, NSUInteger count) {
    if (![value isKindOfClass:[NSString class]] || [value length] != count) return false;
    return [value rangeOfCharacterFromSet:
        [[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location == NSNotFound;
}
bool boolean(id value) {
    return value && CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID();
}
bool integer(id value) {
    if (![value isKindOfClass:[NSNumber class]] || boolean(value)) return false;
    const char *type = [value objCType];
    return type && strchr("cCsSiIlLqQ", type[0]) && type[1] == 0;
}
bool metadataValid(NSDictionary *m) {
    if (!keys(m, @[@"uid", @"domain", @"label", @"plist", @"bundle_path",
                  @"controller_id", @"helper_id", @"controller_cdhash", @"helper_cdhash",
                  @"plist_sha256", @"pid", @"boot", @"start_seconds", @"start_microseconds"])) return false;
    if (!integer(m[@"uid"]) || [m[@"uid"] unsignedLongLongValue] != geteuid() ||
        ![m[@"domain"] isEqual:[NSString stringWithFormat:@"gui/%u", geteuid()]] ||
        ![m[@"label"] isEqual:@PRO40_LAB_SERVICE_ID] ||
        ![m[@"plist"] isEqual:@PRO40_LAB_PLIST] ||
        ![m[@"controller_id"] isEqual:@PRO40_LAB_APP_ID] ||
        ![m[@"helper_id"] isEqual:m[@"label"]]) return false;
    NSString *path = m[@"bundle_path"];
    return text(path) && [path isAbsolutePath] &&
        [path isEqual:[path stringByStandardizingPath]] &&
        hex(m[@"controller_cdhash"], 40) && hex(m[@"helper_cdhash"], 40) &&
        hex(m[@"plist_sha256"], 64) && text(m[@"boot"]) &&
        [[NSUUID alloc] initWithUUIDString:m[@"boot"]] != nil &&
        integer(m[@"pid"]) && [m[@"pid"] longLongValue] > 0 &&
        integer(m[@"start_seconds"]) && [m[@"start_seconds"] longLongValue] > 0 &&
        integer(m[@"start_microseconds"]) && [m[@"start_microseconds"] longLongValue] >= 0 &&
        [m[@"start_microseconds"] longLongValue] < 1000000;
}
bool resultValid(NSDictionary *e) {
    if (!keys(e, @[@"success", @"error_domain", @"error_code", @"raw_status", @"ownership_conflict"]) ||
        !boolean(e[@"success"]) || !boolean(e[@"ownership_conflict"]) ||
        !integer(e[@"error_code"]) || !integer(e[@"raw_status"]) ||
        ![e[@"error_domain"] isKindOfClass:[NSString class]]) return false;
    return [e[@"success"] boolValue] ?
        [e[@"error_domain"] length] == 0 && [e[@"error_code"] longLongValue] == 0 : text(e[@"error_domain"]);
}
bool preflightValid(NSDictionary *p, NSDictionary *metadata) {
    if (!keys(p, @[@"identity", @"raw_status", @"experiment", @"package_verified",
                  @"observations_current", @"elapsed_ns", @"provenance", @"job"]) ||
        ![p[@"identity"] isEqual:metadata] || !integer(p[@"raw_status"]) ||
        !boolean(p[@"package_verified"]) || ![p[@"package_verified"] boolValue] ||
        !boolean(p[@"observations_current"]) || ![p[@"observations_current"] boolValue] ||
        !integer(p[@"elapsed_ns"]) || [p[@"elapsed_ns"] longLongValue] < 0 ||
        [p[@"elapsed_ns"] unsignedLongLongValue] > 5000000000ULL ||
        !text(p[@"provenance"])) return false;
    const auto status = [p[@"raw_status"] longLongValue];
    if (!((status == 0 && [p[@"experiment"] isEqual:@"ordinary"]) ||
          (status == 3 && [p[@"experiment"] isEqual:@"reviewed-first-use"]))) return false;
    NSDictionary *job = p[@"job"];
    if (!keys(job, @[@"argv", @"exit", @"stdout", @"stderr"]) ||
        !integer(job[@"exit"]) || [job[@"exit"] longLongValue] != 113 ||
        ![job[@"stdout"] isEqual:@""]) return false;
    NSArray *argv = @[@"/bin/launchctl", @"print", [NSString stringWithFormat:@"%@/%@",
        metadata[@"domain"], metadata[@"label"]]];
    // Exact locally observed absence grammar. Other outputs remain unknown.
    NSString *error = [NSString stringWithFormat:
        @"Bad request.\nCould not find service \"%@\" in domain for user gui: %@\n",
        metadata[@"label"], metadata[@"uid"]];
    return [job[@"argv"] isEqual:argv] && [job[@"stderr"] isEqual:error];
}
NSString *recordHash(NSDictionary *record) {
    NSData *bytes = [NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingSortedKeys error:nil];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH]{};
    CC_SHA256(bytes.bytes, static_cast<CC_LONG>(bytes.length), digest);
    NSMutableString *hash = [NSMutableString string];
    for (auto byte : digest) [hash appendFormat:@"%02x", byte];
    return hash;
}
bool recoveryValid(NSDictionary *prefix, NSDictionary *evidence) {
    if (!keys(evidence, @[@"record_sha256", @"observation", @"original_owner_gone",
                         @"no_inflight_operations", @"review_reference"]) ||
        ![evidence[@"record_sha256"] isEqual:recordHash(prefix)] ||
        !boolean(evidence[@"original_owner_gone"]) || ![evidence[@"original_owner_gone"] boolValue] ||
        !boolean(evidence[@"no_inflight_operations"]) || ![evidence[@"no_inflight_operations"] boolValue] ||
        !text(evidence[@"review_reference"])) return false;
    NSDictionary *observation = evidence[@"observation"];
    if (![observation isKindOfClass:[NSDictionary class]]) return false;
    NSDictionary *current = observation[@"identity"], *old = prefix[@"metadata"];
    if (!metadataValid(current) || !preflightValid(observation, current) ||
        ![observation[@"experiment"] isEqual:@"ordinary"] ||
        [observation[@"raw_status"] longLongValue] != 0) return false;
    // Same exact package and user/domain. Recovery observer has a different
    // process instance; neither this comparison nor a boolean proves liveness.
    for (NSString *key in old) {
        if ([@[@"pid", @"boot", @"start_seconds", @"start_microseconds"] containsObject:key]) continue;
        if (![old[key] isEqual:current[key]]) return false;
    }
    bool same = true;
    for (NSString *key in @[@"pid", @"start_seconds", @"start_microseconds"])
        same = same && [old[key] isEqual:current[key]];
    same = same && [old[@"boot"] caseInsensitiveCompare:current[@"boot"]] == NSOrderedSame;
    return !same;
}
bool positive(id v) { return integer(v) && [v longLongValue] > 0; }
bool yes(id v) { return boolean(v) && [v boolValue]; }
bool launchRecoveryValid(NSDictionary *prefix,NSDictionary *e,bool requireReboot=true){
    if(!keys(e,@[@"record_sha256",@"observation",@"helper_absence",@"review_reference"]) ||
       ![e[@"record_sha256"] isEqual:recordHash(prefix)] || !text(e[@"review_reference"]))return false;
    NSDictionary *observation=e[@"observation"];
    if(![observation isKindOfClass:[NSDictionary class]])return false;
    NSDictionary *current=observation[@"identity"],*old=prefix[@"metadata"];
    if(!metadataValid(current) || !preflightValid(observation,current) ||
       ![observation[@"experiment"] isEqual:@"ordinary"] || [observation[@"raw_status"] integerValue]!=0 ||
       (requireReboot && [old[@"boot"] caseInsensitiveCompare:current[@"boot"]]==NSOrderedSame))return false;
    for(NSString *key in old){
        if([@[@"pid",@"boot",@"start_seconds",@"start_microseconds"] containsObject:key])continue;
        if(![old[key] isEqual:current[key]])return false;
    }
    for(NSDictionary *event in prefix[@"events"])
        if([event[@"evidence"][@"ownership_conflict"] boolValue])return false;
    NSDictionary *helper=e[@"helper_absence"];
    return keys(helper,@[@"argv",@"exit",@"stdout",@"stderr",@"usable",@"reaped"]) &&
        [helper[@"argv"] isEqual:@[@"/usr/bin/pgrep",@"-x",@"Pro40VerifierLab"]] &&
        integer(helper[@"exit"]) && [helper[@"exit"] integerValue]==1 &&
        [helper[@"stdout"] isEqual:@""] && [helper[@"stderr"] isEqual:@""] &&
        yes(helper[@"usable"]) && yes(helper[@"reaped"]);
}
bool neverLaunchedRecoveryValid(NSDictionary *prefix,NSDictionary *e){
    NSArray *events=prefix[@"events"];
    if(events.count!=3 || ![events[0][@"kind"] isEqual:@"register_intent"] ||
       ![events[1][@"kind"] isEqual:@"register_result"] || ![events[2][@"kind"] isEqual:@"launch_failed"] ||
       !yes(events[1][@"evidence"][@"success"]) || [events[1][@"evidence"][@"raw_status"] integerValue]!=1 ||
       !keys(e,@[@"record_sha256",@"observation",@"helper_absence",@"review_reference",@"removal_receipt_sha256",@"removal_intent_sha256",@"original_controller_absent",@"unregister_result"]) ||
       !hex(e[@"removal_receipt_sha256"],64) || !hex(e[@"removal_intent_sha256"],64) || !yes(e[@"original_controller_absent"]))return false;
    NSDictionary *result=e[@"unregister_result"];
    if(!keys(result,@[@"callback_received",@"success",@"error_domain",@"error_code",@"raw_status"]) ||
       !yes(result[@"callback_received"]) || !yes(result[@"success"]) || ![result[@"error_domain"] isEqual:@""] ||
       !integer(result[@"error_code"]) || [result[@"error_code"] integerValue]!=0 ||
       !integer(result[@"raw_status"]) || [result[@"raw_status"] integerValue]!=0)return false;
    NSMutableDictionary *common=[e mutableCopy];
    for(NSString *key in @[@"removal_receipt_sha256",@"removal_intent_sha256",@"original_controller_absent",@"unregister_result"]) [common removeObjectForKey:key];
    return launchRecoveryValid(prefix,common,false);
}
bool removalValid(NSDictionary *e, bool terminal) {
    return keys(e, @[@"not_registered", @"job_absent", @"unexpected_helper"]) &&
        boolean(e[@"not_registered"]) && boolean(e[@"job_absent"]) && boolean(e[@"unexpected_helper"]) &&
        (!terminal || ([e[@"not_registered"] boolValue] && [e[@"job_absent"] boolValue] && ![e[@"unexpected_helper"] boolValue]));
}
bool launchEventsValid(NSDictionary *r) {
    NSArray *events = r[@"events"];
    if([events.lastObject[@"kind"] isEqual:@"launch_recovered"] || [events.lastObject[@"kind"] isEqual:@"never_launched_reconciled"]){
        if(events.count<2 || events.count>11 || !keys(events.lastObject,@[@"kind",@"evidence"]))return false;
        NSMutableDictionary *prefix=[r mutableCopy];prefix[@"events"]=[events subarrayWithRange:NSMakeRange(0,events.count-1)];
        NSString *last=[prefix[@"events"] lastObject][@"kind"];
        if([last isEqual:@"complete"] || [last isEqual:@"launch_recovered"] || [last isEqual:@"never_launched_reconciled"])return false;
        return launchEventsValid(prefix) && ([events.lastObject[@"kind"] isEqual:@"never_launched_reconciled"]?
            neverLaunchedRecoveryValid(prefix,events.lastObject[@"evidence"]):launchRecoveryValid(prefix,events.lastObject[@"evidence"]));
    }
    if ([events.lastObject[@"kind"] isEqual:@"launch_failed"]) {
        if (events.count<2 || events.count>10) return false;
        NSDictionary *event=events.lastObject, *e=event[@"evidence"];
        if (!keys(event,@[@"kind",@"evidence"]) ||
            !keys(e,@[@"prefix_sha256",@"after_event",@"category",@"detail",@"observed_ns"]) ||
            ![@[@"cancelled",@"expired",@"api-error",@"observation-invalid",@"transport-error"] containsObject:e[@"category"]] ||
            !text(e[@"detail"]) || !positive(e[@"observed_ns"])) return false;
        NSMutableDictionary *prefix=[r mutableCopy];
        prefix[@"events"]=[events subarrayWithRange:NSMakeRange(0,events.count-1)];
        NSDictionary *prior=[prefix[@"events"] lastObject];
        if ([prior[@"kind"] isEqual:@"launch_failed"] || [prior[@"kind"] isEqual:@"launch_recovered"] || [prior[@"kind"] isEqual:@"never_launched_reconciled"] || ![e[@"after_event"] isEqual:prior[@"kind"]]) return false;
        return launchEventsValid(prefix) && [e[@"prefix_sha256"] isEqual:recordHash(prefix)];
    }
    NSArray *order = @[@"register_intent", @"register_result", @"launch_intent", @"ready",
        @"release_intent", @"exit", @"unregister_intent", @"unregister_result", @"removal", @"complete"];
    if (!events.count || events.count > order.count) return false;
    uint64_t runs = 0, last = 0, deadline = 0;
    NSString *nonce = nil;
    NSDictionary *ownership = nil;
    for (NSUInteger i=0; i<events.count; ++i) {
        NSDictionary *event = events[i], *e = event[@"evidence"];
        if (!keys(event,@[@"kind",@"evidence"]) || ![event[@"kind"] isEqual:order[i]]) return false;
        if (i==1 || i==7) {
            if (!resultValid(e) || ([e[@"ownership_conflict"] boolValue] && events.count>i+1)) return false;
            if (i==1 && events.count>2 && (![e[@"success"] boolValue] || [e[@"raw_status"] integerValue]!=1)) return false;
        } else if (i==2) {
            NSArray *launchKeys=@[@"nonce",@"baseline_runs",@"observed_ns",@"deadline_ns",@"exact_job",@"no_running_pid",@"job_output"];
            ownership=e[@"submitted_ownership"];
            if(ownership)launchKeys=[launchKeys arrayByAddingObject:@"submitted_ownership"];
            if (!keys(e,launchKeys) ||
                !hex(e[@"nonce"],32) || [e[@"nonce"] isEqual:@"00000000000000000000000000000000"] ||
                !integer(e[@"baseline_runs"]) || [e[@"baseline_runs"] longLongValue]<0 ||
                [e[@"baseline_runs"] unsignedLongLongValue]>=INT64_MAX ||
                !positive(e[@"observed_ns"]) || !positive(e[@"deadline_ns"]) ||
                !yes(e[@"exact_job"]) || !yes(e[@"no_running_pid"]) || !text(e[@"job_output"])) return false;
            nonce=e[@"nonce"]; runs=[e[@"baseline_runs"] unsignedLongLongValue]+1;
            last=[e[@"observed_ns"] unsignedLongLongValue]; deadline=[e[@"deadline_ns"] unsignedLongLongValue];
            if (last>=deadline) return false;
        } else if (i==3) {
            if (!keys(e,@[@"nonce",@"pid",@"launchd_pid",@"boot",@"start_seconds",@"start_microseconds",
                @"helper_cdhash",@"authenticated_peer",@"process_birth_matches",@"runs",@"observed_ns",@"job_output"]) ||
                ![e[@"nonce"] isEqual:nonce] || !positive(e[@"pid"]) || [e[@"pid"] longLongValue]>INT32_MAX ||
                !integer(e[@"launchd_pid"]) || ![e[@"launchd_pid"] isEqual:e[@"pid"]] ||
                !text(e[@"boot"]) || ![[NSUUID alloc] initWithUUIDString:e[@"boot"]] ||
                [e[@"boot"] caseInsensitiveCompare:r[@"metadata"][@"boot"]]!=NSOrderedSame ||
                !positive(e[@"start_seconds"]) || !integer(e[@"start_microseconds"]) ||
                [e[@"start_microseconds"] longLongValue]<0 || [e[@"start_microseconds"] longLongValue]>=1000000 ||
                ![e[@"helper_cdhash"] isEqual:r[@"metadata"][@"helper_cdhash"]] ||
                !yes(e[@"authenticated_peer"]) || !yes(e[@"process_birth_matches"]) ||
                !integer(e[@"runs"]) || [e[@"runs"] unsignedLongLongValue]!=runs || !text(e[@"job_output"])) return false;
        } else if (i==4) {
            if (!keys(e,@[@"observed_ns"])) return false;
        } else if (i==5) {
            if (!keys(e,@[@"runs",@"observed_ns",@"code",@"exit_code_known",@"signalled",@"exact_job",@"not_running",@"original_instance_gone",@"job_output"]) ||
                !integer(e[@"runs"]) || [e[@"runs"] unsignedLongLongValue]!=runs ||
                !integer(e[@"code"]) || [e[@"code"] longLongValue]<0 || [e[@"code"] longLongValue]>255 ||
                !yes(e[@"exit_code_known"]) || !boolean(e[@"signalled"]) || [e[@"signalled"] boolValue] || !yes(e[@"exact_job"]) ||
                !yes(e[@"not_running"]) || !yes(e[@"original_instance_gone"]) || !text(e[@"job_output"])) return false;
        } else if (i==8) {
            if (!removalValid(e,events.count==10)) return false;
        } else if (!keys(e,@[])) return false;
        if(i==2 || i==3 || i==5){
            NSString *target=[NSString stringWithFormat:@"%@/%@",r[@"metadata"][@"domain"],r[@"metadata"][@"label"]];
            if(ownership){
                if(!keys(ownership,@[@"uuid",@"target",@"bundle_path",@"attempt",@"observed_ns",@"deadline_ns"]) ||
                   !text(ownership[@"uuid"]) || ![[NSUUID alloc] initWithUUIDString:ownership[@"uuid"]] ||
                   ![ownership[@"target"] isEqual:target] || ![ownership[@"bundle_path"] isEqual:r[@"metadata"][@"bundle_path"]] ||
                   ![ownership[@"attempt"] isEqual:r[@"attempt"]] || !positive(ownership[@"observed_ns"]) ||
                   [ownership[@"observed_ns"] unsignedLongLongValue]>[events[2][@"evidence"][@"observed_ns"] unsignedLongLongValue] ||
                   !positive(ownership[@"deadline_ns"]) || [ownership[@"deadline_ns"] unsignedLongLongValue]!=deadline)return false;
                NSData *raw=[e[@"job_output"] dataUsingEncoding:NSUTF8StringEncoding];
                if(!raw)return false;
                auto parsed=parseSubmittedVerifierJobSnapshot(std::string(static_cast<const char *>(raw.bytes),raw.length),target.UTF8String);
                if(!parsed || parsed->btmUUID!=[ownership[@"uuid"] UTF8String])return false;
                const auto& state=parsed->state;
                if(i==2 && (state.running || state.pid || state.runs!=runs-1))return false;
                if(i==3 && (!state.running || !state.pid || *state.pid!=[e[@"pid"] intValue] || state.runs!=runs))return false;
                if(i==5 && (state.running || state.pid || state.runs!=runs || !state.lastExitCode ||
                   *state.lastExitCode!=[e[@"code"] unsignedIntValue]))return false;
            }else if([e[@"job_output"] rangeOfString:@"type = Submitted"].location!=NSNotFound)return false;
        }
        if (i>=3 && i<=5) {
            if (!positive(e[@"observed_ns"])) return false;
            auto now=[e[@"observed_ns"] unsignedLongLongValue];
            if (now<last || now>=deadline) return false;
            last=now;
        }
    }
    return true;
}
bool recordComplete(NSDictionary *r) {
    if([r[@"version"] integerValue]==5 && [[[r[@"events"] lastObject] objectForKey:@"kind"] isEqual:@"never_launched_reconciled"])return true;
    if([r[@"version"] integerValue]==5 && [[[r[@"events"] lastObject] objectForKey:@"kind"] isEqual:@"launch_recovered"])return true;
    return [r[@"events"] count] == ([r[@"version"] integerValue]==5 ? 10 : 6) &&
        ![[r[@"events"] lastObject][@"kind"] isEqual:@"launch_failed"];
}
bool recordValid(NSDictionary *r) {
    if (!keys(r, @[@"version", @"attempt", @"metadata", @"preflight", @"events"]) ||
        !integer(r[@"version"]) || ([r[@"version"] longLongValue] != 4 && [r[@"version"] longLongValue] != 5) ||
        !hex(r[@"attempt"], 32) || !metadataValid(r[@"metadata"]) ||
        !preflightValid(r[@"preflight"], r[@"metadata"]) ||
        ![r[@"events"] isKindOfClass:[NSArray class]]) return false;
    if ([r[@"version"] integerValue]==5) return launchEventsValid(r);
    NSArray *events = r[@"events"];
    NSArray *order = @[@"register_intent", @"register_result", @"unregister_intent",
                       @"unregister_result", @"removal", @"complete"];
    if (events.count == 0 || events.count > order.count) return false;
    for (NSUInteger i = 0; i < events.count; ++i) {
        NSDictionary *event = events[i];
        if (!keys(event, @[@"kind", @"evidence"])) return false;
        bool recovered = i == 5 && [event[@"kind"] isEqual:@"recovered"];
        if (![event[@"kind"] isEqual:order[i]] && !recovered) return false;
        NSDictionary *e = event[@"evidence"];
        if (recovered) {
            NSMutableDictionary *prefix = [r mutableCopy];
            prefix[@"events"] = [events subarrayWithRange:NSMakeRange(0, 5)];
            if (!recoveryValid(prefix, e)) return false;
        } else if (i == 1 || i == 3) {
            if (!resultValid(e)) return false;
            if ([e[@"ownership_conflict"] boolValue] && events.count > i + 1) return false;
        } else if (i == 4) {
            if (!keys(e, @[@"not_registered", @"job_absent", @"unexpected_helper"]) ||
                !boolean(e[@"not_registered"]) || !boolean(e[@"job_absent"]) ||
                !boolean(e[@"unexpected_helper"])) return false;
            if (events.count == 6 && (![e[@"not_registered"] boolValue] ||
                ![e[@"job_absent"] boolValue] || [e[@"unexpected_helper"] boolValue])) return false;
        } else if (!keys(e, @[])) return false;
    }
    return true;
}
}

RegistrationJournal::~RegistrationJournal() {
    if (lock_ >= 0) close(lock_);
    if (directory_ >= 0) close(directory_);
}
bool RegistrationJournal::validatePreflight(NSDictionary *identity, NSDictionary *preflight) {
    return metadataValid(identity) && preflightValid(preflight, identity);
}
bool RegistrationJournal::fail(const char *message) {
    error = message;
    poisoned_ = true;
    return false;
}
bool RegistrationJournal::open(const std::string& path, bool readOnly, bool existingOnly) {
    if (poisoned_ || directory_ >= 0 || path.empty() || path[0] != '/' || path.back() == '/')
        return fail("invalid or repeated journal open");
    readOnly_ = readOnly;
    recoveryOnly_ = existingOnly && !readOnly;
    // Walk every component without following symlinks, not merely the leaf.
    FD parent(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (parent.value < 0) return fail("cannot open root");
    size_t start = 1;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".." || part.find('\0') != std::string::npos)
            return fail("invalid journal path component");
        if (end == std::string::npos && !readOnly_ && !existingOnly) {
            if (mkdirat(parent.value, part.c_str(), 0700) != 0 && errno != EEXIST)
                return fail("cannot create journal directory");
            if (fsync(parent.value) != 0) return fail("cannot sync journal parent");
        }
        int next = openat(parent.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) return fail(errno == ENOENT ? "journal path missing" : "unsafe or inaccessible journal path");
        close(parent.value);
        parent.value = next;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    directory_ = parent.value;
    parent.value = -1;
    if (!safe(directory_, true)) return fail("unsafe journal directory");
    lock_ = openat(directory_, "owner.lock", (readOnly_ ? O_RDONLY : O_RDWR | (existingOnly ? 0 : O_CREAT)) |
        O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (lock_ < 0 || !safe(lock_) || flock(lock_, LOCK_EX | LOCK_NB) != 0)
        return fail("unsafe or occupied journal lock");
    if (!readOnly_ && (fsync(lock_) != 0 || fsync(directory_) != 0)) return fail("cannot sync journal lock");
    return true;
}
NSArray *RegistrationJournal::scan() {
    if (poisoned_ || directory_ < 0 || active_) { (void)fail("journal unavailable or attempt active"); return nil; }
    NSMutableArray *records = [NSMutableArray array];
    scannedRecords_ = 0;
    // Fresh open file description: dup would share a directory enumeration offset.
    int scanFD = openat(directory_, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scanFD < 0) { (void)fail("cannot scan journal"); return nil; }
    DIR *scan = fdopendir(scanFD);
    if (!scan) { close(scanFD); (void)fail("cannot scan journal"); return nil; }
    bool ok = true;
    for (;;) {
        errno = 0;
        dirent *entry = readdir(scan);
        if (!entry) { if (errno) ok = false; break; }
        std::string name(entry->d_name);
        if (name == "." || name == ".." || name == "owner.lock") continue;
        if (name.size() != 37 || name.substr(32) != ".json") { ok = false; break; }
        FD file(openat(directory_, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (file.value < 0 || !safe(file.value)) { ok = false; break; }
        std::string bytes;
        char buffer[4096];
        for (;;) {
            ssize_t count = read(file.value, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { ok = false; break; }
            if (count == 0) break;
            bytes.append(buffer, static_cast<size_t>(count));
            if (bytes.size() > maxRecord) { ok = false; break; }
        }
        if (!ok) break;
        NSData *data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        NSDictionary *record = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if (!recordValid(record) ||
            ![[record[@"attempt"] stringByAppendingString:@".json"] isEqual:
                [NSString stringWithUTF8String:name.c_str()]]) { ok = false; break; }
        // Accept only our emitted representation. In particular, duplicate JSON
        // keys must not be silently collapsed into apparently valid evidence.
        NSData *canonical = [NSJSONSerialization dataWithJSONObject:record
            options:NSJSONWritingSortedKeys error:nil];
        if (![canonical isEqualToData:data]) { ok = false; break; }
        [records addObject:record];
        ++scannedRecords_;
    }
    closedir(scan);
    if (!ok) { (void)fail("unsafe or invalid journal contents"); return nil; }
    return [records sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        return [a[@"attempt"] compare:b[@"attempt"]];
    }];
}
bool RegistrationJournal::requireClear() {
    NSArray *records = scan();
    if (!records) return false;
    for (NSDictionary *record in records)
        if (!recordComplete(record)) return fail("unresolved journal attempt");
    return true;
}
NSDictionary *RegistrationJournal::audit() {
    if (!readOnly_) { (void)fail("audit requires read-only open"); return nil; }
    NSArray *records = scan();
    if (!records) return nil;
    NSMutableArray *entries = [NSMutableArray array];
    NSUInteger unresolved = 0;
    for (NSDictionary *record in records) {
        NSArray *events = record[@"events"];
        bool complete = recordComplete(record);
        if (!complete) ++unresolved;
        NSString *hash = recordHash(record);
        [entries addObject:@{@"record": record, @"record_sha256": hash,
            @"last_event": events.lastObject[@"kind"], @"complete": @(complete),
            @"recovery_authorized": @NO,
            @"next_action": complete ? @"retain-history" : @"independent-reconciliation-required"}];
    }
    return @{@"audit_version": @1, @"read_only": @YES, @"records": entries,
        @"unresolved_count": @(unresolved), @"all_records_complete": @(unresolved == 0),
        @"recovery_authorized": @NO};
}
bool RegistrationJournal::write(NSDictionary *record) {
    if (readOnly_ || poisoned_ || directory_ < 0 || !recordValid(record)) return fail("invalid record or read-only journal");
    NSData *data = [NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingSortedKeys error:nil];
    if (!data || data.length > maxRecord) return fail("record serialization failed");
    std::string stem([record[@"attempt"] UTF8String]);
    std::string pending = stem + ".pending", final = stem + ".json";
    FD file(openat(directory_, pending.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.value < 0 || !safe(file.value)) return fail("cannot create pending record");
#ifdef PRO40_JOURNAL_TESTING
    if (failWriteStage == 1) return fail("injected after create");
#endif
    size_t offset = 0;
    while (offset < data.length) {
        ssize_t count = ::write(file.value, static_cast<const char *>(data.bytes) + offset, data.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return fail("cannot write record");
        offset += static_cast<size_t>(count);
    }
#ifdef PRO40_JOURNAL_TESTING
    if (failWriteStage == 2) return fail("injected after write");
#endif
    if (fsync(file.value) != 0) return fail("cannot sync record");
#ifdef PRO40_JOURNAL_TESTING
    if (failWriteStage == 3) return fail("injected after file sync");
#endif
    if (renameat(directory_, pending.c_str(), directory_, final.c_str()) != 0) return fail("cannot rename record");
#ifdef PRO40_JOURNAL_TESTING
    if (failWriteStage == 4) return fail("injected after rename");
#endif
    if (fsync(directory_) != 0) return fail("cannot sync journal directory");
    // Keep a deep immutable snapshot: caller mutation cannot change later writes.
    active_ = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return true;
}
bool RegistrationJournal::beginVersion(NSString *attempt, NSDictionary *metadata, NSDictionary *preflight, int version) {
    if (recoveryOnly_) return fail("recovery-only journal");
    if (!requireClear()) return false;
    if (!hex(attempt, 32) || !metadataValid(metadata)) return fail("invalid attempt identity");
    if (!preflightValid(preflight, metadata)) return fail("invalid or unbound preflight");
    if ([preflight[@"experiment"] isEqual:@"reviewed-first-use"] && scannedRecords_ != 0)
        return fail("first use requires an empty locked journal");
    std::string name([[attempt stringByAppendingString:@".json"] UTF8String]);
    struct stat existing{};
    if (fstatat(directory_, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
        return fail("reused or unreadable attempt identity");
    return write(@{@"version": @(version), @"attempt": attempt, @"metadata": metadata, @"preflight": preflight,
                   @"events": @[@{@"kind": @"register_intent", @"evidence": @{}}]});
}
bool RegistrationJournal::begin(NSString *attempt, NSDictionary *metadata, NSDictionary *preflight) {
    return beginVersion(attempt,metadata,preflight,4);
}
bool RegistrationJournal::beginLaunch(NSString *attempt, NSDictionary *metadata, NSDictionary *preflight) {
    return beginVersion(attempt,metadata,preflight,5);
}
bool RegistrationJournal::matchesRegisteredLaunch(NSString *attempt, NSDictionary *metadata) const {
    if(poisoned_ || readOnly_ || recoveryOnly_ || directory_<0 || lock_<0 ||
       [active_[@"version"] integerValue]!=5 || ![active_[@"attempt"] isEqual:attempt] ||
       ![active_[@"metadata"] isEqual:metadata] || [active_[@"events"] count]!=2)return false;
    NSDictionary *event=active_[@"events"][1], *e=event[@"evidence"];
    return [event[@"kind"] isEqual:@"register_result"] && [e[@"success"] boolValue] &&
        [e[@"raw_status"] integerValue]==1 && ![e[@"ownership_conflict"] boolValue];
}
NSDictionary *RegistrationJournal::reviewRemoval(NSString *attempt, NSString *expectedHash) {
    if ((!recoveryOnly_ && !readOnly_) || active_ || !hex(attempt, 32) || !hex(expectedHash, 64))
        { (void)fail("existing-only recovery or read-only review required"); return nil; }
    NSArray *records = scan();
    if (!records) return nil;
    NSDictionary *target = nil;
    for (NSDictionary *record in records) {
        if ([record[@"attempt"] isEqual:attempt]) target = record;
        else if (!recordComplete(record)) { (void)fail("other unresolved attempt"); return nil; }
    }
    if (!target || [target[@"version"] integerValue]!=4 || [target[@"events"] count] != 5 || ![recordHash(target) isEqual:expectedHash])
        { (void)fail("record changed or not at removal boundary"); return nil; }
    NSDictionary *removal = target[@"events"][4][@"evidence"];
    if (![removal[@"not_registered"] boolValue] || ![removal[@"job_absent"] boolValue] ||
        [removal[@"unexpected_helper"] boolValue]) { (void)fail("prior removal not confirmed"); return nil; }
    return target;
}
bool RegistrationJournal::validateRecovery(NSDictionary *record, NSDictionary *evidence) {
    return recordValid(record) && [record[@"version"] integerValue]==4 && [record[@"events"] count] == 5 && recoveryValid(record, evidence);
}
NSDictionary *RegistrationJournal::reviewLaunchFailure(NSString *attempt,NSString *expectedHash){
    if((!readOnly_ && !recoveryOnly_) || active_ || !hex(attempt,32) || !hex(expectedHash,64))
        {(void)fail("read-only or existing recovery launch review required");return nil;}
    NSArray *records=scan();if(!records)return nil;
    NSDictionary *target=nil;
    for(NSDictionary *record in records){
        if([record[@"attempt"] isEqual:attempt])target=record;
        else if(!recordComplete(record)){(void)fail("other unresolved attempt");return nil;}
    }
    if(!target || [target[@"version"] integerValue]!=5 || recordComplete(target) || ![recordHash(target) isEqual:expectedHash])
        {(void)fail("record changed or not an unresolved launch");return nil;}
    for(NSDictionary *event in target[@"events"]){
        if([event[@"evidence"][@"ownership_conflict"] boolValue])
            {(void)fail("launch ownership conflict");return nil;}
    }
    return target;
}
bool RegistrationJournal::recoverLaunchFailure(NSString *attempt,NSString *expectedHash,NSDictionary *evidence){
    if(!recoveryOnly_ || readOnly_)return fail("writable existing-only recovery required");
    NSDictionary *target=reviewLaunchFailure(attempt,expectedHash);
    if(!target)return false;
    if(!launchRecoveryValid(target,evidence))return fail("insufficient launch recovery evidence");
    NSMutableDictionary *updated=[target mutableCopy];NSMutableArray *events=[target[@"events"] mutableCopy];
    [events addObject:@{@"kind":@"launch_recovered",@"evidence":evidence}];updated[@"events"]=events;
    return write(updated);
}
bool RegistrationJournal::reconcileNeverLaunched(NSString *attempt,NSString *expectedHash,NSDictionary *evidence){
    if(!recoveryOnly_ || readOnly_)return fail("writable existing-only reconciliation required");
    NSDictionary *target=reviewLaunchFailure(attempt,expectedHash);
    if(!target || !neverLaunchedRecoveryValid(target,evidence))return fail("insufficient never-launched reconciliation evidence");
    NSMutableDictionary *updated=[target mutableCopy];NSMutableArray *events=[target[@"events"] mutableCopy];
    [events addObject:@{@"kind":@"never_launched_reconciled",@"evidence":evidence}];updated[@"events"]=events;
    return write(updated);
}
bool RegistrationJournal::recoverRemoval(NSString *attempt, NSString *expectedHash, NSDictionary *evidence) {
    if (!recoveryOnly_ || readOnly_) return fail("writable recovery required");
    NSDictionary *target = reviewRemoval(attempt, expectedHash);
    if (!target) return false;
    if (!recoveryValid(target, evidence)) return fail("insufficient recovery evidence");
    NSMutableDictionary *updated = [target mutableCopy];
    NSMutableArray *events = [target[@"events"] mutableCopy];
    [events addObject:@{@"kind": @"recovered", @"evidence": evidence}];
    updated[@"events"] = events;
    // Full validation rejects negative prior removal/conflicts. Same atomic write
    // leaves pending files on failure; no historical event is synthesized.
    return write(updated);
}
bool RegistrationJournal::recordLaunchFailure(NSString *category, NSString *detail, uint64_t observedNS) {
    if (!active_ || [active_[@"version"] integerValue]!=5 || recordComplete(active_))
        return fail("no incomplete launch attempt");
    return append(@"launch_failed", @{@"prefix_sha256":recordHash(active_),
        @"after_event":[active_[@"events"] lastObject][@"kind"],
        @"category":category ?: @"", @"detail":detail ?: @"", @"observed_ns":@(observedNS)});
}
bool RegistrationJournal::append(NSString *kind, NSDictionary *evidence) {
    if([kind isEqual:@"launch_recovered"] || [kind isEqual:@"never_launched_reconciled"])return fail("explicit recovery path required");
    if (poisoned_ || !active_ || !kind || !evidence) return fail("no active journal attempt");
    NSMutableArray *events = [active_[@"events"] mutableCopy];
    [events addObject:@{@"kind": kind, @"evidence": evidence}];
    NSMutableDictionary *next = [active_ mutableCopy];
    next[@"events"] = events;
    return write(next);
}
}
