// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host instrument (scripts/build-qemu-metal.py check): this Mac's QuartzCore renders one layer tree twice through CARenderer, once on this
// Mac's own Metal device and once on NativeMetal's frontend over QemuMetalRemote.cpp (the window, qemu-metal-host, this Mac's GPU again),
// and the two pictures must agree. The tree takes the renderer's usual paths: solid layers, opacity, rounded corners and borders, a shadow,
// an image's contents (a texture upload) and text. No Core Image filter: this Mac's Core Image takes newer paths than iPadOS 17's (Metal
// Performance Shaders for a blur, newLibraryWithDescriptor: for its kernels), so iPadOS's library paths (QemuMetalLibraries.mm) are not what it
// would check. The Mac's QuartzCore is not the guest's: this checks the wire and the host, not iPadOS.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreImage/CoreImage.h>
#import <ImageIO/ImageIO.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"

static CGColorRef color(CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    const CGFloat c[] = {r, g, b, a};
    CGColorRef made = CGColorCreate(space, c);
    CGColorSpaceRelease(space);
    return (CGColorRef)CFAutorelease(made);
}
static CGImageRef checker(size_t side) {
    std::vector<uint32_t> pixels(side * side);
    for (size_t y = 0; y < side; y++)
        for (size_t x = 0; x < side; x++) pixels[y * side + x] = ((x / 8 + y / 8) & 1) ? 0xff2080e0u : 0xfff0d020u;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(pixels.data(), side, side, 8, side * 4, space,
                                                 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    return (CGImageRef)CFAutorelease(image);
}
static CALayer *scene(CGFloat side) {
    CALayer *root = [CALayer layer];
    root.frame = CGRectMake(0, 0, side, side);
    root.backgroundColor = color(0.1, 0.2, 0.6, 1);
    CALayer *card = [CALayer layer];
    card.frame = CGRectMake(16, 16, 96, 64);
    card.backgroundColor = color(1, 1, 1, 1);
    card.cornerRadius = 12;
    card.shadowOpacity = 0.6;
    card.shadowRadius = 6;
    card.shadowOffset = CGSizeMake(0, 4);
    [root addSublayer:card];
    CALayer *image = [CALayer layer];
    image.frame = CGRectMake(136, 16, 96, 96);
    image.contents = (__bridge id)checker(64);
    image.cornerRadius = 20;
    image.masksToBounds = YES;
    [root addSublayer:image];
    CATextLayer *text = [CATextLayer layer];
    text.frame = CGRectMake(16, 128, 224, 40);
    text.string = @"QEMU Metal 12:34";
    text.fontSize = 24;
    text.foregroundColor = color(1, 1, 0.8, 1);
    text.contentsScale = 1;
    [root addSublayer:text];
    CALayer *veil = [CALayer layer];
    veil.frame = CGRectMake(64, 64, 128, 128);
    veil.backgroundColor = color(0, 1, 0.3, 0.5);
    [root addSublayer:veil];
    CALayer *rounded = [CALayer layer];
    rounded.frame = CGRectMake(24, 184, 208, 56);
    rounded.backgroundColor = color(1, 0.4, 0.2, 0.9);
    rounded.cornerRadius = 9;
    rounded.borderWidth = 3;
    rounded.borderColor = color(1, 1, 1, 0.8);
    [root addSublayer:rounded];
    return root;
}
// The tree rendered on the device, as BGRA rows; empty when something failed.
static std::vector<uint8_t> render(id<MTLDevice> device, NSUInteger side) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:side height:side mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [device newTextureWithDescriptor:d];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!target || !queue) return {};
    CARenderer *renderer = [CARenderer rendererWithMTLTexture:target options:@{kCARendererMetalCommandQueue: queue}];
    if (!renderer) return {};
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    renderer.layer = scene(side);
    renderer.bounds = CGRectMake(0, 0, side, side);
    [CATransaction commit];
    // The renderer only sees committed trees.
    [CATransaction flush];
    for (int frame = 0; frame < 2; frame++) {
        [renderer beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
        [renderer addUpdateRect:renderer.bounds];
        [renderer render];
        [renderer endFrame];
    }
    id<MTLCommandBuffer> last = [queue commandBuffer];
    [last commit];
    [last waitUntilCompleted];
    std::vector<uint8_t> pixels(side * side * 4);
    [target getBytes:pixels.data() bytesPerRow:side * 4 fromRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0];
    return pixels;
}
static void save(const std::vector<uint8_t> &pixels, NSUInteger side, const char *path) {
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate((void *)pixels.data(), side, side, 8, side * 4, space,
                                                 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGImageDestinationRef file = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(path)], CFSTR("public.png"), 1, nullptr);
    CGImageDestinationAddImage(file, image, nullptr);
    CGImageDestinationFinalize(file);
    CFRelease(file);
    CGImageRelease(image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
}

static void onMissing(const char *cls, const char *sel) {std::fprintf(stderr, "missing selector: %s %s\n", cls, sel);}
extern "C" void qemu_metal_complete_frontend(void *device);
extern "C" bool qemu_metal_answer(const char *selector, uint64_t *value);
extern "C" void qemu_metal_add_libraries(Class device);
extern "C" void qemu_metal_answer_gaps(Class cls);
// Device queries this Mac's QuartzCore sends that the frontend's device (the guest's) does not answer: every argument-less count of this
// Mac's own device class it lacks answers as this Mac's device does. Feature flags stay the device's own (QemuMetalRemote.cpp kFlags): a
// flag answered YES here sends the renderer to objects the frontend does not make (tile pipelines). Instrument only.
static id gMacDevice;
static NSUInteger macCount(id, SEL selector) {return ((NSUInteger (*)(id, SEL))objc_msgSend)(gMacDevice, selector);}
// The frontend refuses a pipeline with an error and no log; the instrument prints the error.
static IMP gComputeWithDescriptor;
static id loggedCompute(id self, SEL cmd, MTLComputePipelineDescriptor *d, MTLPipelineOption options, id *reflection, NSError **error) {
    NSError *local = nil;
    id made = ((id (*)(id, SEL, id, MTLPipelineOption, id *, NSError **))gComputeWithDescriptor)(self, cmd, d, options, reflection, &local);
    if (!made) std::fprintf(stderr, "compute pipeline %s refused: %s\n", d.label.UTF8String ?: "?", local.localizedDescription.UTF8String ?: "?");
    if (error) *error = local;
    return made;
}
static void answerMacQueries(id device) {
    // The selectors the frontend lacks are answered as on the guest (QemuMetalGaps.mm), named in the host's log.
    qemu_metal_answer_gaps(objc_getClass("NBObject"));
    qemu_metal_answer_gaps(object_getClass(device));
    Method compute = class_getInstanceMethod(object_getClass(device), @selector(newComputePipelineStateWithDescriptor:options:reflection:error:));
    gComputeWithDescriptor = method_setImplementation(compute, (IMP)loggedCompute);
    for (Class cls = object_getClass(gMacDevice); cls && cls != [NSObject class]; cls = class_getSuperclass(cls)) {
        unsigned count = 0;
        Method *methods = class_copyMethodList(cls, &count);
        for (unsigned i = 0; i < count; i++) {
            SEL selector = method_getName(methods[i]);
            const char *types = method_getTypeEncoding(methods[i]);
            if (!types || strchr(sel_getName(selector), ':') || strcmp(types + 1, "16@0:8") || [device respondsToSelector:selector]) continue;
            if (strchr("QqIi", types[0])) class_addMethod(object_getClass(device), selector, (IMP)macCount, "Q16@0:8");
        }
        free(methods);
    }
}

// argv: the directory for the two pictures.
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) return 2;
        const NSUInteger side = 256;
        gMacDevice = MTLCreateSystemDefaultDevice();
        std::vector<uint8_t> expected = render(gMacDevice, side);
        id<MTLDevice> remote = native_bridge_metal_open("", argv[1], 0x716d746c, onMissing);
        if (!remote) {std::printf("FAIL no remote device\n"); return 1;}
        qemu_metal_complete_frontend((__bridge void *)remote);
        qemu_metal_add_libraries(object_getClass(remote));
        answerMacQueries(remote);
        std::vector<uint8_t> actual = render(remote, side);
        if (expected.empty() || actual.empty()) {std::printf("FAIL nothing rendered (%s)\n", expected.empty() ? "Mac" : "remote"); return 1;}
        save(expected, side, [@(argv[1]) stringByAppendingPathComponent:@"check-mac.png"].UTF8String);
        save(actual, side, [@(argv[1]) stringByAppendingPathComponent:@"check-remote.png"].UTF8String);
        size_t differ = 0, blank = 0, worst = 0;
        for (size_t i = 0; i < expected.size(); i += 4) {
            size_t most = 0;
            for (int c = 0; c < 4; c++) most = std::max<size_t>(most, (size_t)std::abs(expected[i + c] - actual[i + c]));
            if (most > 8) differ++;
            worst = std::max(worst, most);
            if (!actual[i + 3]) blank++;
        }
        // A kernel from Metal source (QemuMetalLibraries.mm compiles it on the host), as iPadOS's Core Image writes one for a filter graph.
        {
            NSError *error = nil;
            id<MTLLibrary> library = [remote newLibraryWithSource:@"kernel void fill(device uint *out [[buffer(0)]], uint i [[thread_position_in_grid]])"
                                                                   " { out[i] = i * 3u + SHIFT; }"
                                                          options:[&] {MTLCompileOptions *o = [MTLCompileOptions new]; o.preprocessorMacros = @{@"SHIFT": @"7"}; return o;}()
                                                            error:&error];
            id<MTLFunction> function = [library newFunctionWithName:@"fill"];
            id<MTLComputePipelineState> pipeline = function ? [remote newComputePipelineStateWithFunction:function error:&error] : nil;
            id<MTLBuffer> buffer = [remote newBufferWithLength:256 * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> commandBuffer = [[remote newCommandQueue] commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:buffer offset:0 atIndex:0];
            [encoder dispatchThreads:MTLSizeMake(256, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [encoder endEncoding];
            [commandBuffer commit];
            [commandBuffer waitUntilCompleted];
            const uint32_t *words = (const uint32_t *)buffer.contents;
            bool right = pipeline != nil;
            for (uint32_t i = 0; right && i < 256; i++) right = words[i] == i * 3 + 7;
            if (!right) {std::printf("FAIL the kernel from source: %s\n", error.localizedDescription.UTF8String ?: "wrong values"); return 1;}
        }
        // A query this Mac's device does not answer is refused as unknown, and the host keeps serving.
        uint64_t unknown = 0;
        if (qemu_metal_answer("noSuchDeviceQuery", &unknown)) {std::printf("FAIL an unknown query was answered\n"); return 1;}
        if (!qemu_metal_answer("maxThreadgroupMemoryLength", &unknown) || !unknown) {
            std::printf("FAIL the host stopped answering after an unknown query\n");
            return 1;
        }
        char status[512];
        native_bridge_metal_status(status, sizeof status);
        std::printf("frontend: %s\n", status);
        std::printf("%zu of %zu pixels differ by more than 8 (largest %zu); %zu blank\n", differ, expected.size() / 4, worst, blank);
        // Rendering on two devices may round differently; a few edge pixels may differ.
        if (differ > expected.size() / 4 / 100 || blank == expected.size() / 4) {std::printf("FAIL the pictures disagree\n"); return 1;}
        std::printf("PASS: this Mac's CARenderer drew the same picture on %s\n", remote.name.UTF8String);
        return 0;
    }
}
