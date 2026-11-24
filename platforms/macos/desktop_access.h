#pragma once
#import <AppKit/AppKit.h>
@interface DPPDesktopAccess : NSObject <NSWindowDelegate>
- (void)show;
- (void)shutdown;
- (void)closeMirror;
@end
