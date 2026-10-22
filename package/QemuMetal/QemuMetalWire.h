// SPDX-License-Identifier: BSD-2-Clause-Patent
// The wire between a guest process's Metal device (QemuMetalRemote.cpp, under NativeMetal's frontend) and qemu-metal-host on the Mac
// (QemuMetalHost.mm): the memory window of run-qemu.py --shared-window, which the guest process maps through the kernel and the host maps
// from RUN_DIR/shared.bin. Nothing is copied between them: a request, its reply, a command buffer's commands, the contents of every buffer
// and the shadows of guest memory (IOSurfaces) all lie in the window, and the host reaches them at the same offsets.
//
// One request at a time: the guest writes a request into the request area and then its number into QMControl.request; the host answers
// into the reply area and then writes the same number into QMControl.reply. A committed command buffer is a request too (QM_COMMIT): the
// host encodes and commits it before it answers, and writes a QMCompletion into the completion ring once the Mac's GPU has finished it.
// Both sides are arm64 little endian; every structure is made of 64-bit fields (doubles where Metal has them) so the layouts agree.
#pragma once
#include <stdint.h>

#define QM_MAGIC 0x4c544d51u                 // "QMTL"
#define QM_VERSION 1u

// The window is cut into slots, one per process with a device: slot 0, the render server's, is half of it, and eight more share the other
// half (128 MiB each in a 2 GiB window). The kernel maps a process its own slot only (QemuMetalAccelerator.cpp); the host serves each.
// Every offset below is within a slot.
#define QM_SLOTS 9u
#define QM_SLOT_ALIGNMENT 0x4000ull
static inline void qm_slot(uint64_t window, unsigned slot, uint64_t *offset, uint64_t *bytes) {
    const uint64_t half = window / 2 / QM_SLOT_ALIGNMENT * QM_SLOT_ALIGNMENT;
    const uint64_t each = (window - half) / (QM_SLOTS - 1) / QM_SLOT_ALIGNMENT * QM_SLOT_ALIGNMENT;
    *offset = slot ? half + (slot - 1) * each : 0;
    *bytes = slot ? each : half;
}
// A slot: the kernel's mailbox (SharedWindow.cpp) keeps the first 64 KiB of slot 0.
#define QM_CONTROL 0x10000ull
#define QM_REQUEST 0x20000ull
#define QM_REQUEST_BYTES 0x70000ull
#define QM_REPLY 0x90000ull
#define QM_REPLY_BYTES 0x70000ull
#define QM_COMPLETIONS 0x100000ull
#define QM_COMPLETION_SLOTS 4096u
#define QM_LOG 0x1f0000ull                   // the guest's log lines, a ring the host copies into its own log
#define QM_LOG_BYTES 0x10000ull
#define QM_HEAP 0x200000ull                  // to the window's end: buffers, shadows and staging, allocated by the guest
#define QM_HEAP_ALIGNMENT 256ull             // every allocation (a linear texture's offset needs 16 on Apple GPUs)
#define QM_NONE (~0ull)

struct QMControl {
    uint32_t magic, version;                 // guest, once the window is mapped
    uint64_t session;                        // guest: a new number per opening process; the host then drops the old session's objects
    uint64_t request;                        // guest: the number of the request in the request area
    uint64_t reply;                          // host: the number of the request answered in the reply area
    uint64_t hostBeat;                       // host: counts up while the host serves
    uint64_t completionsWritten;             // host: QMCompletion records written into the ring
    uint64_t completionsRead;                // guest: records it has taken
    uint64_t windowBytes;                    // guest: the window's size
    uint64_t logWritten;                     // guest: bytes written into the log ring
    uint64_t logRead;                        // host: bytes it has copied out
};
struct QMRequestHeader {uint32_t op, bytes;};                        // then bytes of the op's payload
struct QMReplyHeader {int32_t status; uint32_t bytes; uint64_t handle;}; // status 0, or a failure whose payload is its reason (text)
struct QMCompletion {
    uint64_t commandBuffer;                  // the guest's number for it (QMCommit.commandBuffer)
    uint64_t status;                         // MTLCommandBufferStatus: 4 completed, 5 error
    double gpuStart, gpuEnd;
    char error[96];
};

enum QMOp {
    QM_OPEN = 1,                 // QMOpen -> QMDeviceInfo
    QM_RELEASE,                  // uint64_t count, handles[count]: objects the guest no longer holds
    QM_ANSWERS,                  // uint64_t count, then count NUL-terminated selectors -> count QMAnswer
    QM_ANSWER,                   // QMAnswerRequest -> uint64_t value; status QM_UNKNOWN when the Mac's device has no such query
    QM_TEXTURE,                  // QMTexture -> handle, QMTextureDescriptor (as made)
    QM_TEXTURE_ON_BUFFER,        // QMTextureOnBuffer -> handle, QMTextureDescriptor
    QM_TEXTURE_VIEW,             // QMTextureView -> handle, QMTextureDescriptor
    QM_TEXTURE_PURGEABLE,        // QMHandleValue -> uint64_t state
    QM_TEXTURE_REPLACE,          // QMTextureBytes: from the staging bytes into the texture
    QM_TEXTURE_GET_BYTES,        // QMTextureBytes: from the texture into the staging bytes
    QM_LIBRARY,                  // QMLibrary -> handle, uint64_t count, then count NUL-terminated function names
    QM_FUNCTION,                 // QMFunction (+ name, constants) -> handle, uint64_t type
    QM_RENDER_PIPELINE,          // QMRenderPipeline -> handle
    QM_COMPUTE_PIPELINE,         // QMComputePipeline -> handle, uint64_t maxThreads, executionWidth
    QM_SAMPLER,                  // QMSampler -> handle
    QM_DEPTH_STENCIL,            // QMDepthStencil -> handle
    QM_QUEUE,                    // nothing -> handle
    QM_EVENT,                    // uint64_t shared -> handle
    QM_EVENT_VALUE,              // uint64_t handle -> uint64_t value
    QM_EVENT_SET_VALUE,          // QMHandleValue
    QM_FENCE,                    // nothing -> handle
    QM_COMMIT,                   // QMCommit
    QM_LINEAR_BYTES_PER_ROW,     // QMHandleValue {format, width} -> uint64_t
    QM_STITCHED_LIBRARY,         // QMStitched, its function handles, its graphs -> handle, uint64_t count, then the names (QM_LIBRARY's)
    QM_EXTERN_NAMES,             // uint64_t library -> uint64_t count, then count NUL-terminated names (externFunctionNames)
    QM_SOURCE_LIBRARY,           // QMSource, then its macros -> handle, uint64_t count, then the names (QM_LIBRARY's)
};
enum {QM_FAILED = -1, QM_UNKNOWN = -2, QM_NOT_FOUND = -3};   // QM_NOT_FOUND: a function name the library has not (its text is Metal's)

struct QMOpen {uint64_t session, windowBytes;};
struct QMDeviceInfo {
    char name[64], vendor[32];
    uint64_t maxBufferLength, maxThreads[3];
    uint64_t heapAddress;                    // the GPU address of the window's heap on the Mac (buffers' gpuAddress)
};
struct QMAnswer {uint64_t known, value;};
struct QMAnswerRequest {char selector[96]; uint64_t argument;};
struct QMHandleValue {uint64_t handle, value;};
struct QMTextureDescriptor {uint64_t type, format, width, height, depth, levels, samples, arrayLength, usage, options;};
struct QMTexture {QMTextureDescriptor descriptor;};
// A linear texture over window bytes: a buffer (offset is the window's) or the shadow of an IOSurface.
struct QMTextureOnBuffer {QMTextureDescriptor descriptor; uint64_t offset, bytesPerRow, bytesPerImage, surface;};
struct QMTextureView {uint64_t texture, format, kind, levels[2], slices[2], whole;};
// The texture region (x, y, z, width, height, depth), its level and slice, and the window's staging bytes in rows.
struct QMTextureBytes {uint64_t texture, region[6], level, slice, offset, bytesPerRow, bytesPerImage, length;};
struct QMLibrary {uint64_t offset, length;};
// Then the name (nameBytes, NUL included), then each constant: QMConstant, its name (nameBytes), its value (valueBytes), each padded to 8.
struct QMFunction {uint64_t library, nameBytes, constants;};
struct QMConstant {uint64_t type, index, nameBytes, valueBytes;};
struct QMColorAttachment {uint64_t format, writeMask, blending, sourceRGB, destinationRGB, rgbOperation, sourceAlpha, destinationAlpha, alphaOperation;};
struct QMVertexAttribute {uint64_t format, offset, buffer;};
struct QMVertexLayout {uint64_t stride, stepFunction, stepRate;};
struct QMRenderPipeline {
    uint64_t vertex, fragment;               // function handles; fragment 0 for none
    QMColorAttachment colors[8];
    uint64_t depthFormat, stencilFormat, samples, topology, rasterization, vertexInputs;
    QMVertexAttribute attributes[31];
    QMVertexLayout layouts[31];
};
struct QMComputePipeline {uint64_t function, maxThreads, multipleOfExecutionWidth, maxCallStackDepth, mutability[31];};
struct QMSampler {
    uint64_t minFilter, magFilter, mipFilter, sAddress, tAddress, rAddress, borderColor, normalized, maxAnisotropy, compare, lodAverage;
    double lodMin, lodMax;
};
struct QMStencil {uint64_t compare, failure, depthFailure, pass, readMask, writeMask;};
struct QMDepthStencil {uint64_t compare, write; QMStencil front, back;};
// The command buffer's commands (QMCommand records) are the window's bytes [offset, offset + length).
struct QMCommit {uint64_t queue, commandBuffer, offset, length;};
// A stitched library (MTLStitchedLibraryDescriptor): the function handles, then each graph, a QMStitchGraph, its name (padded to 8), then
// its nodes, each a QMStitchNode, its name (padded), its arguments and its control dependencies (uint64_t each). An argument is a node's
// index, or QM_STITCH_INPUT | the graph argument's index. Nodes not in the graph's own list (listed 0) are only reached through others.
// A library compiled from Metal source (newLibraryWithSource:options:error:): the source is the window's bytes [offset, offset + length);
// the preprocessor macros follow as NUL-terminated "NAME=VALUE" texts (macros of them). languageVersion 0 leaves the compiler's.
struct QMSource {uint64_t offset, length, fastMath, languageVersion, macros;};
struct QMStitched {uint64_t functions, graphs;};
struct QMStitchGraph {uint64_t nameBytes, nodes, output, inlined;};
struct QMStitchNode {uint64_t nameBytes, arguments, dependencies, listed;};
#define QM_STITCH_INPUT (1ull << 63)

// ---- commands: QMCommandHeader, then the command's fields; bytes counts the header and is a multiple of 8 ----
struct QMCommandHeader {uint32_t op, bytes;};
enum QMCommand {
    QC_SIGNAL_EVENT = 1,         // QCEvent
    QC_WAIT_EVENT,               // QCEvent
    QC_RENDER,                   // QCRenderPass: a render encoder
    QC_COMPUTE,                  // nothing: a compute encoder
    QC_BLIT,                     // nothing: a blit encoder
    QC_END,                      // nothing: ends the encoder
    QC_UPDATE_FENCE,             // QCFence
    QC_WAIT_FENCE,               // QCFence
    QC_BUFFER,                   // QCBind: value is the window offset of the bound bytes
    QC_BYTES,                    // QCBind (value: length), then the bytes padded to 8
    QC_TEXTURE,                  // QCBind: value is the texture handle (0: none)
    QC_SAMPLER,                  // QCBind: value is the sampler handle (0: none)
    QC_COMPUTE_PIPELINE,         // uint64_t pipeline
    QC_THREADGROUP_MEMORY,       // uint64_t length, index
    QC_DISPATCH,                 // QCDispatch
    QC_RENDER_PIPELINE,          // uint64_t pipeline
    QC_DEPTH_STENCIL,            // uint64_t state (0: none)
    QC_VIEWPORT,                 // double[6]
    QC_SCISSOR,                  // uint64_t[4]
    QC_RASTER,                   // uint64_t cull, winding, fill
    QC_STENCIL_REFERENCE,        // uint64_t front, back
    QC_BLEND_COLOR,              // double[4]
    QC_DEPTH_BIAS,               // double bias, slope, clamp
    QC_VISIBILITY,               // uint64_t mode, offset
    QC_DRAW,                     // QCDraw
    QC_DRAW_INDEXED,             // QCDrawIndexed
    QC_COPY_BUFFER,              // uint64_t source, destination (window offsets), size
    QC_FILL_BUFFER,              // uint64_t offset (window), length, value
    QC_COPY_TEXTURE,             // QCCopyTexture
    QC_BUFFER_TO_TEXTURE,        // QCBufferTexture
    QC_TEXTURE_TO_BUFFER,        // QCBufferTexture
    QC_GENERATE_MIPMAPS,         // uint64_t texture
    QC_IMAGEBLOCK,               // uint64_t width, height: a compute encoder's imageblock (QemuMetalRemote.cpp adds the selector)
};
struct QCEvent {uint64_t event, value;};
struct QCFence {uint64_t fence, stages;};
struct QCBind {uint64_t stage, index, value;};      // stage: 0 compute, 1 vertex, 2 fragment
struct QCDispatch {uint64_t size[3], perGroup[3], threads;};
struct QCDraw {uint64_t primitive, start, count, instances, baseInstance;};
struct QCDrawIndexed {uint64_t primitive, count, indexType, indices, instances; int64_t baseVertex; uint64_t baseInstance;};
struct QCAttachment {uint64_t texture, level, slice, depthPlane, load, store; double clear[4];};
struct QCRenderPass {QCAttachment colors[8], depth, stencil; uint64_t width, height, arrayLength, samples, visibility;};
struct QCCopyTexture {uint64_t source, sourceSlice, sourceLevel, sourceOrigin[3], size[3], destination, destinationSlice, destinationLevel,
                      destinationOrigin[3];};
struct QCBufferTexture {uint64_t buffer, bytesPerRow, bytesPerImage, size[3], texture, slice, level, origin[3];};
