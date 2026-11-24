#pragma once
#import <Foundation/Foundation.h>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>

namespace dpp::mac {
// This serializer is observational: it never writes a journal or changes a deadline.
inline NSDictionary* ownerTimingSample(NSDictionary* original, NSDictionary* current, bool active,
                                      unsigned long long now, unsigned long long ownerLimit) {
    if (!current[@"session_id"]) return @{};
    const auto started = [current[@"started_ms"] unsignedLongLongValue];
    return @{
        @"protected_session_id" : current[@"session_id"],
        @"original_started_ms" : original[@"started_ms"] ? original[@"started_ms"] : (id)NSNull.null,
        @"original_deadline_ms" : original[@"started_ms"]
            ? @([original[@"started_ms"] unsignedLongLongValue] + ownerLimit) : (id)NSNull.null,
        @"current_owner_started_ms" : @(started),
        @"current_owner_deadline_ms" : @(started + ownerLimit),
        @"owner_active" : @(active),
        @"sampled_ms" : @(now),
        @"clock" : @"mach_continuous_time_ms",
        @"source" : @"owner_backend_record_and_remainingMs_limit"
    };
}
inline NSDictionary* helperTimingSample(NSDictionary* controlling, unsigned long long heartbeat,
                                       bool active, unsigned long long sequence, unsigned long long now) {
    return @{
        @"protected_session_id" : controlling[@"session_id"],
        @"helper_sample_request" : @(sequence),
        @"current_helper_started_ms" : controlling[@"started_ms"],
        @"current_helper_limit_ms" : controlling[@"limit_ms"],
        @"current_helper_deadline_ms" : @([controlling[@"started_ms"] unsignedLongLongValue] +
                                         [controlling[@"limit_ms"] unsignedLongLongValue]),
        @"current_heartbeat_deadline_ms" : @(heartbeat + 5000),
        @"helper_active" : @(active), @"sampled_ms" : @(now),
        @"clock" : @"mach_continuous_time_ms",
        @"source" : @"helper_loop_controlling_record_and_heartbeat"
    };
}
// Optional datagram telemetry never blocks the independent recovery helper.
inline void sendHelperTimingSample(int fd, NSDictionary* row) {
    @try {
        NSData* data = [NSJSONSerialization dataWithJSONObject:row options:0 error:nil];
        if (fd >= 0 && data && data.length <= 4096)
            (void)send(fd, data.bytes, data.length, MSG_DONTWAIT);
    } @catch (NSException*) {
    }
}
inline void serviceHelperTimingRequests(int fd, NSDictionary* controlling, unsigned long long heartbeat,
                                       bool active, unsigned long long now, unsigned& processed) {
    if (fd < 0) return;
    // Bounded independently of socket traffic; no heartbeat/deadline mutation.
    for (unsigned n = 0; n < 16 && processed < 260; ++n) {
        uint64_t sequence = 0;
        if (recv(fd, &sequence, sizeof(sequence), MSG_DONTWAIT) != sizeof(sequence)) break;
        ++processed;
        sendHelperTimingSample(fd, helperTimingSample(controlling, heartbeat, active, sequence, now));
    }
}
// Only the owner writes the launch log; one bounded write avoids stdio fragments.
// Missing/failed writes are missing evidence, never permission to continue protection.
inline void writeMirrorTrace(NSString* prefix, NSDictionary* row) {
    @try {
        NSData* json = [NSJSONSerialization dataWithJSONObject:row options:0 error:nil];
        if (!json) return;
        NSMutableData* line = [[prefix dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];
        [line appendData:json];
        [line appendBytes:"\n" length:1];
        if (line.length <= 4096) (void)::write(STDERR_FILENO, line.bytes, line.length);
    } @catch (NSException*) {
    }
}
}  // namespace dpp::mac
