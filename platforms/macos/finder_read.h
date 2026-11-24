#pragma once
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <cmath>
namespace dpp::mac {
// Public Finder.sdef property/object codes. This module sends ONLY core/getd, never set/move/reveal.
inline NSAppleEventDescriptor* object(OSType desired, OSType form, NSAppleEventDescriptor* key,
                                      NSAppleEventDescriptor* container = nil) {
    NSAppleEventDescriptor* spec = [NSAppleEventDescriptor recordDescriptor];
    [spec setDescriptor:[NSAppleEventDescriptor descriptorWithTypeCode:desired] forKeyword:keyAEDesiredClass];
    [spec setDescriptor:container ? container : NSAppleEventDescriptor.nullDescriptor
             forKeyword:keyAEContainer];
    [spec setDescriptor:[NSAppleEventDescriptor descriptorWithEnumCode:form] forKeyword:keyAEKeyForm];
    [spec setDescriptor:key forKeyword:keyAEKeyData];
    return [spec coerceToDescriptorType:typeObjectSpecifier];
}
inline NSAppleEventDescriptor* property(OSType code, NSAppleEventDescriptor* container = nil) {
    return object(typeProperty, formPropertyID, [NSAppleEventDescriptor descriptorWithTypeCode:code],
                  container);
}
inline NSAppleEventDescriptor* allItems(NSAppleEventDescriptor* container) {
    const OSType ordinal = kAEAll;
    auto key = [NSAppleEventDescriptor descriptorWithDescriptorType:typeAbsoluteOrdinal
                                                              bytes:&ordinal
                                                             length:sizeof(ordinal)];
    return object('cobj', formAbsolutePosition, key, container);
}
inline NSString* fourCC(OSType code) {
    return [NSString stringWithFormat:@"%08x", static_cast<unsigned>(code)];
}
inline bool numericType(DescType t) {
    return t == typeSInt16 || t == typeSInt32 || t == typeSInt64 || t == typeIEEE32BitFloatingPoint ||
           t == typeIEEE64BitFloatingPoint;
}
// Unknown/malformed descriptors stay unknown, never coerced into a success at (0,0).
inline NSArray* geometry(NSAppleEventDescriptor* descriptor, NSUInteger count) {
    if (!descriptor) return nil;
    if (descriptor.descriptorType == typeAEList &&
        descriptor.numberOfItems == static_cast<NSInteger>(count)) {
        NSMutableArray* values = [NSMutableArray array];
        for (NSInteger i = 1; i <= static_cast<NSInteger>(count); ++i) {
            NSAppleEventDescriptor* n = [descriptor descriptorAtIndex:i];
            if (!numericType(n.descriptorType) || !std::isfinite(n.doubleValue)) return nil;
            [values addObject:@(n.doubleValue)];
        }
        return values;
    }
    const bool point = descriptor.descriptorType == typeQDPoint && count == 2;
    const bool rect = descriptor.descriptorType == typeQDRectangle && count == 4;
    if ((point || rect) && descriptor.data.length == sizeof(int16_t) * count) {
        int16_t values[4]{};
        [descriptor.data getBytes:values length:descriptor.data.length];
        if (point) return @[ @(values[1]), @(values[0]) ];  // QuickDraw Point is vertical,horizontal
        return @[ @(values[1]), @(values[0]), @(values[3]), @(values[2]) ];  // left,top,right,bottom
    }
    return nil;
}
struct ReadResult {
    NSAppleEventDescriptor* value = nil;
    OSStatus error = noErr;
    const char* phase = "reply_value";
};
inline ReadResult readFinder(NSAppleEventDescriptor* target, NSAppleEventDescriptor* spec) {
    NSAppleEventDescriptor* event = [NSAppleEventDescriptor appleEventWithEventClass:kAECoreSuite
                                                                             eventID:kAEGetData
                                                                    targetDescriptor:target
                                                                            returnID:kAutoGenerateReturnID
                                                                       transactionID:kAnyTransactionID];
    [event setParamDescriptor:spec forKeyword:keyDirectObject];
    AEDesc reply{typeNull, nullptr};
    OSStatus error = AESendMessage(event.aeDesc, &reply,
                                   kAEWaitReply | kAENeverInteract | kAEDoNotPromptForUserConsent, 120);
    NSAppleEventDescriptor* owned = [[NSAppleEventDescriptor alloc] initWithAEDescNoCopy:&reply];
    if (error != noErr) return {nil, error, "send_transport"};
    NSAppleEventDescriptor* resultError = [owned paramDescriptorForKeyword:keyErrorNumber];
    if (resultError && !numericType(resultError.descriptorType))
        return {nil, errAEWrongDataType, "reply_error_descriptor"};
    if (resultError && resultError.int32Value != 0)
        return {nil, resultError.int32Value, "finder_reply_error"};
    NSAppleEventDescriptor* value = [owned paramDescriptorForKeyword:keyDirectObject];
    return value ? ReadResult{value, noErr} : ReadResult{nil, errAEEventFailed, "missing_direct_object"};
}
}  // namespace dpp::mac
