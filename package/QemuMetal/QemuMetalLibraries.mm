// SPDX-License-Identifier: BSD-2-Clause-Patent
// The libraries NativeMetal's frontend does not make, over QemuMetalRemote.cpp; Core Image needs each to draw anything (the symbols of
// the icons iconservicesagent renders):
//   - libraries compiled from Metal source (newLibraryWithSource:…): Core Image writes a kernel for each filter graph its archives lack,
//     and on the target's GPU family it compiles that source. The base class's own compiler makes a library the frontend refuses to make
//     a pipeline from ("foreign function"), and the command buffer stays empty. The source goes to the Mac (QM_SOURCE_LIBRARY);
//   - function stitching (newLibraryWithStitchedDescriptor:…), which Core Image uses where the GPU family has it. The graph goes to the Mac
//     whole (QM_STITCHED_LIBRARY), where Metal stitches it from the same functions;
//   - externFunctionNames, by which Core Image finds a kernel's own library, and Metal's own error for a name a library has not, on which
//     it goes on to the next library.
// Each library that comes back is the frontend's own NBLibrary.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "QemuMetalWire.h"

extern "C" {
uint64_t qemu_metal_function_handle(const void *function);
void *qemu_metal_stitched_library(const void *payload, size_t bytes);
void *qemu_metal_source_library(const void *source, size_t length, const QMSource *options, const void *macros, size_t macroBytes);
long qemu_metal_extern_names(const void *library, char *out, size_t size);
const char *nmb_last_error(void);
void qemu_metal_add_libraries(Class device);
}

namespace {
NSError *failure(NSString *why) {
    return [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorCompileFailure userInfo:@{NSLocalizedDescriptionKey: why}];
}
void *handleOf(id object) {
    return object ? ((void *(*)(id, SEL))objc_msgSend)(object, sel_registerName("handle")) : nullptr;
}
struct Payload {
    std::vector<unsigned char> bytes;
    void word(uint64_t value) {append(&value, 8);}
    void append(const void *data, size_t n) {
        bytes.insert(bytes.end(), (const unsigned char *)data, (const unsigned char *)data + n);
        bytes.resize((bytes.size() + 7) / 8 * 8);
    }
    void text(NSString *value) {
        const char *s = value.UTF8String ?: "";
        append(s, strlen(s) + 1);
    }
};

// The request for a descriptor, or nil with the reason.
NSString *request(MTLStitchedLibraryDescriptor *descriptor, Payload &out) {
    Class functionClass = objc_getClass("NBFunction");
    const QMStitched header = {descriptor.functions.count, descriptor.functionGraphs.count};
    out.append(&header, sizeof header);
    for (id function in descriptor.functions) {
        if (![function isKindOfClass:functionClass]) return @"a function of another device";
        out.word(qemu_metal_function_handle(handleOf(function)));
    }
    for (MTLFunctionStitchingGraph *graph in descriptor.functionGraphs) {
        // Every function node the graph reaches, its own list first, each once.
        std::vector<MTLFunctionStitchingFunctionNode *> nodes;
        std::unordered_map<void *, uint64_t> index;
        auto add = [&](MTLFunctionStitchingFunctionNode *node) {
            if (node && !index.count((__bridge void *)node)) {index[(__bridge void *)node] = nodes.size(); nodes.push_back(node);}
        };
        for (MTLFunctionStitchingFunctionNode *node in graph.nodes) add(node);
        const size_t listed = nodes.size();
        add(graph.outputNode);
        for (size_t i = 0; i < nodes.size(); i++) {
            for (id argument in nodes[i].arguments)
                if ([argument isKindOfClass:[MTLFunctionStitchingFunctionNode class]]) add(argument);
            for (MTLFunctionStitchingFunctionNode *dependency in nodes[i].controlDependencies) add(dependency);
        }
        bool inlined = false;
        for (id attribute in graph.attributes) inlined |= [attribute isKindOfClass:[MTLFunctionStitchingAttributeAlwaysInline class]];
        if (!graph.outputNode) return @"a graph without an output node";
        const QMStitchGraph header = {strlen(graph.functionName.UTF8String ?: "") + 1, nodes.size(),
                                      index[(__bridge void *)graph.outputNode], inlined};
        out.append(&header, sizeof header);
        out.text(graph.functionName);
        for (size_t i = 0; i < nodes.size(); i++) {
            MTLFunctionStitchingFunctionNode *node = nodes[i];
            const QMStitchNode fields = {strlen(node.name.UTF8String ?: "") + 1, node.arguments.count, node.controlDependencies.count, i < listed};
            out.append(&fields, sizeof fields);
            out.text(node.name);
            for (id argument in node.arguments) {
                if ([argument isKindOfClass:[MTLFunctionStitchingInputNode class]])
                    out.word(QM_STITCH_INPUT | [(MTLFunctionStitchingInputNode *)argument argumentIndex]);
                else if ([argument isKindOfClass:[MTLFunctionStitchingFunctionNode class]])
                    out.word(index[(__bridge void *)argument]);
                else
                    return [NSString stringWithFormat:@"a stitching node of class %@", [argument class]];
            }
            for (MTLFunctionStitchingFunctionNode *dependency in node.controlDependencies) out.word(index[(__bridge void *)dependency]);
        }
    }
    return nil;
}

// The selectors here are of Metal's new… family: the caller owns what they return (ns_returns_retained), and so does whoever calls the
// frontend's own new… methods below.
#define RETAINED __attribute__((ns_returns_retained))
// The frontend's library over a handle of the bridge's.
RETAINED id libraryOver(void *handle) {
    id library = [objc_getClass("NBLibrary") new];
    ((void (*)(id, SEL, void *))objc_msgSend)(library, sel_registerName("setHandle:"), handle);
    return library;
}
RETAINED id stitched(id self, SEL, MTLStitchedLibraryDescriptor *descriptor, NSError **error) {
    (void)self;
    Payload payload;
    if (NSString *why = descriptor ? request(descriptor, payload) : @"no descriptor") {
        if (error) *error = failure(why);
        return nil;
    }
    void *handle = qemu_metal_stitched_library(payload.bytes.data(), payload.bytes.size());
    if (!handle) {
        if (error) *error = failure([NSString stringWithFormat:@"stitching: %s", nmb_last_error()]);
        return nil;
    }
    if (error) *error = nil;
    return libraryOver(handle);
}
RETAINED id compiled(id self, SEL, NSString *source, MTLCompileOptions *options, NSError **error) {
    (void)self;
    const char *text = source.UTF8String;
    if (!text) {
        if (error) *error = failure(@"no source");
        return nil;
    }
    QMSource fields = {0, strlen(text), options ? (uint64_t)options.fastMathEnabled : 1, options ? (uint64_t)options.languageVersion : 0, 0};
    std::string macros;
    for (NSString *name in options.preprocessorMacros) {
        id value = options.preprocessorMacros[name];
        macros += std::string(name.UTF8String ?: "") + "=" + ([value respondsToSelector:@selector(stringValue)] ? [value stringValue] : [value description]).UTF8String;
        macros += '\0';
        fields.macros++;
    }
    void *handle = qemu_metal_source_library(text, fields.length, &fields, macros.data(), macros.size());
    if (!handle) {
        if (error) *error = failure([NSString stringWithFormat:@"source: %s", nmb_last_error()]);
        return nil;
    }
    if (error) *error = nil;
    return libraryOver(handle);
}
void compiledLater(id self, SEL, NSString *source, MTLCompileOptions *options, void (^handler)(id<MTLLibrary>, NSError *)) {
    NSError *error = nil;
    id library = compiled(self, nullptr, source, options, &error);
    if (handler) handler(library, error);
}
void stitchedLater(id self, SEL, MTLStitchedLibraryDescriptor *descriptor, void (^handler)(id<MTLLibrary>, NSError *)) {
    NSError *error = nil;
    id library = stitched(self, nullptr, descriptor, &error);
    if (handler) handler(library, error);
}
// A name the library has not is Metal's MTLLibraryErrorFunctionNotFound, with Metal's text: Core Image looks a kernel up in its stdlib
// first and goes on to the kernel's own library only after that error; the frontend's own error stops it.
IMP gSpecialized, gDescribed;
NSError *notFound(NSError *error) {
    const char *why = nmb_last_error();
    if (!why || !strstr(why, "was not found in the library")) return error;
    return [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorFunctionNotFound userInfo:@{NSLocalizedDescriptionKey: @(why)}];
}
using Specialized = RETAINED id (*)(id, SEL, NSString *, id, NSError **);
using Described = RETAINED id (*)(id, SEL, id, NSError **);
RETAINED id specialized(id self, SEL selector, NSString *name, id values, NSError **error) {
    NSError *local = nil;
    id function = ((Specialized)gSpecialized)(self, selector, name, values, &local);
    if (error) *error = function ? nil : notFound(local);
    return function;
}
RETAINED id described(id self, SEL selector, id descriptor, NSError **error) {
    NSError *local = nil;
    id function = ((Described)gDescribed)(self, selector, descriptor, &local);
    if (error) *error = function ? nil : notFound(local);
    return function;
}
NSArray<NSString *> *externNames(id self, SEL) {
    std::vector<char> text(1 << 16);
    const long count = qemu_metal_extern_names(handleOf(self), text.data(), text.size());
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (size_t at = 0; count > 0 && (long)names.count < count && at < text.size() && text[at];) {
        NSString *name = [NSString stringWithUTF8String:text.data() + at];
        at += strlen(text.data() + at) + 1;
        if (name) [names addObject:name];
    }
    return names;
}
}

extern "C" void qemu_metal_add_libraries(Class device) {
    class_addMethod(device, @selector(newLibraryWithStitchedDescriptor:error:), (IMP)stitched, "@32@0:8@16^@24");
    class_addMethod(device, @selector(newLibraryWithStitchedDescriptor:completionHandler:), (IMP)stitchedLater, "v32@0:8@16@?24");
    class_addMethod(device, @selector(newLibraryWithSource:options:error:), (IMP)compiled, "@40@0:8@16@24^@32");
    class_addMethod(device, @selector(newLibraryWithSource:options:completionHandler:), (IMP)compiledLater, "v40@0:8@16@24@?32");
    Class library = objc_getClass("NBLibrary");
    if (!library) return;
    class_addMethod(library, sel_registerName("externFunctionNames"), (IMP)externNames, "@16@0:8");
    if (!gSpecialized)
        gSpecialized = method_setImplementation(class_getInstanceMethod(library, @selector(newFunctionWithName:constantValues:error:)), (IMP)specialized);
    if (!gDescribed)
        gDescribed = method_setImplementation(class_getInstanceMethod(library, @selector(newFunctionWithDescriptor:error:)), (IMP)described);
}
