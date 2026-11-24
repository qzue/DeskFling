// Explicit native Quick Look service fixture; not registered with ctest.
#include "platforms/macos/document_thumbnail.h"
using namespace dpp::mac;
int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        [NSApp finishLaunching];
        NSURL* directory = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES];
        // Inputs are generated synthetic fixtures only; never enumerate Desktop.
        auto done = std::make_shared<std::atomic_bool>(false);
        __block int result = 0;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
          @autoreleasepool {
              auto cancelled = std::make_shared<std::atomic_bool>(false);
              for (NSString* name in
                   @[ @"sample.pdf", @"sample.docx", @"sample.xlsx", @"sample.json", @"sample.md" ]) {
                  auto value = documentThumbnail([directory URLByAppendingPathComponent:name], 128, 2,
                                                 std::chrono::steady_clock::now() + std::chrono::seconds(5),
                                                 cancelled);
                  std::printf("fixture=%s status=%s\n", name.UTF8String, [value[@"status"] UTF8String]);
                  if (![value[@"status"] isEqual:@"quicklook_content"]) {
                      result = 1;
                      continue;
                  }
                  CGImageRef image = (__bridge CGImageRef)value[@"image"];
                  NSURL* output =
                      [directory URLByAppendingPathComponent:[name stringByAppendingString:@".png"]];
                  CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
                      (__bridge CFURLRef)output, CFSTR("public.png"), 1, nullptr);
                  if (!destination) {
                      result = 1;
                      continue;
                  }
                  CGImageDestinationAddImage(destination, image, nullptr);
                  if (!CGImageDestinationFinalize(destination)) result = 1;
                  CFRelease(destination);
              }
              auto expired =
                  documentThumbnail([directory URLByAppendingPathComponent:@"sample.pdf"], 128, 2,
                                    std::chrono::steady_clock::now() - std::chrono::seconds(1), cancelled);
              const bool budget = [expired[@"status"] isEqual:@"batch_timeout"];
              cancelled->store(true);
              auto stopped =
                  documentThumbnail([directory URLByAppendingPathComponent:@"sample.pdf"], 128, 2,
                                    std::chrono::steady_clock::now() + std::chrono::seconds(1), cancelled);
              const bool cancellation = [stopped[@"status"] isEqual:@"cancelled"];
              std::printf("expired_budget_rejected=%s cancelled_request_rejected=%s\n",
                          budget ? "true" : "false", cancellation ? "true" : "false");
              if (!budget || !cancellation) result = 1;
              done->store(true);
          }
        });
        while (!done->load())
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.05]];
        return result;
    }
}
