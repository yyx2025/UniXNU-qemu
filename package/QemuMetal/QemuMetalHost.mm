// SPDX-License-Identifier: BSD-2-Clause-Patent
// qemu-metal-host: the Mac side of the guest's Metal device (QemuMetalRemote.cpp). It maps one slot of the memory window QEMU shares with
// the guest (run-qemu.py --shared-window: RUN_DIR/shared.bin; QemuMetalWire.h qm_slot), answers the requests of the guest process that owns
// the slot with this Mac's Metal device, and runs that process's command buffers on this Mac's GPU. run-qemu.py --metal runs one per slot.
//
//   qemu-metal-host --window RUN_DIR/shared.bin [--slot N] [--log FILE] [--verbose]
//
// The slot's heap is one MTLBuffer over the mapped file, so a guest buffer is that buffer at the guest's offset, and a texture over guest
// bytes (an IOSurface's shadow) is a linear texture on it: the GPU reads and writes the guest's memory where it lies. Libraries arrive as
// the guest's metallib files; an iPadOS library is universal, and its AIR slice (kits/AirNir/Library.h) is what this Mac compiles.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <CommonCrypto/CommonDigest.h>
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include "Library.h"
#include "QemuMetalWire.h"

static unsigned char *gWindow;
static uint64_t gWindowBytes;
static id<MTLDevice> gDevice;
static id<MTLBuffer> gHeap;                         // the window from QM_HEAP
static id<MTLCommandQueue> gUtility;               // replaceRegion / getBytes
static FILE *gLog;
static unsigned gSlot;                             // --slot: the window's slot this process serves
static bool gVerbose;                              // --verbose: every request and command
static std::unordered_map<uint64_t, id> gObjects;
static uint64_t gNextHandle = 1;
static uint64_t gSession;
static std::mutex gRingLock;
static std::atomic<uint64_t> gGeneration;          // a new session's completions only
static NSMutableDictionary<NSData *, id<MTLLibrary>> *gLibraries;   // by the SHA-256 of the file, across sessions
static struct {uint64_t requests, commits, failures, completions;} gCount;

static QMControl *control() {return reinterpret_cast<QMControl *>(gWindow + QM_CONTROL);}
static uint64_t load(const uint64_t *word) {return __atomic_load_n(word, __ATOMIC_ACQUIRE);}
static void store(uint64_t *word, uint64_t value) {__atomic_store_n(word, value, __ATOMIC_RELEASE);}
static void note(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    char text[1024];
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    fprintf(gLog, "%.3f [%u] %s\n", [NSDate timeIntervalSinceReferenceDate], gSlot, text);
    fflush(gLog);
}

struct Failure {std::string why; int32_t status = QM_FAILED;};
[[noreturn]] static void refuse(const std::string &why) {throw Failure{why};}
template <class T> static T object(uint64_t handle, const char *what) {
    auto it = gObjects.find(handle);
    if (it == gObjects.end()) refuse(std::string("no ") + what + " " + std::to_string(handle));
    return (T)it->second;
}
static uint64_t keep(id value, const char *what) {
    if (!value) refuse(std::string("Metal made no ") + what);
    gObjects[gNextHandle] = value;
    return gNextHandle++;
}
// Window bytes at a window offset, as the heap buffer's offset; the range must lie in the heap.
static uint64_t heapOffset(uint64_t offset, uint64_t length) {
    if (offset < QM_HEAP || offset > gWindowBytes || length > gWindowBytes - offset) refuse("window range outside the heap");
    return offset - QM_HEAP;
}

// ---- replies ----
struct Out {
    uint64_t handle = 0;
    std::vector<unsigned char> data;
    void word(uint64_t value) {append(&value, 8);}
    void append(const void *bytes, size_t n) {data.insert(data.end(), (const unsigned char *)bytes, (const unsigned char *)bytes + n);}
};
template <class T> static const T &fields(const std::vector<unsigned char> &in) {
    if (in.size() < sizeof(T)) refuse("short request");
    return *reinterpret_cast<const T *>(in.data());
}
static void describe(id<MTLTexture> texture, Out &out) {
    const QMTextureDescriptor d = {(uint64_t)texture.textureType, (uint64_t)texture.pixelFormat, texture.width, texture.height, texture.depth,
                                   texture.mipmapLevelCount, texture.sampleCount, texture.arrayLength, (uint64_t)texture.usage,
                                   (uint64_t)texture.resourceOptions};
    out.append(&d, sizeof d);
}
static MTLTextureDescriptor *textureDescriptor(const QMTextureDescriptor &w) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = (MTLTextureType)w.type;
    d.pixelFormat = (MTLPixelFormat)w.format;
    d.width = w.width;
    d.height = std::max<uint64_t>(w.height, 1);
    d.depth = std::max<uint64_t>(w.depth, 1);
    d.mipmapLevelCount = std::max<uint64_t>(w.levels, 1);
    d.sampleCount = std::max<uint64_t>(w.samples, 1);
    d.arrayLength = std::max<uint64_t>(w.arrayLength, 1);
    d.usage = (MTLTextureUsage)w.usage;
    d.resourceOptions = (MTLResourceOptions)w.options;
    return d;
}
// Bytes per texel of the uncompressed formats a linear texture or a staged copy uses (0: not known here).
static uint64_t texelBytes(uint64_t format) {
    switch (format) {
    case MTLPixelFormatA8Unorm: case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB: case MTLPixelFormatR8Snorm:
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: case MTLPixelFormatStencil8: return 1;
    case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint:
    case MTLPixelFormatR16Float: case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Unorm_sRGB: case MTLPixelFormatRG8Snorm:
    case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint: case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatA1BGR5Unorm:
    case MTLPixelFormatABGR4Unorm: case MTLPixelFormatBGR5A1Unorm: case MTLPixelFormatDepth16Unorm: return 2;
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatR32Float: case MTLPixelFormatRG16Unorm:
    case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRG16Float:
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatRGBA8Snorm: case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatRGBA8Sint: case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB: case MTLPixelFormatRGB10A2Unorm:
    case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG11B10Float: case MTLPixelFormatRGB9E5Float: case MTLPixelFormatBGR10A2Unorm:
    case MTLPixelFormatBGR10_XR: case MTLPixelFormatBGR10_XR_sRGB: case MTLPixelFormatDepth32Float: return 4;
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatBGRA10_XR: case MTLPixelFormatBGRA10_XR_sRGB: return 8;
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatRGBA32Float: return 16;
    default: return 0;
    }
}
static void waitUtility(id<MTLCommandBuffer> commandBuffer) {
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    if (commandBuffer.status != MTLCommandBufferStatusCompleted) refuse(commandBuffer.error.localizedDescription.UTF8String ?: "copy failed");
}

// ---- the session ----
static void reset(uint64_t session) {
    std::lock_guard<std::mutex> lock(gRingLock);
    gObjects.clear();
    gSession = session;
    gGeneration++;
    store(&control()->completionsWritten, 0);
    note("session %llx", (unsigned long long)session);
}
static void writeCompletion(uint64_t generation, uint64_t number, uint64_t status, double start, double end, const char *error) {
    std::lock_guard<std::mutex> lock(gRingLock);
    if (generation != gGeneration) return;
    const uint64_t written = load(&control()->completionsWritten);
    // The guest takes them in order; a full ring waits for it (it never stops taking while it lives).
    for (unsigned round = 0; written - load(&control()->completionsRead) >= QM_COMPLETION_SLOTS; round++) {
        if (round > 20000) {note("completion ring full: the guest stopped taking them"); return;}
        usleep(100);
    }
    QMCompletion record = {number, status, start, end, {}};
    if (error) snprintf(record.error, sizeof record.error, "%s", error);
    memcpy(gWindow + QM_COMPLETIONS + (written % QM_COMPLETION_SLOTS) * sizeof record, &record, sizeof record);
    store(&control()->completionsWritten, written + 1);
    gCount.completions++;
}

// ---- answers ----
static bool answerSelector(const char *name, bool argument, uint64_t value, uint64_t &out) {
    SEL selector = sel_registerName(name);
    Method method = class_getInstanceMethod(object_getClass(gDevice), selector);
    if (!method) return false;
    const char *types = method_getTypeEncoding(method);
    if (!types) return false;
    switch (types[0]) {
    case 'B': case 'c':
        out = argument ? ((BOOL (*)(id, SEL, NSUInteger))objc_msgSend)(gDevice, selector, value)
                       : ((BOOL (*)(id, SEL))objc_msgSend)(gDevice, selector);
        return true;
    case 'Q': case 'q': case 'I': case 'i':
        out = argument ? ((NSUInteger (*)(id, SEL, NSUInteger))objc_msgSend)(gDevice, selector, value)
                       : ((NSUInteger (*)(id, SEL))objc_msgSend)(gDevice, selector);
        if (types[0] == 'I' || types[0] == 'i') out &= 0xffffffffu;
        return true;
    default:
        return false;
    }
}

// ---- commands ----
struct Encoding {
    id<MTLCommandBuffer> commandBuffer;
    id<MTLRenderCommandEncoder> render;
    id<MTLComputeCommandEncoder> compute;
    id<MTLBlitCommandEncoder> blit;
    uint64_t width = 0, height = 0;                 // the render pass's, to keep scissors inside it
    uint64_t visibility = QM_NONE;
    std::vector<std::pair<id<MTLBuffer>, uint64_t>> visibilityCopies;   // a pass's results, into the window once complete
    id<MTLCommandEncoder> current() const {return render ? (id<MTLCommandEncoder>)render : compute ? (id<MTLCommandEncoder>)compute : blit;}
    void end() {
        [current() endEncoding];
        render = nil; compute = nil; blit = nil;
    }
};
static MTLRenderPassDescriptor *passDescriptor(const QCRenderPass &p, Encoding &e) {
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    auto attach = [&](const QCAttachment &a, MTLRenderPassAttachmentDescriptor *out) {
        if (!a.texture) return;
        id<MTLTexture> texture = object<id<MTLTexture>>(a.texture, "attachment texture");
        out.texture = texture;
        out.level = a.level;
        out.slice = a.slice;
        out.depthPlane = a.depthPlane;
        out.loadAction = (MTLLoadAction)a.load;
        out.storeAction = (MTLStoreAction)a.store;
        if (!e.width) {e.width = texture.width >> a.level ?: 1; e.height = texture.height >> a.level ?: 1;}
    };
    for (unsigned i = 0; i < 8; i++) {
        attach(p.colors[i], d.colorAttachments[i]);
        d.colorAttachments[i].clearColor = MTLClearColorMake(p.colors[i].clear[0], p.colors[i].clear[1], p.colors[i].clear[2], p.colors[i].clear[3]);
    }
    attach(p.depth, d.depthAttachment);
    d.depthAttachment.clearDepth = p.depth.clear[0];
    attach(p.stencil, d.stencilAttachment);
    d.stencilAttachment.clearStencil = (uint32_t)p.stencil.clear[0];
    if (p.width) {d.renderTargetWidth = p.width; if (!e.width) e.width = p.width;}
    if (p.height) {d.renderTargetHeight = p.height; if (!e.height) e.height = p.height;}
    if (p.arrayLength) d.renderTargetArrayLength = p.arrayLength;
    if (p.samples) d.defaultRasterSampleCount = p.samples;
    if (p.visibility != QM_NONE) {
        // Visibility offsets are limited (maxVisibilityQueryOffset): the pass gets a buffer of its own, copied into the window afterwards.
        id<MTLBuffer> results = [gDevice newBufferWithLength:65536 options:MTLResourceStorageModeShared];
        d.visibilityResultBuffer = results;
        e.visibility = p.visibility;
        e.visibilityCopies.push_back({results, p.visibility});
    }
    return d;
}
template <class T> static const T &take(const unsigned char *body, uint32_t bytes) {
    if (bytes < sizeof(T)) refuse("short command");
    return *reinterpret_cast<const T *>(body);
}
static void bind(Encoding &e, const QCBind &b, id<MTLBuffer> buffer, uint64_t offset, id<MTLTexture> texture, id<MTLSamplerState> sampler, int kind) {
    const NSUInteger index = (NSUInteger)b.index;
    switch (b.stage) {
    case 0:
        if (!e.compute) refuse("compute binding outside a compute encoder");
        if (kind == 0) [e.compute setBuffer:buffer offset:offset atIndex:index];
        else if (kind == 1) [e.compute setTexture:texture atIndex:index];
        else [e.compute setSamplerState:sampler atIndex:index];
        break;
    case 1: case 2:
        if (!e.render) refuse("render binding outside a render encoder");
        if (b.stage == 1) {
            if (kind == 0) [e.render setVertexBuffer:buffer offset:offset atIndex:index];
            else if (kind == 1) [e.render setVertexTexture:texture atIndex:index];
            else [e.render setVertexSamplerState:sampler atIndex:index];
        } else {
            if (kind == 0) [e.render setFragmentBuffer:buffer offset:offset atIndex:index];
            else if (kind == 1) [e.render setFragmentTexture:texture atIndex:index];
            else [e.render setFragmentSamplerState:sampler atIndex:index];
        }
        break;
    default:
        refuse("bad stage");
    }
}
static void encode(Encoding &e, uint32_t op, const unsigned char *body, uint32_t bytes) {
    switch (op) {
    case QC_SIGNAL_EVENT: case QC_WAIT_EVENT: {
        const QCEvent &f = take<QCEvent>(body, bytes);
        if (e.current()) refuse("event inside an encoder");
        id<MTLEvent> event = object<id<MTLEvent>>(f.event, "event");
        if (op == QC_SIGNAL_EVENT) [e.commandBuffer encodeSignalEvent:event value:f.value];
        else [e.commandBuffer encodeWaitForEvent:event value:f.value];
        break;
    }
    case QC_RENDER: {
        if (e.current()) refuse("encoder inside an encoder");
        e.width = e.height = 0;
        e.render = [e.commandBuffer renderCommandEncoderWithDescriptor:passDescriptor(take<QCRenderPass>(body, bytes), e)];
        if (!e.render) refuse("no render encoder");
        break;
    }
    case QC_COMPUTE:
        if (e.current()) refuse("encoder inside an encoder");
        e.compute = [e.commandBuffer computeCommandEncoder];
        break;
    case QC_BLIT:
        if (e.current()) refuse("encoder inside an encoder");
        e.blit = [e.commandBuffer blitCommandEncoder];
        break;
    case QC_END:
        if (!e.current()) refuse("end without an encoder");
        e.end();
        break;
    case QC_UPDATE_FENCE: case QC_WAIT_FENCE: {
        const QCFence &f = take<QCFence>(body, bytes);
        id<MTLFence> fence = object<id<MTLFence>>(f.fence, "fence");
        const bool update = op == QC_UPDATE_FENCE;
        if (e.render) {
            if (update) [e.render updateFence:fence afterStages:(MTLRenderStages)(f.stages ?: MTLRenderStageFragment)];
            else [e.render waitForFence:fence beforeStages:(MTLRenderStages)(f.stages ?: MTLRenderStageVertex)];
        } else if (e.compute) {
            if (update) [e.compute updateFence:fence]; else [e.compute waitForFence:fence];
        } else if (e.blit) {
            if (update) [e.blit updateFence:fence]; else [e.blit waitForFence:fence];
        } else refuse("fence outside an encoder");
        break;
    }
    case QC_BUFFER: {
        const QCBind &b = take<QCBind>(body, bytes);
        if (b.value == QM_NONE) bind(e, b, nil, 0, nil, nil, 0);
        else bind(e, b, gHeap, heapOffset(b.value, 0), nil, nil, 0);
        break;
    }
    case QC_BYTES: {
        const QCBind &b = take<QCBind>(body, bytes);
        if (b.value > bytes - sizeof b) refuse("short bytes");
        const unsigned char *data = body + sizeof b;
        if (b.value <= 4096) {
            if (b.stage == 0 && e.compute) [e.compute setBytes:data length:b.value atIndex:b.index];
            else if (b.stage == 1 && e.render) [e.render setVertexBytes:data length:b.value atIndex:b.index];
            else if (b.stage == 2 && e.render) [e.render setFragmentBytes:data length:b.value atIndex:b.index];
            else refuse("bytes outside a matching encoder");
        } else {
            // setBytes takes 4 KiB; larger ones become a buffer of their own.
            bind(e, b, [gDevice newBufferWithBytes:data length:b.value options:MTLResourceStorageModeShared], 0, nil, nil, 0);
        }
        break;
    }
    case QC_TEXTURE: {
        const QCBind &b = take<QCBind>(body, bytes);
        bind(e, b, nil, 0, b.value ? object<id<MTLTexture>>(b.value, "texture") : nil, nil, 1);
        break;
    }
    case QC_SAMPLER: {
        const QCBind &b = take<QCBind>(body, bytes);
        bind(e, b, nil, 0, nil, b.value ? object<id<MTLSamplerState>>(b.value, "sampler") : nil, 2);
        break;
    }
    case QC_COMPUTE_PIPELINE:
        if (!e.compute) refuse("pipeline outside a compute encoder");
        [e.compute setComputePipelineState:object<id<MTLComputePipelineState>>(take<uint64_t>(body, bytes), "compute pipeline")];
        break;
    case QC_THREADGROUP_MEMORY: {
        if (!e.compute || bytes < 16) refuse("threadgroup memory outside a compute encoder");
        const uint64_t *f = (const uint64_t *)body;
        [e.compute setThreadgroupMemoryLength:f[0] atIndex:f[1]];
        break;
    }
    case QC_DISPATCH: {
        const QCDispatch &f = take<QCDispatch>(body, bytes);
        if (!e.compute) refuse("dispatch outside a compute encoder");
        const MTLSize size = MTLSizeMake(f.size[0], f.size[1], f.size[2]), group = MTLSizeMake(f.perGroup[0], f.perGroup[1], f.perGroup[2]);
        if (f.threads) [e.compute dispatchThreads:size threadsPerThreadgroup:group];
        else [e.compute dispatchThreadgroups:size threadsPerThreadgroup:group];
        break;
    }
    case QC_RENDER_PIPELINE:
        if (!e.render) refuse("pipeline outside a render encoder");
        [e.render setRenderPipelineState:object<id<MTLRenderPipelineState>>(take<uint64_t>(body, bytes), "render pipeline")];
        break;
    case QC_DEPTH_STENCIL: {
        if (!e.render) refuse("depth state outside a render encoder");
        const uint64_t handle = take<uint64_t>(body, bytes);
        [e.render setDepthStencilState:handle ? object<id<MTLDepthStencilState>>(handle, "depth stencil state") : nil];
        break;
    }
    case QC_VIEWPORT: {
        if (!e.render || bytes < 48) refuse("viewport outside a render encoder");
        const double *v = (const double *)body;
        [e.render setViewport:(MTLViewport){v[0], v[1], v[2], v[3], v[4], v[5]}];
        break;
    }
    case QC_SCISSOR: {
        if (!e.render || bytes < 32) refuse("scissor outside a render encoder");
        const uint64_t *r = (const uint64_t *)body;
        // Metal refuses a scissor beyond the attachments; the guest's GPU clipped it.
        uint64_t x = std::min(r[0], e.width), y = std::min(r[1], e.height);
        uint64_t w = std::min(r[2], e.width - x), h = std::min(r[3], e.height - y);
        [e.render setScissorRect:(MTLScissorRect){x, y, w, h}];
        break;
    }
    case QC_RASTER: {
        if (!e.render || bytes < 24) refuse("raster state outside a render encoder");
        const uint64_t *f = (const uint64_t *)body;
        [e.render setCullMode:(MTLCullMode)f[0]];
        [e.render setFrontFacingWinding:(MTLWinding)f[1]];
        [e.render setTriangleFillMode:(MTLTriangleFillMode)f[2]];
        break;
    }
    case QC_STENCIL_REFERENCE: {
        if (!e.render || bytes < 16) refuse("stencil reference outside a render encoder");
        const uint64_t *f = (const uint64_t *)body;
        [e.render setStencilFrontReferenceValue:(uint32_t)f[0] backReferenceValue:(uint32_t)f[1]];
        break;
    }
    case QC_BLEND_COLOR: {
        if (!e.render || bytes < 32) refuse("blend color outside a render encoder");
        const double *c = (const double *)body;
        [e.render setBlendColorRed:c[0] green:c[1] blue:c[2] alpha:c[3]];
        break;
    }
    case QC_DEPTH_BIAS: {
        if (!e.render || bytes < 24) refuse("depth bias outside a render encoder");
        const double *f = (const double *)body;
        [e.render setDepthBias:f[0] slopeScale:f[1] clamp:f[2]];
        break;
    }
    case QC_VISIBILITY: {
        if (!e.render || bytes < 16) refuse("visibility outside a render encoder");
        const uint64_t *f = (const uint64_t *)body;
        if (f[1] >= 65536) refuse("visibility offset past 64 KiB");
        [e.render setVisibilityResultMode:(MTLVisibilityResultMode)f[0] offset:f[1]];
        break;
    }
    case QC_DRAW: {
        const QCDraw &f = take<QCDraw>(body, bytes);
        if (!e.render) refuse("draw outside a render encoder");
        [e.render drawPrimitives:(MTLPrimitiveType)f.primitive vertexStart:f.start vertexCount:f.count instanceCount:std::max<uint64_t>(f.instances, 1)
                    baseInstance:f.baseInstance];
        break;
    }
    case QC_DRAW_INDEXED: {
        const QCDrawIndexed &f = take<QCDrawIndexed>(body, bytes);
        if (!e.render) refuse("draw outside a render encoder");
        [e.render drawIndexedPrimitives:(MTLPrimitiveType)f.primitive indexCount:f.count indexType:(MTLIndexType)f.indexType indexBuffer:gHeap
                      indexBufferOffset:heapOffset(f.indices, 0) instanceCount:std::max<uint64_t>(f.instances, 1) baseVertex:f.baseVertex
                           baseInstance:f.baseInstance];
        break;
    }
    case QC_COPY_BUFFER: {
        if (!e.blit || bytes < 24) refuse("copy outside a blit encoder");
        const uint64_t *f = (const uint64_t *)body;
        [e.blit copyFromBuffer:gHeap sourceOffset:heapOffset(f[0], f[2]) toBuffer:gHeap destinationOffset:heapOffset(f[1], f[2]) size:f[2]];
        break;
    }
    case QC_FILL_BUFFER: {
        if (!e.blit || bytes < 24) refuse("fill outside a blit encoder");
        const uint64_t *f = (const uint64_t *)body;
        [e.blit fillBuffer:gHeap range:NSMakeRange(heapOffset(f[0], f[1]), f[1]) value:(uint8_t)f[2]];
        break;
    }
    case QC_COPY_TEXTURE: {
        const QCCopyTexture &f = take<QCCopyTexture>(body, bytes);
        if (!e.blit) refuse("copy outside a blit encoder");
        [e.blit copyFromTexture:object<id<MTLTexture>>(f.source, "texture") sourceSlice:f.sourceSlice sourceLevel:f.sourceLevel
                   sourceOrigin:MTLOriginMake(f.sourceOrigin[0], f.sourceOrigin[1], f.sourceOrigin[2])
                     sourceSize:MTLSizeMake(f.size[0], f.size[1], f.size[2]) toTexture:object<id<MTLTexture>>(f.destination, "texture")
               destinationSlice:f.destinationSlice destinationLevel:f.destinationLevel
              destinationOrigin:MTLOriginMake(f.destinationOrigin[0], f.destinationOrigin[1], f.destinationOrigin[2])];
        break;
    }
    case QC_BUFFER_TO_TEXTURE: case QC_TEXTURE_TO_BUFFER: {
        const QCBufferTexture &f = take<QCBufferTexture>(body, bytes);
        if (!e.blit) refuse("copy outside a blit encoder");
        id<MTLTexture> texture = object<id<MTLTexture>>(f.texture, "texture");
        const MTLSize size = MTLSizeMake(f.size[0], f.size[1], f.size[2]);
        const MTLOrigin origin = MTLOriginMake(f.origin[0], f.origin[1], f.origin[2]);
        const uint64_t offset = heapOffset(f.buffer, 0);
        if (op == QC_BUFFER_TO_TEXTURE)
            [e.blit copyFromBuffer:gHeap sourceOffset:offset sourceBytesPerRow:f.bytesPerRow sourceBytesPerImage:f.bytesPerImage sourceSize:size
                         toTexture:texture destinationSlice:f.slice destinationLevel:f.level destinationOrigin:origin];
        else
            [e.blit copyFromTexture:texture sourceSlice:f.slice sourceLevel:f.level sourceOrigin:origin sourceSize:size toBuffer:gHeap
                  destinationOffset:offset destinationBytesPerRow:f.bytesPerRow destinationBytesPerImage:f.bytesPerImage];
        break;
    }
    case QC_GENERATE_MIPMAPS:
        if (!e.blit) refuse("mipmaps outside a blit encoder");
        [e.blit generateMipmapsForTexture:object<id<MTLTexture>>(take<uint64_t>(body, bytes), "texture")];
        break;
    case QC_IMAGEBLOCK: {
        if (!e.compute || bytes < 16) refuse("imageblock outside a compute encoder");
        const uint64_t *f = (const uint64_t *)body;
        [e.compute setImageblockWidth:f[0] height:f[1]];
        break;
    }
    default:
        refuse("unknown command " + std::to_string(op));
    }
}
static void commit(const QMCommit &c) {
    id<MTLCommandQueue> queue = object<id<MTLCommandQueue>>(c.queue, "queue");
    heapOffset(c.offset, c.length);
    Encoding e;
    e.commandBuffer = [queue commandBuffer];
    const uint64_t generation = gGeneration, number = c.commandBuffer;
    try {
        for (uint64_t at = 0; at < c.length;) {
            if (c.length - at < sizeof(QMCommandHeader)) refuse("truncated commands");
            QMCommandHeader header;
            memcpy(&header, gWindow + c.offset + at, sizeof header);
            if (header.bytes < sizeof header || header.bytes > c.length - at) refuse("bad command length");
            if (gVerbose) {
                const uint64_t *w = (const uint64_t *)(gWindow + c.offset + at + sizeof header);
                const unsigned words = (header.bytes - (uint32_t)sizeof header) / 8;
                note("  command %u (%u bytes) %llx %llx %llx", header.op, header.bytes, words > 0 ? w[0] : 0, words > 1 ? w[1] : 0, words > 2 ? w[2] : 0);
            }
            encode(e, header.op, gWindow + c.offset + at + sizeof header, header.bytes - (uint32_t)sizeof header);
            at += header.bytes;
        }
        if (e.current()) refuse("an encoder was left open");
    } catch (Failure &f) {
        if (e.current()) e.end();
        gCount.failures++;
        note("command buffer %llu refused: %s", (unsigned long long)number, f.why.c_str());
        writeCompletion(generation, number, 5, 0, 0, f.why.c_str());
        return;
    }
    auto copies = e.visibilityCopies;
    [e.commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        const bool ok = done.status == MTLCommandBufferStatusCompleted;
        if (ok)
            for (const auto &copy : copies) memcpy(gWindow + copy.second, copy.first.contents, 65536);
        if (!ok) note("command buffer %llu failed: %s", (unsigned long long)number, done.error.localizedDescription.UTF8String ?: "?");
        writeCompletion(generation, number, ok ? 4 : 5, done.GPUStartTime, done.GPUEndTime, ok ? nullptr : done.error.localizedDescription.UTF8String);
    }];
    [e.commandBuffer commit];
    gCount.commits++;
}

// ---- requests ----
static id<MTLLibrary> library(const unsigned char *bytes, uint64_t length) {
    uint8_t digest[32];
    CC_SHA256(bytes, (CC_LONG)length, digest);
    NSData *key = [NSData dataWithBytes:digest length:32];
    if (id<MTLLibrary> known = gLibraries[key]) return known;
    air_library_view view = {};
    const uint8_t *slice = bytes;
    size_t sliceBytes = length;
    auto hash = [](const uint8_t *p, size_t n, uint8_t out[32], void *) -> int {CC_SHA256(p, (CC_LONG)n, out); return 0;};
    // A universal iPadOS library: its AIR slice. Anything else (a Mac library, on the host instruments) goes to Metal as it is.
    if (!air_library_open(bytes, length, hash, nullptr, &view)) {slice = bytes + view.slice_offset; sliceBytes = view.slice_bytes;}
    dispatch_data_t data = dispatch_data_create(slice, sliceBytes, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    id<MTLLibrary> made = [gDevice newLibraryWithData:data error:&error];
    if (!made) refuse(error.localizedDescription.UTF8String ?: "library refused");
    made.label = [NSString stringWithFormat:@"%zu bytes", (size_t)length];
    gLibraries[key] = made;
    note("library %zu bytes: %lu functions", (size_t)length, (unsigned long)made.functionNames.count);
    return made;
}
static void handle(uint32_t op, const std::vector<unsigned char> &in, Out &out) {
    switch (op) {
    case QM_OPEN: {
        reset(fields<QMOpen>(in).session);
        QMDeviceInfo info = {};
        snprintf(info.name, sizeof info.name, "%s", gDevice.name.UTF8String);
        snprintf(info.vendor, sizeof info.vendor, "Apple");
        info.maxBufferLength = gDevice.maxBufferLength;
        const MTLSize threads = gDevice.maxThreadsPerThreadgroup;
        info.maxThreads[0] = threads.width; info.maxThreads[1] = threads.height; info.maxThreads[2] = threads.depth;
        info.heapAddress = gHeap.gpuAddress;
        out.append(&info, sizeof info);
        break;
    }
    case QM_RELEASE: {
        const uint64_t count = fields<uint64_t>(in);
        if (in.size() < (count + 1) * 8) refuse("short release");
        for (uint64_t i = 0; i < count; i++) gObjects.erase(((const uint64_t *)in.data())[1 + i]);
        break;
    }
    case QM_ANSWERS: {
        const uint64_t count = fields<uint64_t>(in);
        size_t at = 8;
        for (uint64_t i = 0; i < count; i++) {
            if (at >= in.size()) refuse("short answers");
            const char *name = (const char *)in.data() + at;
            at += strnlen(name, in.size() - at) + 1;
            QMAnswer answer = {0, 0};
            answer.known = answerSelector(name, false, 0, answer.value);
            out.append(&answer, sizeof answer);
        }
        break;
    }
    case QM_ANSWER: {
        QMAnswerRequest request = fields<QMAnswerRequest>(in);
        request.selector[sizeof request.selector - 1] = 0;
        uint64_t value = 0;
        if (!answerSelector(request.selector, true, request.argument, value)) throw Failure{"", QM_UNKNOWN};
        out.word(value);
        break;
    }
    case QM_TEXTURE: {
        id<MTLTexture> texture = [gDevice newTextureWithDescriptor:textureDescriptor(fields<QMTexture>(in).descriptor)];
        out.handle = keep(texture, "texture");
        if (gVerbose) note("  texture %llu: format %lu %lux%lu storage %lu usage %lu", out.handle, (unsigned long)texture.pixelFormat,
                           (unsigned long)texture.width, (unsigned long)texture.height, (unsigned long)texture.storageMode, (unsigned long)texture.usage);
        describe(texture, out);
        break;
    }
    case QM_TEXTURE_ON_BUFFER: {
        const QMTextureOnBuffer &f = fields<QMTextureOnBuffer>(in);
        MTLTextureDescriptor *d = textureDescriptor(f.descriptor);
        d.storageMode = MTLStorageModeShared;
        d.resourceOptions = (d.resourceOptions & ~MTLResourceStorageModeMask) | MTLResourceStorageModeShared;
        const uint64_t offset = heapOffset(f.offset, f.bytesPerRow * d.height);
        id<MTLTexture> texture = [gHeap newTextureWithDescriptor:d offset:offset bytesPerRow:f.bytesPerRow];
        out.handle = keep(texture, "texture over the window");
        if (gVerbose) note("  texture %llu over %llx: format %lu %lux%lu row %llu usage %lu", out.handle, f.offset, (unsigned long)texture.pixelFormat,
                           (unsigned long)texture.width, (unsigned long)texture.height, f.bytesPerRow, (unsigned long)texture.usage);
        describe(texture, out);
        break;
    }
    case QM_TEXTURE_VIEW: {
        const QMTextureView &f = fields<QMTextureView>(in);
        id<MTLTexture> parent = object<id<MTLTexture>>(f.texture, "texture");
        id<MTLTexture> view = f.whole ? [parent newTextureViewWithPixelFormat:(MTLPixelFormat)f.format]
            : [parent newTextureViewWithPixelFormat:(MTLPixelFormat)f.format textureType:(MTLTextureType)f.kind
                                             levels:NSMakeRange(f.levels[0], f.levels[1]) slices:NSMakeRange(f.slices[0], f.slices[1])];
        out.handle = keep(view, "texture view");
        describe(view, out);
        break;
    }
    case QM_TEXTURE_REPLACE: case QM_TEXTURE_GET_BYTES: {
        const QMTextureBytes &f = fields<QMTextureBytes>(in);
        id<MTLTexture> texture = object<id<MTLTexture>>(f.texture, "texture");
        const uint64_t offset = heapOffset(f.offset, f.length);
        const MTLOrigin origin = MTLOriginMake(f.region[0], f.region[1], f.region[2]);
        const MTLSize size = MTLSizeMake(f.region[3], f.region[4], f.region[5]);
        const uint64_t image = texture.textureType == MTLTextureType3D ? f.bytesPerImage : 0;
        id<MTLCommandBuffer> commandBuffer = [gUtility commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        if (op == QM_TEXTURE_REPLACE)
            [blit copyFromBuffer:gHeap sourceOffset:offset sourceBytesPerRow:f.bytesPerRow sourceBytesPerImage:image sourceSize:size
                       toTexture:texture destinationSlice:f.slice destinationLevel:f.level destinationOrigin:origin];
        else
            [blit copyFromTexture:texture sourceSlice:f.slice sourceLevel:f.level sourceOrigin:origin sourceSize:size toBuffer:gHeap
                destinationOffset:offset destinationBytesPerRow:f.bytesPerRow destinationBytesPerImage:image];
        [blit endEncoding];
        waitUtility(commandBuffer);
        if (gVerbose) {
            uint64_t nonzero = 0;
            for (uint64_t i = 0; i < f.length; i++) nonzero += gWindow[f.offset + i] != 0;
            note("  %s texture %llu: format %lu %lux%lu storage %lu usage %lu, region %llux%llu, %llu of %llu bytes nonzero",
                 op == QM_TEXTURE_REPLACE ? "replace" : "get bytes", f.texture, (unsigned long)texture.pixelFormat, (unsigned long)texture.width,
                 (unsigned long)texture.height, (unsigned long)texture.storageMode, (unsigned long)texture.usage, f.region[3], f.region[4], nonzero,
                 f.length);
        }
        break;
    }
    case QM_LIBRARY: {
        const QMLibrary &f = fields<QMLibrary>(in);
        id<MTLLibrary> made = library(gWindow + QM_HEAP + heapOffset(f.offset, f.length), f.length);
        out.handle = keep(made, "library");
        NSArray<NSString *> *names = made.functionNames;
        out.word(names.count);
        for (NSString *name in names) out.append(name.UTF8String, strlen(name.UTF8String) + 1);
        break;
    }
    case QM_STITCHED_LIBRARY: {
        // The guest's graphs, rebuilt from QemuMetalLibraries.mm's request, stitched from this Mac's functions.
        size_t at = 0;
        auto take = [&](size_t n) {
            if (at + n > in.size()) refuse("short stitching request");
            const unsigned char *p = in.data() + at;
            at = (at + n + 7) / 8 * 8;
            return p;
        };
        auto word = [&] {return *(const uint64_t *)take(8);};
        const QMStitched header = *(const QMStitched *)take(sizeof(QMStitched));
        NSMutableArray<id<MTLFunction>> *functions = [NSMutableArray array];
        for (uint64_t i = 0; i < header.functions; i++) [functions addObject:object<id<MTLFunction>>(word(), "stitchable function")];
        NSMutableArray<MTLFunctionStitchingGraph *> *graphs = [NSMutableArray array];
        for (uint64_t g = 0; g < header.graphs; g++) {
            const QMStitchGraph graph = *(const QMStitchGraph *)take(sizeof(QMStitchGraph));
            NSString *name = @((const char *)take(graph.nameBytes));
            if (graph.output >= graph.nodes) refuse("stitching output node out of range");
            NSMutableArray<MTLFunctionStitchingFunctionNode *> *nodes = [NSMutableArray array], *listed = [NSMutableArray array];
            std::vector<std::vector<uint64_t>> arguments, dependencies;
            for (uint64_t n = 0; n < graph.nodes; n++) {
                const QMStitchNode node = *(const QMStitchNode *)take(sizeof(QMStitchNode));
                MTLFunctionStitchingFunctionNode *made = [[MTLFunctionStitchingFunctionNode alloc] initWithName:@((const char *)take(node.nameBytes))
                                                                                                      arguments:@[] controlDependencies:@[]];
                arguments.emplace_back();
                for (uint64_t a = 0; a < node.arguments; a++) arguments.back().push_back(word());
                dependencies.emplace_back();
                for (uint64_t d = 0; d < node.dependencies; d++) dependencies.back().push_back(word());
                [nodes addObject:made];
                if (node.listed) [listed addObject:made];
            }
            for (uint64_t n = 0; n < graph.nodes; n++) {
                NSMutableArray *nodeArguments = [NSMutableArray array];
                for (uint64_t a : arguments[n]) {
                    if (a & QM_STITCH_INPUT) [nodeArguments addObject:[[MTLFunctionStitchingInputNode alloc] initWithArgumentIndex:a & ~QM_STITCH_INPUT]];
                    else if (a < graph.nodes) [nodeArguments addObject:nodes[a]];
                    else refuse("stitching argument out of range");
                }
                NSMutableArray *nodeDependencies = [NSMutableArray array];
                for (uint64_t d : dependencies[n]) {
                    if (d >= graph.nodes) refuse("stitching dependency out of range");
                    [nodeDependencies addObject:nodes[d]];
                }
                nodes[n].arguments = nodeArguments;
                nodes[n].controlDependencies = nodeDependencies;
            }
            NSArray *attributes = graph.inlined ? @[[MTLFunctionStitchingAttributeAlwaysInline new]] : @[];
            [graphs addObject:[[MTLFunctionStitchingGraph alloc] initWithFunctionName:name nodes:listed outputNode:nodes[graph.output]
                                                                           attributes:attributes]];
        }
        MTLStitchedLibraryDescriptor *descriptor = [MTLStitchedLibraryDescriptor new];
        descriptor.functions = functions;
        descriptor.functionGraphs = graphs;
        NSError *error = nil;
        id<MTLLibrary> made = [gDevice newLibraryWithStitchedDescriptor:descriptor error:&error];
        if (!made) refuse([NSString stringWithFormat:@"stitched library: %@", error.localizedDescription ?: @"refused"].UTF8String);
        note("stitched %s from %lu functions", graphs.firstObject.functionName.UTF8String ?: "?", (unsigned long)functions.count);
        out.handle = keep(made, "stitched library");
        NSArray<NSString *> *names = made.functionNames;
        out.word(names.count);
        for (NSString *n in names) out.append(n.UTF8String, strlen(n.UTF8String) + 1);
        break;
    }
    case QM_SOURCE_LIBRARY: {
        // Metal source the guest's Core Image wrote for a filter graph, compiled by this Mac's compiler.
        const QMSource &f = fields<QMSource>(in);
        NSString *source = [[NSString alloc] initWithBytes:gWindow + QM_HEAP + heapOffset(f.offset, f.length) length:f.length
                                                  encoding:NSUTF8StringEncoding];
        if (!source) refuse("source is not UTF-8");
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.mathMode = f.fastMath ? MTLMathModeFast : MTLMathModeSafe;
        if (f.languageVersion) options.languageVersion = (MTLLanguageVersion)f.languageVersion;
        NSMutableDictionary *macros = [NSMutableDictionary dictionary];
        size_t at = sizeof f;
        for (uint64_t i = 0; i < f.macros && at < in.size(); i++) {
            const char *text = (const char *)in.data() + at;
            at += strnlen(text, in.size() - at) + 1;
            const char *equals = strchr(text, '=');
            if (!equals) continue;
            macros[[[NSString alloc] initWithBytes:text length:(NSUInteger)(equals - text) encoding:NSUTF8StringEncoding]] = @(equals + 1);
        }
        if (macros.count) options.preprocessorMacros = macros;
        NSError *error = nil;
        id<MTLLibrary> made = [gDevice newLibraryWithSource:source options:options error:&error];
        if (!made) refuse([NSString stringWithFormat:@"source library: %@", error.localizedDescription ?: @"refused"].UTF8String);
        made.label = [NSString stringWithFormat:@"source of %llu bytes", f.length];
        note("compiled source of %llu bytes: %lu functions", f.length, (unsigned long)made.functionNames.count);
        out.handle = keep(made, "source library");
        NSArray<NSString *> *names = made.functionNames;
        out.word(names.count);
        for (NSString *n in names) out.append(n.UTF8String, strlen(n.UTF8String) + 1);
        break;
    }
    case QM_EXTERN_NAMES: {
        id<MTLLibrary> owner = object<id<MTLLibrary>>(fields<uint64_t>(in), "library");
        SEL selector = sel_registerName("externFunctionNames");
        if (![owner respondsToSelector:selector]) throw Failure{"", QM_UNKNOWN};
        NSArray<NSString *> *names = ((NSArray<NSString *> * (*)(id, SEL))objc_msgSend)(owner, selector);
        out.word(names.count);
        for (NSString *n in names) out.append(n.UTF8String, strlen(n.UTF8String) + 1);
        break;
    }
    case QM_FUNCTION: {
        const QMFunction &f = fields<QMFunction>(in);
        id<MTLLibrary> owner = object<id<MTLLibrary>>(f.library, "library");
        size_t at = sizeof f;
        auto take = [&](size_t n) {
            if (at + n > in.size()) refuse("short function request");
            const unsigned char *p = in.data() + at;
            at = (at + n + 7) / 8 * 8;
            return p;
        };
        NSString *name = [NSString stringWithUTF8String:(const char *)take(f.nameBytes)];
        if (gVerbose) note("  function %s in %s, %llu constants", name.UTF8String, owner.label.UTF8String ?: "?", f.constants);
        id<MTLFunction> function = nil;
        NSError *error = nil;
        NSMutableString *constants = [NSMutableString string];   // for a refusal's text
        if (!f.constants) function = [owner newFunctionWithName:name];
        else {
            MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
            for (uint64_t i = 0; i < f.constants; i++) {
                const QMConstant c = *(const QMConstant *)take(sizeof(QMConstant));
                const char *constantName = c.nameBytes ? (const char *)take(c.nameBytes) : nullptr;
                const void *value = take(c.valueBytes);
                if (constantName) [values setConstantValue:value type:(MTLDataType)c.type withName:@(constantName)];
                else [values setConstantValue:value type:(MTLDataType)c.type atIndex:c.index];
                [constants appendFormat:@" %s:%llu", constantName ?: [NSString stringWithFormat:@"#%llu", c.index].UTF8String, c.type];
            }
            function = [owner newFunctionWithName:name constantValues:values error:&error];
        }
        // Metal's own text for a name the library has not (QemuMetalLibraries.mm makes it Metal's error again: Core Image goes on to its
        // next library only on that one).
        if (!function && !error) {
            if (gVerbose) note("  not in %s", owner.label.UTF8String ?: "?");
            throw Failure{[NSString stringWithFormat:@"Function %@ was not found in the library", name].UTF8String, QM_NOT_FOUND};
        }
        if (!function)
            refuse([NSString stringWithFormat:@"function %@ (library of %lu functions%@%@): %@", name, (unsigned long)owner.functionNames.count,
                    owner.label ? @", " : @"", owner.label ?: @"", error.localizedDescription].UTF8String + std::string(constants.UTF8String));
        out.handle = keep(function, "function");
        out.word((uint64_t)function.functionType);
        break;
    }
    case QM_RENDER_PIPELINE: {
        const QMRenderPipeline &f = fields<QMRenderPipeline>(in);
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = object<id<MTLFunction>>(f.vertex, "vertex function");
        if (f.fragment) d.fragmentFunction = object<id<MTLFunction>>(f.fragment, "fragment function");
        for (unsigned i = 0; i < 8; i++) {
            const QMColorAttachment &c = f.colors[i];
            MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
            a.pixelFormat = (MTLPixelFormat)c.format;
            a.writeMask = (MTLColorWriteMask)c.writeMask;
            a.blendingEnabled = c.blending != 0;
            a.sourceRGBBlendFactor = (MTLBlendFactor)c.sourceRGB;
            a.destinationRGBBlendFactor = (MTLBlendFactor)c.destinationRGB;
            a.rgbBlendOperation = (MTLBlendOperation)c.rgbOperation;
            a.sourceAlphaBlendFactor = (MTLBlendFactor)c.sourceAlpha;
            a.destinationAlphaBlendFactor = (MTLBlendFactor)c.destinationAlpha;
            a.alphaBlendOperation = (MTLBlendOperation)c.alphaOperation;
        }
        d.depthAttachmentPixelFormat = (MTLPixelFormat)f.depthFormat;
        d.stencilAttachmentPixelFormat = (MTLPixelFormat)f.stencilFormat;
        if (f.samples) d.rasterSampleCount = f.samples;
        d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)f.topology;
        d.rasterizationEnabled = f.rasterization != 0;
        if (f.vertexInputs) {
            MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
            for (unsigned i = 0; i < 31; i++) {
                if (!f.attributes[i].format) continue;
                v.attributes[i].format = (MTLVertexFormat)f.attributes[i].format;
                v.attributes[i].offset = f.attributes[i].offset;
                v.attributes[i].bufferIndex = f.attributes[i].buffer;
                if (f.attributes[i].buffer < 31) {
                    const QMVertexLayout &l = f.layouts[f.attributes[i].buffer];
                    v.layouts[f.attributes[i].buffer].stride = l.stride;
                    v.layouts[f.attributes[i].buffer].stepFunction = (MTLVertexStepFunction)l.stepFunction;
                    v.layouts[f.attributes[i].buffer].stepRate = l.stepRate;
                }
            }
            d.vertexDescriptor = v;
        }
        NSError *error = nil;
        id<MTLRenderPipelineState> pipeline = [gDevice newRenderPipelineStateWithDescriptor:d error:&error];
        if (!pipeline) refuse([NSString stringWithFormat:@"render pipeline %@/%@: %@", d.vertexFunction.name, d.fragmentFunction.name,
                                                         error.localizedDescription].UTF8String);
        out.handle = keep(pipeline, "render pipeline");
        break;
    }
    case QM_COMPUTE_PIPELINE: {
        const QMComputePipeline &f = fields<QMComputePipeline>(in);
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = object<id<MTLFunction>>(f.function, "kernel function");
        if (f.maxThreads) d.maxTotalThreadsPerThreadgroup = f.maxThreads;
        d.threadGroupSizeIsMultipleOfThreadExecutionWidth = f.multipleOfExecutionWidth != 0;
        if (f.maxCallStackDepth) d.maxCallStackDepth = f.maxCallStackDepth;
        for (unsigned i = 0; i < 31; i++) if (f.mutability[i]) d.buffers[i].mutability = (MTLMutability)f.mutability[i];
        NSError *error = nil;
        if (gVerbose) note("  compute pipeline %s", d.computeFunction.name.UTF8String);
        id<MTLComputePipelineState> pipeline = [gDevice newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
        if (!pipeline) refuse([NSString stringWithFormat:@"compute pipeline %@: %@", d.computeFunction.name, error.localizedDescription].UTF8String);
        out.handle = keep(pipeline, "compute pipeline");
        out.word(pipeline.maxTotalThreadsPerThreadgroup);
        out.word(pipeline.threadExecutionWidth);
        break;
    }
    case QM_SAMPLER: {
        const QMSampler &f = fields<QMSampler>(in);
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        d.minFilter = (MTLSamplerMinMagFilter)f.minFilter;
        d.magFilter = (MTLSamplerMinMagFilter)f.magFilter;
        d.mipFilter = (MTLSamplerMipFilter)f.mipFilter;
        d.sAddressMode = (MTLSamplerAddressMode)f.sAddress;
        d.tAddressMode = (MTLSamplerAddressMode)f.tAddress;
        d.rAddressMode = (MTLSamplerAddressMode)f.rAddress;
        d.borderColor = (MTLSamplerBorderColor)f.borderColor;
        d.normalizedCoordinates = f.normalized != 0;
        d.maxAnisotropy = std::max<uint64_t>(f.maxAnisotropy, 1);
        d.compareFunction = (MTLCompareFunction)f.compare;
        d.lodMinClamp = (float)f.lodMin;
        d.lodMaxClamp = (float)f.lodMax;
        out.handle = keep([gDevice newSamplerStateWithDescriptor:d], "sampler");
        break;
    }
    case QM_DEPTH_STENCIL: {
        const QMDepthStencil &f = fields<QMDepthStencil>(in);
        MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = (MTLCompareFunction)f.compare;
        d.depthWriteEnabled = f.write != 0;
        auto stencil = [](const QMStencil &s) {
            MTLStencilDescriptor *out = [MTLStencilDescriptor new];
            out.stencilCompareFunction = (MTLCompareFunction)s.compare;
            out.stencilFailureOperation = (MTLStencilOperation)s.failure;
            out.depthFailureOperation = (MTLStencilOperation)s.depthFailure;
            out.depthStencilPassOperation = (MTLStencilOperation)s.pass;
            out.readMask = (uint32_t)s.readMask;
            out.writeMask = (uint32_t)s.writeMask;
            return out;
        };
        d.frontFaceStencil = stencil(f.front);
        d.backFaceStencil = stencil(f.back);
        out.handle = keep([gDevice newDepthStencilStateWithDescriptor:d], "depth stencil state");
        break;
    }
    case QM_QUEUE:
        out.handle = keep([gDevice newCommandQueueWithMaxCommandBufferCount:256], "queue");
        break;
    case QM_EVENT:
        out.handle = keep(fields<uint64_t>(in) ? (id)[gDevice newSharedEvent] : (id)[gDevice newEvent], "event");
        break;
    case QM_EVENT_VALUE: {
        id event = object<id>(fields<uint64_t>(in), "event");
        if (![event conformsToProtocol:@protocol(MTLSharedEvent)]) refuse("not a shared event");
        out.word([(id<MTLSharedEvent>)event signaledValue]);
        break;
    }
    case QM_EVENT_SET_VALUE: {
        const QMHandleValue &f = fields<QMHandleValue>(in);
        id event = object<id>(f.handle, "event");
        if (![event conformsToProtocol:@protocol(MTLSharedEvent)]) refuse("not a shared event");
        [(id<MTLSharedEvent>)event setSignaledValue:f.value];
        break;
    }
    case QM_FENCE:
        out.handle = keep([gDevice newFence], "fence");
        break;
    case QM_COMMIT:
        commit(fields<QMCommit>(in));
        break;
    case QM_LINEAR_BYTES_PER_ROW: {
        const QMHandleValue &f = fields<QMHandleValue>(in);
        const uint64_t texel = texelBytes(f.handle);
        if (!texel) refuse("no linear layout for pixel format " + std::to_string(f.handle));
        const uint64_t alignment = [gDevice minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)f.handle];
        out.word((f.value * texel + alignment - 1) / alignment * alignment);
        break;
    }
    default:
        refuse("unknown request " + std::to_string(op));
    }
}

// The guest's log lines (QemuMetalRemote.cpp qemu_metal_log), into this log.
static void drainLog() {
    static std::string partial;
    const uint64_t written = load(&control()->logWritten);
    uint64_t read = control()->logRead;
    if (written == read) return;
    if (written - read > QM_LOG_BYTES) read = written - QM_LOG_BYTES;
    for (; read < written; read++) {
        const char c = (char)gWindow[QM_LOG + read % QM_LOG_BYTES];
        if (c != '\n') {partial += c; continue;}
        note("guest: %s", partial.c_str());
        partial.clear();
    }
    store(&control()->logRead, read);
}

// The request's refusal is caught here, in the innermost frame: a C++ exception unwinding through an Objective-C @try frame was not
// caught at all (the host aborted on an unknown device query).
static Failure attempt(uint32_t op, const std::vector<unsigned char> &in, Out &out) {
    try {
        handle(op, in, out);
        return Failure{"", 0};
    } catch (Failure &f) {
        return f;
    }
}

static void serve(uint64_t number) {
    QMRequestHeader header;
    memcpy(&header, gWindow + QM_REQUEST, sizeof header);
    std::vector<unsigned char> in;
    if (header.bytes <= QM_REQUEST_BYTES - sizeof header) in.assign(gWindow + QM_REQUEST + sizeof header, gWindow + QM_REQUEST + sizeof header + header.bytes);
    Out out;
    Failure result;
    @autoreleasepool {
        @try {
            result = attempt(header.op, in, out);
        } @catch (NSException *exception) {
            result = Failure{std::string("exception: ") + (exception.reason.UTF8String ?: "?")};
        }
    }
    int32_t status = result.status;
    if (status) {
        out = Out();
        out.append(result.why.data(), result.why.size());
        if (status == QM_FAILED) {
            gCount.failures++;
            note("request %u refused: %s", header.op, result.why.c_str());
        }
    }
    gCount.requests++;
    if (gVerbose) note("request %u (%u bytes): status %d, handle %llu, %zu bytes", header.op, header.bytes, status,
                       (unsigned long long)out.handle, out.data.size());
    if (out.data.size() > QM_REPLY_BYTES - sizeof(QMReplyHeader)) {
        status = QM_FAILED;
        out = Out();
        const char why[] = "reply too large";
        out.append(why, sizeof why - 1);
    }
    const QMReplyHeader reply = {status, (uint32_t)out.data.size(), out.handle};
    memcpy(gWindow + QM_REPLY, &reply, sizeof reply);
    if (!out.data.empty()) memcpy(gWindow + QM_REPLY + sizeof reply, out.data.data(), out.data.size());
    store(&control()->reply, number);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        const char *path = nullptr, *logPath = nullptr;
        unsigned slot = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--verbose")) gVerbose = true;
            else if (i + 1 < argc && !strcmp(argv[i], "--window")) path = argv[++i];
            else if (i + 1 < argc && !strcmp(argv[i], "--log")) logPath = argv[++i];
            else if (i + 1 < argc && !strcmp(argv[i], "--slot")) slot = (unsigned)atoi(argv[++i]);
        }
        gLog = logPath ? fopen(logPath, "a") : stderr;
        if (!path || !gLog || slot >= QM_SLOTS) {
            fprintf(stderr, "usage: qemu-metal-host --window RUN_DIR/shared.bin [--slot N] [--log FILE]\n");
            return 2;
        }
        gSlot = slot;
        int file = open(path, O_RDWR);
        struct stat info = {};
        if (file < 0 || fstat(file, &info)) {note("no window at %s", path); return 1;}
        // This process serves one slot (QemuMetalWire.h): one guest process's device. run-qemu.py --metal runs one for each.
        uint64_t offset = 0;
        qm_slot((uint64_t)info.st_size, slot, &offset, &gWindowBytes);
        if (gWindowBytes < QM_HEAP + (64ull << 20)) {note("slot %u of %s is too small", slot, path); return 1;}
        gWindow = (unsigned char *)mmap(nullptr, gWindowBytes, PROT_READ | PROT_WRITE, MAP_SHARED, file, (off_t)offset);
        close(file);
        if (gWindow == MAP_FAILED) {note("mmap: %s", strerror(errno)); return 1;}
        gDevice = MTLCreateSystemDefaultDevice();
        gHeap = [gDevice newBufferWithBytesNoCopy:gWindow + QM_HEAP length:gWindowBytes - QM_HEAP options:MTLResourceStorageModeShared deallocator:nil];
        gUtility = [gDevice newCommandQueue];
        gLibraries = [NSMutableDictionary dictionary];
        if (!gDevice || !gHeap || !gUtility) {note("no Metal device or heap buffer over the window"); return 1;}
        note("serving %s at %llu MiB: %llu MiB, %s, heap at GPU address %llx", path, (unsigned long long)(offset >> 20),
             (unsigned long long)(gWindowBytes >> 20), gDevice.name.UTF8String, (unsigned long long)gHeap.gpuAddress);
        uint64_t answered = load(&control()->reply), lastReport = 0;
        for (unsigned idle = 0;;) {
            const uint64_t request = load(&control()->request);
            if (request != answered && load(&control()->reply) != request) {
                serve(request);
                answered = request;
                idle = 0;
                continue;
            }
            store(&control()->hostBeat, control()->hostBeat + 1);
            if ((idle & 1023) == 0) drainLog();
            // Spin while the guest is busy, then poll ever more slowly.
            if (idle < 20000) {idle++; __asm__ volatile("yield");}
            else usleep(idle++ < 40000 ? 20 : 500);
            const uint64_t now = (uint64_t)[NSDate timeIntervalSinceReferenceDate];
            if (now - lastReport >= 10 && gCount.requests) {
                lastReport = now;
                note("%llu requests, %llu commits, %llu completions, %llu refusals, %zu objects", (unsigned long long)gCount.requests,
                     (unsigned long long)gCount.commits, (unsigned long long)gCount.completions, (unsigned long long)gCount.failures,
                     gObjects.size());
            }
        }
    }
}
