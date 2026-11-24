#pragma once
#import <Security/Security.h>
#include "platforms/macos/diagnostics.h"
namespace dpp::mac {
inline NSDictionary* appIdentity() {
    NSMutableDictionary* result = [NSMutableDictionary dictionaryWithDictionary:@{
        @"bundle_identifier" : NSBundle.mainBundle.bundleIdentifier ? NSBundle.mainBundle.bundleIdentifier
                                                                    : @"unbundled",
        @"display_name" : [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"]
            ? [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"]
            : @"unknown",
        @"usage_description" :
                [NSBundle.mainBundle objectForInfoDictionaryKey:@"NSAppleEventsUsageDescription"]
            ? [NSBundle.mainBundle objectForInfoDictionaryKey:@"NSAppleEventsUsageDescription"]
            : @"missing",
        @"build_id" : @DPP_BUILD_ID,
        @"configuration" : @DPP_BUILD_CONFIG,
        @"pid" : @(NSProcessInfo.processInfo.processIdentifier)
    }];
    SecCodeRef code = nullptr;
    OSStatus status = SecCodeCopySelf(kSecCSDefaultFlags, &code);
    CFDictionaryRef info = nullptr;
    if (status == errSecSuccess)
        status = SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info);
    if (code) CFRelease(code);
    result[@"signing_query_osstatus"] = @(status);
    if (info) {
        NSDictionary* signing = CFBridgingRelease(info);
        NSNumber* flags = signing[(__bridge NSString*)kSecCodeInfoFlags];
        NSDictionary* entitlements = signing[(__bridge NSString*)kSecCodeInfoEntitlementsDict];
        result[@"signing_identifier"] = signing[(__bridge NSString*)kSecCodeInfoIdentifier]
                                            ? signing[(__bridge NSString*)kSecCodeInfoIdentifier]
                                            : @"unknown";
        result[@"signature_mode"] = flags.unsignedIntValue & kSecCodeSignatureAdhoc
                                        ? @"ad_hoc_no_Developer_ID"
                                        : @"non_ad_hoc_inspect_certificate";
        result[@"hardened_runtime"] = @((flags.unsignedIntValue & kSecCodeSignatureRuntime) != 0);
        result[@"app_sandbox"] = @([entitlements[@"com.apple.security.app-sandbox"] boolValue]);
        result[@"effective_entitlements"] = entitlements ? entitlements : @{};
        NSData* hash = signing[(__bridge NSString*)kSecCodeInfoUnique];
        if (hash) {
            NSMutableString* hex = [NSMutableString string];
            for (NSUInteger i = 0; i < hash.length; ++i)
                [hex appendFormat:@"%02x", static_cast<const unsigned char*>(hash.bytes)[i]];
            result[@"cdhash"] = hex;
        }
    }
    return result;
}
}  // namespace dpp::mac
