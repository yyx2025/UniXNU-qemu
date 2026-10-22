// SPDX-License-Identifier: BSD-2-Clause-Patent
// Answers for the selectors NativeMetal's frontend lacks. The frontend raises on one (NBObject doesNotRecognizeSelector:), which kills a
// process on the OS's render path: here such a selector is logged once (qemu_metal_log, into the Mac's log) and answered instead, an
// argument-less count or flag by the Mac's device, anything else with zero (nil for an object). The log names each gap so it can be filled
// properly. QemuMetalPlugin.mm installs it on the device and every frontend object, as QemuMetalCheck.mm does on this Mac.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <vector>

extern "C" {
bool qemu_metal_answer(const char *selector, uint64_t *value);
void qemu_metal_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void qemu_metal_answer_gaps(Class cls);
}

namespace {
std::mutex gGapLock;
std::set<std::string> gGaps;
const char *protocolTypes(SEL selector) {
    Protocol *protocols[] = {@protocol(MTLDevice), @protocol(MTLTexture), @protocol(MTLBuffer), @protocol(MTLResource), @protocol(MTLCommandQueue),
                             @protocol(MTLCommandBuffer), @protocol(MTLRenderCommandEncoder), @protocol(MTLBlitCommandEncoder),
                             @protocol(MTLComputeCommandEncoder), @protocol(MTLCommandEncoder), @protocol(MTLLibrary), @protocol(MTLFunction),
                             @protocol(MTLRenderPipelineState), @protocol(MTLComputePipelineState), @protocol(MTLSamplerState),
                             @protocol(MTLDepthStencilState), @protocol(MTLHeap), @protocol(MTLFence), @protocol(MTLEvent), @protocol(MTLSharedEvent)};
    for (Protocol *p : protocols)
        for (BOOL required : {YES, NO}) {
            objc_method_description m = protocol_getMethodDescription(p, selector, required, YES);
            if (m.types) return m.types;
        }
    return nullptr;
}
NSMethodSignature *gapSignature(id self, SEL, SEL selector) {
    if (Method m = class_getInstanceMethod(object_getClass(self), selector)) return [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(m)];
    // Apple's SPI: words in, a word out; a feature query (supports…, is…) a flag.
    const char *name = sel_getName(selector);
    bool flag = false;
    for (const char *prefix : {"supports", "is", "are", "has", "should", "requires"})
        flag |= !strncmp(name, prefix, strlen(prefix)) && isupper((unsigned char)name[strlen(prefix)]);
    std::string guess = flag ? "B@:" : "Q@:";
    for (const char *c = name; *c; c++) if (*c == ':') guess += "Q";
    const char *types = protocolTypes(selector);
    return [NSMethodSignature signatureWithObjCTypes:types ?: guess.c_str()];
}
void gapForward(id self, SEL, NSInvocation *invocation) {
    const char *name = sel_getName(invocation.selector);
    NSMethodSignature *signature = invocation.methodSignature;
    const NSUInteger length = signature.methodReturnLength;
    uint64_t answer = 0;
    // Only the device's own counts are the Mac device's to answer. A feature flag the device lacks stays NO: the ones the frontend
    // implements are answered already (QemuMetalRemote.cpp kFlags), and a YES here sends the OS to objects it does not make (function
    // pointers, tile shaders).
    const bool asked = [self conformsToProtocol:@protocol(MTLDevice)] && !strchr(name, ':') && length && length <= 8 &&
                       strchr("QqIiLl", signature.methodReturnType[0]) && qemu_metal_answer(name, &answer);
    bool first;
    {
        std::lock_guard<std::mutex> lock(gGapLock);
        first = gGaps.insert(std::string(class_getName(object_getClass(self))) + " " + name).second;
    }
    if (first) qemu_metal_log("gap: %s %s answered %s%llu", class_getName(object_getClass(self)), name, asked ? "by the Mac: " : "zero: ",
                              (unsigned long long)answer);
    if (!length) return;
    std::vector<uint8_t> value(length);
    if (asked) memcpy(value.data(), &answer, std::min<size_t>(length, 8));
    [invocation setReturnValue:value.data()];
}
}

extern "C" void qemu_metal_answer_gaps(Class cls) {
    class_addMethod(cls, @selector(methodSignatureForSelector:), (IMP)gapSignature, "@24@0:8:16");
    class_addMethod(cls, @selector(forwardInvocation:), (IMP)gapForward, "v24@0:8@16");
}
