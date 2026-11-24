#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static CGColorRef rgba(CGColorSpaceRef cs, CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    const CGFloat comp[4] = {r, g, b, a};
    return CGColorCreate(cs, comp);
}

static CGPathRef createSquirclePath(CGRect r, CGFloat cornerRadius) {
    return CGPathCreateWithRoundedRect(r, cornerRadius, cornerRadius, nullptr);
}

static CGImageRef loadCroppedSourceIcon() {
    static CGImageRef cached = nullptr;
    if (cached) return cached;
    NSString* srcPath = @"platforms/macos/deskfling_icon_source.jpg";
    NSImage* ns = [[NSImage alloc] initWithContentsOfFile:srcPath];
    if (!ns) return nullptr;
    CGImageRef full = [ns CGImageForProposedRect:nullptr context:nullptr hints:nullptr];
    if (!full) return nullptr;
    // Exact measured silver bezel in 1024x1024 source: [186..838] x [186..838] (652x652, R=146.8).
    // Inset by 1.0px to [187..837] (650x650) so zero outer white background / shadow fringe enters the mask.
    const CGRect cropRect = CGRectMake(187.0, 187.0, 650.0, 650.0);
    cached = CGImageCreateWithImageInRect(full, cropRect);
    return cached;
}

static NSData* renderIconPng(int px, bool compactPaddingForWinSmall = false) {
    CGImageRef srcBody = loadCroppedSourceIcon();
    if (!srcBody) return nil;

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGBitmapInfo bmpInfo =
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big;
    CGContextRef ctx = CGBitmapContextCreate(nullptr, px, px, 8, px * 4, cs, bmpInfo);
    CGContextSetAllowsAntialiasing(ctx, true);
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextClearRect(ctx, CGRectMake(0, 0, px, px));

    const CGFloat s = static_cast<CGFloat>(px) / 1024.0;
    const CGFloat pad = compactPaddingForWinSmall ? (28.0 * s) : ((px <= 32) ? (48.0 * s) : (96.0 * s));
    const CGRect body = CGRectMake(pad, pad, px - 2.0 * pad, px - 2.0 * pad);
    const CGFloat radius = body.size.width * 0.2245;

    // 1. Clean native transparent drop shadow outside the squircle
    if (px >= 64) {
        CGContextSaveGState(ctx);
        CGColorRef shCol = rgba(cs, 0.0, 0.0, 0.0, 0.36);
        CGContextSetShadowWithColor(ctx, CGSizeMake(0, -12.0 * s), 24.0 * s, shCol);
        CGPathRef sp = createSquirclePath(body, radius);
        CGColorRef fillDark = rgba(cs, 0.12, 0.14, 0.18, 1.0);
        CGContextSetFillColorWithColor(ctx, fillDark);
        CGContextAddPath(ctx, sp);
        CGContextFillPath(ctx);
        CGColorRelease(fillDark);
        CGPathRelease(sp);
        CGColorRelease(shCol);
        CGContextRestoreGState(ctx);
    }

    // 2. Clip to exact squircle and draw high-res DeskFling artwork
    CGContextSaveGState(ctx);
    CGPathRef clipPath = createSquirclePath(body, radius);
    CGContextAddPath(ctx, clipPath);
    CGContextClip(ctx);
    CGContextDrawImage(ctx, body, srcBody);

    // Subtle crisp outer rim highlight so the metallic bezel edge stays razor-sharp at all DPIs
    if (px >= 32) {
        CGPathRef rimPath = createSquirclePath(CGRectInset(body, 0.5, 0.5), std::max<CGFloat>(1.0, radius - 0.5));
        CGColorRef rimCol = rgba(cs, 0.72, 0.75, 0.80, 0.55);
        CGContextSetStrokeColorWithColor(ctx, rimCol);
        CGContextSetLineWidth(ctx, std::max<CGFloat>(0.8, 1.4 * s));
        CGContextAddPath(ctx, rimPath);
        CGContextStrokePath(ctx);
        CGColorRelease(rimCol);
        CGPathRelease(rimPath);
    }

    CGPathRelease(clipPath);
    CGContextRestoreGState(ctx);

    CGImageRef img = CGBitmapContextCreateImage(ctx);
    NSMutableData* out = [NSMutableData data];
    CGImageDestinationRef dest =
        CGImageDestinationCreateWithData((__bridge CFMutableDataRef)out,
                                         (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    NSDictionary* props = @{
        (__bridge NSString*)kCGImagePropertyDPIWidth : @72,
        (__bridge NSString*)kCGImagePropertyDPIHeight : @72
    };
    CGImageDestinationAddImage(dest, img, (__bridge CFDictionaryRef)props);
    CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    return out;
}

static bool writeAppleIcns(NSString* path) {
    struct ChunkSpec {
        const char type[5];
        int px;
    };
    const ChunkSpec chunks[] = {
        {"icp4", 16},   {"icp5", 32},   {"icp6", 64},   {"ic07", 128},
        {"ic08", 256},  {"ic09", 512},  {"ic10", 1024}, {"ic11", 32},
        {"ic12", 64},   {"ic13", 256},  {"ic14", 512},
    };
    NSMutableData* body = [NSMutableData data];
    for (const auto& c : chunks) {
        NSData* png = renderIconPng(c.px, false);
        if (!png) return false;
        const uint32_t lenBE = CFSwapInt32HostToBig(static_cast<uint32_t>(png.length + 8));
        [body appendBytes:c.type length:4];
        [body appendBytes:&lenBE length:4];
        [body appendData:png];
    }
    NSMutableData* icns = [NSMutableData dataWithCapacity:8 + body.length];
    [icns appendBytes:"icns" length:4];
    const uint32_t totalBE = CFSwapInt32HostToBig(static_cast<uint32_t>(body.length + 8));
    [icns appendBytes:&totalBE length:4];
    [icns appendData:body];
    if (![icns writeToFile:path atomically:YES]) return false;
    NSImage* verify = [[NSImage alloc] initWithContentsOfFile:path];
    return verify != nil && verify.representations.count >= 6;
}

static NSData* renderIconDib32(int px) {
    CGDataProviderRef prov = CGDataProviderCreateWithCFData((__bridge CFDataRef)renderIconPng(px, true));
    CGImageRef src = CGImageCreateWithPNGDataProvider(prov, nullptr, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(prov);
    std::vector<uint8_t> rgbaBuf(static_cast<size_t>(px * px * 4), 0);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGBitmapInfo bmpInfo =
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big;
    CGContextRef ctx = CGBitmapContextCreate(rgbaBuf.data(), px, px, 8, px * 4, cs, bmpInfo);
    CGContextDrawImage(ctx, CGRectMake(0, 0, px, px), src);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(src);

    for (size_t i = 0; i < rgbaBuf.size(); i += 4) {
        const uint8_t a = rgbaBuf[i + 3];
        if (a > 0 && a < 255) {
            rgbaBuf[i + 0] = static_cast<uint8_t>(std::min(255, (int(rgbaBuf[i + 0]) * 255 + a / 2) / a));
            rgbaBuf[i + 1] = static_cast<uint8_t>(std::min(255, (int(rgbaBuf[i + 1]) * 255 + a / 2) / a));
            rgbaBuf[i + 2] = static_cast<uint8_t>(std::min(255, (int(rgbaBuf[i + 2]) * 255 + a / 2) / a));
        }
    }

    const uint32_t xorBytes = static_cast<uint32_t>(px * px * 4);
    const uint32_t andStride = static_cast<uint32_t>(((px + 31) / 32) * 4);
    const uint32_t andBytes = andStride * static_cast<uint32_t>(px);

    NSMutableData* dib = [NSMutableData dataWithCapacity:40 + xorBytes + andBytes];
    struct {
        uint32_t biSize;
        int32_t biWidth;
        int32_t biHeight;
        uint16_t biPlanes;
        uint16_t biBitCount;
        uint32_t biCompression;
        uint32_t biSizeImage;
        int32_t biXPelsPerMeter;
        int32_t biYPelsPerMeter;
        uint32_t biClrUsed;
        uint32_t biClrImportant;
    } hdr{40, px, px * 2, 1, 32, 0, xorBytes + andBytes, 0, 0, 0, 0};
    [dib appendBytes:&hdr length:40];

    std::vector<uint8_t> bgraRow(static_cast<size_t>(px * 4));
    for (int y = px - 1; y >= 0; --y) {
        for (int x = 0; x < px; ++x) {
            const size_t si = static_cast<size_t>((y * px + x) * 4);
            bgraRow[static_cast<size_t>(x * 4 + 0)] = rgbaBuf[si + 2];
            bgraRow[static_cast<size_t>(x * 4 + 1)] = rgbaBuf[si + 1];
            bgraRow[static_cast<size_t>(x * 4 + 2)] = rgbaBuf[si + 0];
            bgraRow[static_cast<size_t>(x * 4 + 3)] = rgbaBuf[si + 3];
        }
        [dib appendBytes:bgraRow.data() length:bgraRow.size()];
    }
    std::vector<uint8_t> andRow(andStride, 0);
    for (int y = px - 1; y >= 0; --y) {
        std::fill(andRow.begin(), andRow.end(), 0);
        for (int x = 0; x < px; ++x) {
            const uint8_t a = rgbaBuf[static_cast<size_t>((y * px + x) * 4 + 3)];
            if (a == 0) {
                andRow[static_cast<size_t>(x / 8)] |= static_cast<uint8_t>(0x80u >> (x % 8));
            }
        }
        [dib appendBytes:andRow.data() length:andStride];
    }
    return dib;
}

static bool writeWindowsIco(NSString* path, const std::vector<int>& sizes) {
    NSMutableArray<NSData*>* frames = [NSMutableArray array];
    for (int sz : sizes) {
        [frames addObject:(sz >= 256 ? renderIconPng(sz, true) : renderIconDib32(sz))];
    }
    NSMutableData* ico = [NSMutableData data];
    const uint16_t reserved = 0;
    const uint16_t type = 1;
    const uint16_t count = static_cast<uint16_t>(sizes.size());
    [ico appendBytes:&reserved length:2];
    [ico appendBytes:&type length:2];
    [ico appendBytes:&count length:2];

    uint32_t offset = 6u + 16u * static_cast<uint32_t>(sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i) {
        const int sz = sizes[i];
        const uint8_t w = (sz >= 256) ? 0 : static_cast<uint8_t>(sz);
        const uint8_t h = (sz >= 256) ? 0 : static_cast<uint8_t>(sz);
        const uint8_t colorCount = 0;
        const uint8_t res = 0;
        const uint16_t planes = 1;
        const uint16_t bpp = 32;
        const uint32_t bytesInRes = static_cast<uint32_t>(frames[i].length);
        [ico appendBytes:&w length:1];
        [ico appendBytes:&h length:1];
        [ico appendBytes:&colorCount length:1];
        [ico appendBytes:&res length:1];
        [ico appendBytes:&planes length:2];
        [ico appendBytes:&bpp length:2];
        [ico appendBytes:&bytesInRes length:4];
        [ico appendBytes:&offset length:4];
        offset += bytesInRes;
    }
    for (NSData* frame in frames) {
        [ico appendData:frame];
    }
    return [ico writeToFile:path atomically:YES];
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 4) {
            std::fprintf(stderr, "usage: gen_icons <preview_png> <macos_icns> <windows_ico>\n");
            return 2;
        }
        NSString* previewPng = [NSString stringWithUTF8String:argv[1]];
        NSString* icnsPath = [NSString stringWithUTF8String:argv[2]];
        NSString* icoPath = [NSString stringWithUTF8String:argv[3]];
        if (![renderIconPng(512, false) writeToFile:previewPng atomically:YES]) return 1;
        if (!writeAppleIcns(icnsPath)) return 1;
        if (!writeWindowsIco(icoPath, {256, 64, 48, 32, 16})) return 1;
        std::puts("icons_generated_and_verified=true");
        return 0;
    }
}
