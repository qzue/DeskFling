#pragma once
#include "platforms/macos/full_desktop_snapshot.h"

namespace dpp::mac {
enum class DesktopItemScope { unsupported, directoryChild, mountedVolume };

inline NSSet<NSString*>* desktopPathSet(NSArray<NSURL*>* urls) {
    if (!urls) return nil;
    NSMutableSet* paths = [NSMutableSet set];
    for (NSURL* url in urls) {
        NSString* path = fullDesktopURLIdentity(url);
        if (!path || [paths containsObject:path]) return nil;
        [paths addObject:path];
    }
    return paths;
}

// A volume must be both a root reported by Foundation and a currently mounted
// URL. An arbitrary outside file, alias target or /Volumes child is not enough.
inline DesktopItemScope desktopItemScope(NSURL* url, NSURL* directory, NSSet<NSString*>* mountedRoots,
                                         bool isVolumeRoot) {
    NSString* path = fullDesktopURLIdentity(url);
    if (!path || !fullDesktopURLIdentity(directory)) return DesktopItemScope::unsupported;
    if ([fullDesktopURLIdentity(url.URLByDeletingLastPathComponent)
            isEqual:fullDesktopURLIdentity(directory)] &&
        ![path isEqual:fullDesktopURLIdentity(directory)])
        return DesktopItemScope::directoryChild;
    if (isVolumeRoot && [mountedRoots containsObject:path]) return DesktopItemScope::mountedVolume;
    return DesktopItemScope::unsupported;
}

// Compare filesystem membership only for directory children. Finder's volume
// icons are virtual desktop members and are watched independently.
inline bool desktopDirectoryMatches(NSArray<NSURL*>* snapshot, NSSet<NSString*>* volumes,
                                    NSArray<NSURL*>* entries) {
    NSSet* all = desktopPathSet(snapshot);
    NSSet* actual = desktopPathSet(entries);
    if (!all || !actual || !volumes || ![volumes isSubsetOfSet:all]) return false;
    NSMutableSet* expected = [all mutableCopy];
    [expected minusSet:volumes];
    return [expected isEqualToSet:actual];
}

inline NSString* mirrorFailureMessage(const char* error, bool attempted, bool restored) {
    NSString* detail = error ? [NSString stringWithUTF8String:error] : nil;
    if (!detail.length) detail = @"读取或准备失败，请查看运行日志";
    NSString* state =
        !attempted ? @"尚未隐藏原图标，无需恢复。"
                   : (restored ? @"已完成程序恢复，请检查桌面。" : @"恢复未完成，请运行同目录独立恢复入口。");
    return [NSString stringWithFormat:@"未继续：%@。%@", detail, state];
}
}  // namespace dpp::mac
