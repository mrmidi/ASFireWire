#include "LabIdentity.hpp"
#import <ServiceManagement/ServiceManagement.h>
#import <Security/Security.h>
#include <CommonCrypto/CommonDigest.h>
#include "../NativeProcessBirth.hpp"
#include "NativeRegistrationCollector.hpp"
#include "JobObservation.hpp"
#include "RemovalObservation.hpp"
#include "VerifierServiceProfile.hpp"
#include "NativeDevelopmentIdentity.hpp"
#include <unistd.h>

static NSString *hex(NSData *data) {
    NSMutableString *s = [NSMutableString string];
    const auto *bytes = static_cast<const unsigned char *>(data.bytes);
    for (NSUInteger i = 0; i < data.length; ++i) [s appendFormat:@"%02x", bytes[i]];
    return s;
}
static NSDictionary *signature(NSURL *url, NSString *expected) {
    SecStaticCodeRef code = nullptr;
    OSStatus creation = SecStaticCodeCreateWithPath((__bridge CFURLRef)url, kSecCSDefaultFlags, &code);
    if (creation != errSecSuccess) return @{@"valid": @NO, @"creation_status": @(creation)};
    OSStatus validity = SecStaticCodeCheckValidity(code,
        kSecCSStrictValidate | kSecCSCheckNestedCode | kSecCSCheckAllArchitectures, nullptr);
    CFDictionaryRef raw = nullptr;
    OSStatus infoStatus = SecCodeCopySigningInformation(code, kSecCSSigningInformation, &raw);
    NSDictionary *info = CFBridgingRelease(raw);
    CFRelease(code);
    NSString *identifier = info[(__bridge NSString *)kSecCodeInfoIdentifier];
    NSData *hash = info[(__bridge NSString *)kSecCodeInfoUnique];
    bool valid = validity == errSecSuccess && infoStatus == errSecSuccess &&
        [identifier isEqual:expected] && [hash isKindOfClass:[NSData class]] && hash.length == 20;
    return @{@"valid": @(valid), @"validity_status": @(validity),
             @"information_status": @(infoStatus), @"identifier": identifier ?: @"",
             @"cdhash": [hash isKindOfClass:[NSData class]] ? hex(hash) : @""};
}
static NSString *statusName(SMAppServiceStatus status) {
    switch (status) {
        case SMAppServiceStatusNotRegistered: return @"notRegistered";
        case SMAppServiceStatusEnabled: return @"enabled";
        case SMAppServiceStatusRequiresApproval: return @"requiresApproval";
        case SMAppServiceStatusNotFound: return @"notFound";
    }
    return @"unknown";
}
static NSDictionary *collectReport(Pro40Lab::VerifierServiceProfile profile) {
    const auto collectionStart = std::chrono::steady_clock::now();
    NSBundle *bundle = NSBundle.mainBundle;
    NSURL *url = bundle.bundleURL.URLByResolvingSymlinksInPath;
    NSString *label = @PRO40_LAB_SERVICE_ID;
    NSString *plist = [label stringByAppendingString:@".plist"];
    NSURL *definition = [url URLByAppendingPathComponent:
        [@"Contents/Library/LaunchAgents" stringByAppendingPathComponent:plist]];
    NSData *data = [NSData dataWithContentsOfURL:definition];
    NSDictionary *agent = data ? [NSPropertyListSerialization propertyListWithData:data
        options:NSPropertyListImmutable format:nullptr error:nullptr] : nil;
    bool layout = Pro40Lab::verifierServiceDefinitionMatches(agent,profile) &&
        [bundle.bundleIdentifier isEqual:@PRO40_LAB_APP_ID] &&
        [bundle.infoDictionary[@"CFBundleExecutable"] isEqual:@"Pro40ControllerLab"];
    NSDictionary *controller = signature(url, @PRO40_LAB_APP_ID);
    NSDictionary *helper = signature([url URLByAppendingPathComponent:@"Contents/MacOS/Pro40VerifierLab"], label);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH]{};
    if (data && data.length <= UINT32_MAX) CC_SHA256(data.bytes, static_cast<CC_LONG>(data.length), digest);
    auto birth = NativeProcessBirth::read(getpid());
    SMAppService *service = [SMAppService agentServiceWithPlistName:plist];
    SMAppServiceStatus status = service.status;
    NSString *domain = [NSString stringWithFormat:@"gui/%u", geteuid()];
    NSString *target = [NSString stringWithFormat:@"%@/%@", domain, label];
    auto jobResult = Pro40Lab::observeProcess({"/bin/launchctl", "print", target.UTF8String});
    NSString *jobOut = [[NSString alloc] initWithBytes:jobResult.out.data()
        length:jobResult.out.size() encoding:NSUTF8StringEncoding];
    NSString *jobErr = [[NSString alloc] initWithBytes:jobResult.err.data()
        length:jobResult.err.size() encoding:NSUTF8StringEncoding];
    SMAppServiceStatus statusAfter = service.status;
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - collectionStart).count();
    bool current = elapsed >= 0 && elapsed <= 5000000000LL && status == statusAfter;
    NSDictionary *job = @{@"argv": @[@"/bin/launchctl", @"print", target],
        @"exit": @(jobResult.exit), @"stdout": jobOut ?: @"", @"stderr": jobErr ?: @""};
    NSDictionary *identity = birth ? @{
        @"uid": @(geteuid()), @"domain": domain, @"label": label, @"plist": plist,
        @"bundle_path": url.path, @"controller_id": controller[@"identifier"] ?: @"",
        @"helper_id": helper[@"identifier"] ?: @"",
        @"controller_cdhash": controller[@"cdhash"] ?: @"",
        @"helper_cdhash": helper[@"cdhash"] ?: @"",
        @"plist_sha256": hex([NSData dataWithBytes:digest length:sizeof(digest)]),
        @"pid": @(birth->pid), @"boot": [NSString stringWithUTF8String:birth->boot],
        @"start_seconds": @(birth->seconds), @"start_microseconds": @(birth->microseconds)
    } : @{};
    // A reviewed lab-history reference, NOT proof of an empty locked journal.
    // Persistence will separately establish journal history before permission.
    NSDictionary *candidate = @{@"identity": identity, @"raw_status": @(status),
        @"experiment": status == SMAppServiceStatusNotFound ? @"reviewed-first-use" : @"ordinary",
        @"package_verified": @(layout && [controller[@"valid"] boolValue] && [helper[@"valid"] boolValue]),
        @"observations_current": @(current && jobResult.usable() && jobOut && jobErr),
        @"elapsed_ns": @(elapsed),
        @"provenance": status == SMAppServiceStatusNotFound ?
            @PRO40_LAB_FIRST_USE_REVIEW :
            @"Native current-state collection; independent journal history and ownership checks required",
        @"job": job};
    bool candidateValid = Pro40Lab::RegistrationJournal::validatePreflight(identity, candidate);
    NSMutableDictionary *report = [@{
        @"mode": @"read-only-preflight", @"registration_attempted": @NO, @"registration_allowed": @NO,
        @"bundle_path": url.path ?: @"",
        @"layout_valid": @(layout), @"controller": controller, @"helper": helper,
        @"plist_sha256": hex([NSData dataWithBytes:digest length:sizeof(digest)]),
        @"service_raw_status": @(status), @"service_status": statusName(status),
        @"process_birth_available": @(birth.has_value()),
        @"intent_candidate": candidate, @"intent_candidate_valid": @(candidateValid),
        @"service_status_after": statusName(statusAfter),
        @"job_process": @{@"reaped": @(jobResult.reaped), @"usable": @(jobResult.usable()),
            @"timed_out": @(jobResult.timedOut), @"overflow": @(jobResult.overflow),
            @"io_error": @(jobResult.ioError), @"spawn_error": @(jobResult.spawnError)}
    } mutableCopy];
    if (birth) report[@"process_birth"] = @{@"pid": @(birth->pid),
        @"boot": [NSString stringWithUTF8String:birth->boot],
        @"start_seconds": @(birth->seconds), @"start_microseconds": @(birth->microseconds)};
    return report;
}

NSDictionary *Pro40Lab::collectNativeRegistrationReport(){return collectReport(VerifierServiceProfile::dormant);}
NSDictionary *Pro40Lab::collectNativeVerifierRegistrationReport(){
    const auto began=std::chrono::steady_clock::now();
    NSMutableDictionary *report=[collectReport(VerifierServiceProfile::onDemand) mutableCopy];
    NSMutableDictionary *candidate=[report[@"intent_candidate"] mutableCopy];
    NSDictionary *identity=candidate[@"identity"];
    NSString *requirement=developmentPeerRequirement(DevelopmentRole::controller);
    const bool pins=[requirement isEqual:[NSString stringWithFormat:@"cdhash H\"%@\"",identity[@"helper_cdhash"]]];
    auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count();
    candidate[@"package_verified"]=@([candidate[@"package_verified"] boolValue] && pins);
    candidate[@"observations_current"]=@([candidate[@"observations_current"] boolValue] && elapsed>=0 && elapsed<5000000000LL);
    candidate[@"elapsed_ns"]=@(elapsed);
    report[@"provisioned_peer_matches_package"]=@(pins);
    report[@"intent_candidate"]=candidate;
    report[@"intent_candidate_valid"]=@(RegistrationJournal::validatePreflight(identity,candidate));
    return report;
}

static NSDictionary *collectRemoval(NSDictionary *expectedIdentity,Pro40Lab::VerifierServiceProfile profile) {
    const auto start = std::chrono::steady_clock::now();
    NSDictionary *report = collectReport(profile);
    // Fixed executable name, shorter than pgrep's documented 19-character limit.
    auto helper = Pro40Lab::observeProcess({"/usr/bin/pgrep", "-x", "Pro40VerifierLab"});
    SMAppService *service = [SMAppService agentServiceWithPlistName:@PRO40_LAB_PLIST];
    NSInteger status = service.status;
    auto elapsed = std::chrono::steady_clock::now() - start;
    NSDictionary *evidence = elapsed >= decltype(elapsed)::zero() && elapsed < std::chrono::seconds(5) ?
        Pro40Lab::removalEvidence(expectedIdentity, report[@"intent_candidate"], helper, status) : nil;
    NSString *out = [[NSString alloc] initWithBytes:helper.out.data() length:helper.out.size() encoding:NSUTF8StringEncoding];
    NSString *err = [[NSString alloc] initWithBytes:helper.err.data() length:helper.err.size() encoding:NSUTF8StringEncoding];
    return @{@"registration_observation": report, @"removal_evidence": evidence ?: @{},
        @"valid": @(evidence != nil), @"final_raw_status": @(status),
        @"helper_snapshot": @{@"argv": @[@"/usr/bin/pgrep", @"-x", @"Pro40VerifierLab"],
            @"exit": @(helper.exit), @"stdout": out ?: @"", @"stderr": err ?: @"",
            @"usable": @(helper.usable()), @"reaped": @(helper.reaped)},
        @"elapsed_ns": @(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count())};
}
NSDictionary *Pro40Lab::collectNativeRemovalReport(NSDictionary *identity){return collectRemoval(identity,VerifierServiceProfile::dormant);}
NSDictionary *Pro40Lab::collectNativeVerifierRemovalReport(NSDictionary *identity){return collectRemoval(identity,VerifierServiceProfile::onDemand);}
Pro40Lab::LaunchRecoveryObservation Pro40Lab::collectNativeLaunchRecoveryObservation(){
    NSDictionary *report=collectReport(VerifierServiceProfile::onDemand);
    auto helper=observeProcess({"/usr/bin/pgrep","-x","Pro40VerifierLab"});
    auto service=[SMAppService agentServiceWithPlistName:@PRO40_LAB_PLIST];
    NSMutableDictionary *candidate=[report[@"intent_candidate"] mutableCopy];
    // Detect a registration-state transition during the helper snapshot. The
    // outer review owner enforces the total budget and the retained record hash.
    if(static_cast<NSInteger>(service.status)!=[candidate[@"raw_status"] integerValue])candidate[@"observations_current"]=@NO;
    return {candidate,std::move(helper)};
}
