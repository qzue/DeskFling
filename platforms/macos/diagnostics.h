#pragma once
#import <AppKit/AppKit.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdint>
namespace dpp::mac {
inline NSString* sysctlString(const char* key) {
    char data[256]{};
    size_t length = sizeof(data);
    return sysctlbyname(key, data, &length, nullptr, 0) == 0 ? [NSString stringWithUTF8String:data]
                                                             : @"unavailable";
}
inline NSDictionary* environment() {
    uint64_t memory = 0;
    size_t length = sizeof(memory);
    const bool ok = sysctlbyname("hw.memsize", &memory, &length, nullptr, 0) == 0;
    return @{
#ifdef DPP_BUILD_ID
        @"build_id" : @DPP_BUILD_ID,
#endif
        @"os" : NSProcessInfo.processInfo.operatingSystemVersionString,
#if defined(__arm64__)
        @"architecture" : @"arm64",
#else
        @"architecture" : @"x86_64",
#endif
        @"cpu" : sysctlString("machdep.cpu.brand_string"),
        @"physical_memory_bytes" : ok ? @(memory) : NSNull.null
    };
}
inline id residentMemory() {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS)
        return NSNull.null;
    return @(info.resident_size);
}
inline double processCPUSeconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return NAN;
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-6;
}
inline NSArray* rect(NSRect r) {
    return @[ @(r.origin.x), @(r.origin.y), @(r.size.width), @(r.size.height) ];
}
inline NSArray* screens() {
    NSMutableArray* result = [NSMutableArray array];
    for (NSScreen* screen in NSScreen.screens)
        [result addObject:@{
            @"frame_appkit_points" : rect(screen.frame),
            @"visible_frame_appkit_points" : rect(screen.visibleFrame),
            @"backing_scale" : @(screen.backingScaleFactor)
        }];
    return result;
}
// Creates a new report only. Never overwrites prior evidence; no private file names in the payload.
inline bool writeJSON(NSDictionary* value, NSString* output) {
    NSError* error = nil;
    NSData* data =
        [NSJSONSerialization dataWithJSONObject:value
                                        options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                          error:&error];
    if (!data) return false;
    if (!output) {
        fwrite(data.bytes, 1, data.length, stdout);
        fputc('\n', stdout);
        return true;
    }
    int fd = open(output.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    const char* cursor = static_cast<const char*>(data.bytes);
    size_t left = data.length;
    bool success = true;
    while (left) {
        ssize_t n = write(fd, cursor, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            success = false;
            break;
        }
        cursor += n;
        left -= static_cast<size_t>(n);
    }
    if (fsync(fd) != 0) success = false;
    if (close(fd) != 0) success = false;
    return success;
}
}  // namespace dpp::mac
