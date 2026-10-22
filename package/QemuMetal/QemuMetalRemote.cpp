// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeMetal's bridge ABI (bridge/NativeMetalObjects.h) over the Mac's GPU: the objects NativeBridgeMetalFrontend.mm asks for are made by
// qemu-metal-host on the Mac (QemuMetalHost.mm), through the memory window both sides map (QemuMetalWire.h). It replaces the metal2vulkan
// runtime of sheng's plugin; the frontend above it is unchanged.
//
// Buffers are window bytes, so the guest's CPU and the Mac's GPU use the same memory. Memory the window does not hold (an IOSurface's
// pages, or a caller's bytes given to newBufferWithBytesNoCopy:) has a shadow in the window instead: a command buffer copies the shadows it
// uses in before it is committed, and the ones its GPU work may have written back out once the Mac reports it complete, before its
// completion handler runs. Every other object lives on the Mac and is named here by its handle. Commands are recorded per command buffer
// and go to the Mac whole at commit; the Mac encodes and commits them before it answers, and a thread here takes its completions.
#include "NativeMetalObjects.h"
#include "QemuMetalWire.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <set>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <TargetConditionals.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" void qemu_metal_set_window(void *base, uint64_t bytes);
extern "C" void qemu_metal_complete_frontend(void *device);

namespace {
thread_local std::string tError;
bool fail(const std::string &why) {tError = why; return false;}
template <class T> T *refuse(const std::string &why) {tError = why; return nullptr;}
uint64_t align(uint64_t value, uint64_t to) {return (value + to - 1) / to * to;}
uint64_t nowMs() {return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;}
void pause(unsigned round) {
    // The guest may have one CPU: spin only briefly, then let the scheduler run something else while the Mac works.
    if (round < 64) __asm__ volatile("yield");
    else usleep(round < 2000 ? 20 : 200);
}

unsigned char *gWindow;
uint64_t gWindowBytes;
QMControl *control() {return reinterpret_cast<QMControl *>(gWindow + QM_CONTROL);}
uint64_t load(const uint64_t *word) {return __atomic_load_n(word, __ATOMIC_ACQUIRE);}
void store(uint64_t *word, uint64_t value) {__atomic_store_n(word, value, __ATOMIC_RELEASE);}

// ---- the window's heap: first fit over free blocks, kept by offset ----
std::mutex gHeapLock;
std::map<uint64_t, uint64_t> gFree;
uint64_t gHeapUsed;
uint64_t allocate(uint64_t bytes) {
    bytes = align(bytes ? bytes : 1, QM_HEAP_ALIGNMENT);
    std::lock_guard<std::mutex> lock(gHeapLock);
    for (auto it = gFree.begin(); it != gFree.end(); ++it) {
        if (it->second < bytes) continue;
        const uint64_t offset = it->first, left = it->second - bytes;
        gFree.erase(it);
        if (left) gFree[offset + bytes] = left;
        gHeapUsed += bytes;
        return offset;
    }
    return 0;
}
void deallocate(uint64_t offset, uint64_t bytes) {
    if (!offset) return;
    bytes = align(bytes ? bytes : 1, QM_HEAP_ALIGNMENT);
    std::lock_guard<std::mutex> lock(gHeapLock);
    gHeapUsed -= bytes;
    auto next = gFree.lower_bound(offset);
    if (next != gFree.end() && offset + bytes == next->first) {bytes += next->second; next = gFree.erase(next);}
    if (next != gFree.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == offset) {previous->second += bytes; return;}
    }
    gFree[offset] = bytes;
}
// Window bytes a command buffer in flight may still use are given back once every command buffer committed before the release is done.
std::mutex gFlightLock;
std::set<uint64_t> gInFlight;
uint64_t gLastCommitted;
struct Deferred {uint64_t offset, bytes, after;};
std::vector<Deferred> gDeferred;
void deallocateLater(uint64_t offset, uint64_t bytes) {
    {
        std::lock_guard<std::mutex> lock(gFlightLock);
        if (!gInFlight.empty() && *gInFlight.begin() <= gLastCommitted) {gDeferred.push_back({offset, bytes, gLastCommitted}); return;}
    }
    deallocate(offset, bytes);
}
void collectDeferred() {
    std::vector<Deferred> ready;
    {
        std::lock_guard<std::mutex> lock(gFlightLock);
        const uint64_t oldest = gInFlight.empty() ? UINT64_MAX : *gInFlight.begin();
        for (size_t i = 0; i < gDeferred.size();) {
            if (gDeferred[i].after < oldest) {ready.push_back(gDeferred[i]); gDeferred[i] = gDeferred.back(); gDeferred.pop_back();}
            else i++;
        }
    }
    for (const Deferred &d : ready) deallocate(d.offset, d.bytes);
}

// ---- requests ----
std::mutex gCallLock;
uint64_t gRequestNumber;
std::mutex gReleaseLock;
std::vector<uint64_t> gReleases;
struct Reply {int32_t status = QM_FAILED; uint64_t handle = 0; std::vector<unsigned char> data;};
bool sendLocked(uint32_t op, const void *payload, size_t bytes, Reply &reply) {
    if (!gWindow) return fail("no window to the Mac (qemu-metal-host)");
    if (bytes + sizeof(QMRequestHeader) > QM_REQUEST_BYTES) return fail("request too large");
    const QMRequestHeader header = {op, (uint32_t)bytes};
    memcpy(gWindow + QM_REQUEST, &header, sizeof header);
    if (bytes) memcpy(gWindow + QM_REQUEST + sizeof header, payload, bytes);
    const uint64_t number = ++gRequestNumber;
    store(&control()->request, number);
    const uint64_t began = nowMs();
    for (unsigned round = 0; load(&control()->reply) != number; round++) {
        pause(round);
        if ((round & 255) == 255 && nowMs() - began > 60000) return fail("the Mac (qemu-metal-host) does not answer");
    }
    QMReplyHeader answer;
    memcpy(&answer, gWindow + QM_REPLY, sizeof answer);
    if (answer.bytes > QM_REPLY_BYTES - sizeof answer) return fail("reply too large");
    reply.status = answer.status;
    reply.handle = answer.handle;
    reply.data.assign(gWindow + QM_REPLY + sizeof answer, gWindow + QM_REPLY + sizeof answer + answer.bytes);
    if (answer.status == 0) return true;
    return fail(answer.status == QM_UNKNOWN ? "unknown to the Mac's device" : std::string(reply.data.begin(), reply.data.end()));
}
void flushReleasesLocked() {
    std::vector<uint64_t> handles;
    {
        std::lock_guard<std::mutex> lock(gReleaseLock);
        handles.swap(gReleases);
    }
    if (handles.empty()) return;
    std::vector<uint64_t> payload;
    payload.push_back(handles.size());
    payload.insert(payload.end(), handles.begin(), handles.end());
    Reply reply;
    sendLocked(QM_RELEASE, payload.data(), payload.size() * 8, reply);
}
bool call(uint32_t op, const void *payload, size_t bytes, Reply &reply) {
    std::lock_guard<std::mutex> lock(gCallLock);
    if (gReleases.size() >= 256) flushReleasesLocked();
    return sendLocked(op, payload, bytes, reply);
}
template <class T> bool call(uint32_t op, const T &payload, Reply &reply) {return call(op, &payload, sizeof payload, reply);}
void releaseLater(uint64_t handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lock(gReleaseLock);
    gReleases.push_back(handle);
}
uint64_t replyWord(const Reply &reply, size_t index) {
    uint64_t value = 0;
    if (reply.data.size() >= (index + 1) * 8) memcpy(&value, reply.data.data() + index * 8, 8);
    return value;
}
// Bytes for one request, in the window for its duration.
struct Staging {
    uint64_t offset = 0, bytes = 0;
    explicit Staging(uint64_t length) : offset(allocate(length)), bytes(length) {}
    ~Staging() {deallocate(offset, bytes);}
    unsigned char *data() const {return gWindow + offset;}
};
}  // namespace

// ---- objects ----
struct NmbDevice {
    QMDeviceInfo info;
    std::vector<std::pair<std::string, uint32_t>> selectors;
    std::unordered_map<std::string, uint64_t> answers;
    std::mutex answerLock;
    std::map<std::pair<std::string, uint64_t>, uint64_t> argumentAnswers;
};
struct NmbHeap {uint64_t type, size, options, used = 0;};
struct NmbBuffer {
    uint64_t offset = 0, length = 0, allocation = 0;
    void *guest = nullptr;                     // newBufferWithBytesNoCopy / an IOSurface: the caller's memory; offset is its shadow
    NmbHeap *heap = nullptr;
};
struct NmbTexture {
    uint64_t handle = 0;
    QMTextureDescriptor descriptor = {};       // as the Mac made it
    NmbBuffer *import = nullptr;               // the shadowed memory under it, for its views too
    NmbHeap *heap = nullptr;
    uint64_t heapBytes = 0;
};
struct NmbLibrary {uint64_t handle; std::vector<std::string> names;};
struct NmbFunction {uint64_t handle, type;};
struct NmbComputePipeline {uint64_t handle, maxThreads, executionWidth;};
struct NmbRenderPipeline {uint64_t handle;};
struct NmbSampler {uint64_t handle;};
struct NmbDepthStencil {uint64_t handle;};
struct NmbQueue {uint64_t handle;};
struct NmbEvent {uint64_t handle; bool shared;};
struct NmbFence {uint64_t handle;};
struct NmbBinaryArchive {int unused;};
struct NmbCommandBuffer {
    NmbQueue *queue = nullptr;
    uint64_t number = 0;                        // its commit's number, from 1
    std::vector<unsigned char> commands;
    std::set<NmbBuffer *> reads, writes;        // shadowed memory it uses
    std::unordered_set<const void *> uses;      // buffers, textures and imports, for nmb_command_buffer_uses_*
    NmbEncoder *open = nullptr;
    std::mutex lock;
    std::condition_variable changed;
    uint64_t status = 0;                        // MTLCommandBufferStatus
    bool scheduled = false, released = false, copiedOut = false;
    std::string error;
    double gpuStart = 0, gpuEnd = 0;
    void (*onCompleted)(void *) = nullptr, (*onScheduled)(void *) = nullptr;
    void *completedContext = nullptr, *scheduledContext = nullptr;
};
struct NmbEncoder {
    NmbCommandBuffer *commandBuffer;
    int kind;                                   // 0 compute, 1 render, 2 blit
    NmbBuffer *bound[3][31] = {};
    uint64_t boundOffset[3][31] = {};
};

namespace {
NmbDevice *gDevice;

// ---- completions ----
std::mutex gCompletionLock;
std::condition_variable gCompletionWake;
std::unordered_map<uint64_t, NmbCommandBuffer *> gCommitted;
std::vector<NmbCommandBuffer *> gToSchedule;
struct Notify {NmbEvent *event; uint64_t value; void (*handler)(void *, uint64_t); void *context;};
std::vector<Notify> gNotifies;

void copyIn(NmbBuffer *buffer) {memcpy(gWindow + buffer->offset, buffer->guest, buffer->length);}
void copyOut(NmbBuffer *buffer) {memcpy(buffer->guest, gWindow + buffer->offset, buffer->length);}
// The frontend's handlers run on a queue of their own, in order (scheduled before completed): the completion thread never runs the OS's
// code, so a handler waiting on a lock the committing thread holds cannot stop the completion that thread waits for.
dispatch_queue_t gHandlers;
void deliverScheduled(NmbCommandBuffer *commandBuffer) {
    void (*handler)(void *);
    void *context;
    {
        std::lock_guard<std::mutex> lock(commandBuffer->lock);
        if (commandBuffer->scheduled) return;
        commandBuffer->scheduled = true;
        if (commandBuffer->status < 3) commandBuffer->status = 3;
        handler = commandBuffer->onScheduled;
        context = commandBuffer->scheduledContext;
    }
    commandBuffer->changed.notify_all();
    if (handler) dispatch_async_f(gHandlers, context, handler);
}
void complete(NmbCommandBuffer *commandBuffer, uint64_t status, double start, double end, const char *error) {
    if (status == 4) for (NmbBuffer *buffer : commandBuffer->writes) copyOut(buffer);
    deliverScheduled(commandBuffer);
    void (*handler)(void *);
    void *context;
    bool release;
    {
        std::lock_guard<std::mutex> lock(commandBuffer->lock);
        commandBuffer->status = status;
        commandBuffer->copiedOut = true;
        commandBuffer->gpuStart = start;
        commandBuffer->gpuEnd = end;
        if (error && *error) commandBuffer->error = error;
        handler = commandBuffer->onCompleted;
        context = commandBuffer->completedContext;
        release = commandBuffer->released;
    }
    commandBuffer->changed.notify_all();
    {
        std::lock_guard<std::mutex> lock(gFlightLock);
        gInFlight.erase(commandBuffer->number);
    }
    collectDeferred();
    // The handler may release the command buffer (the frontend's object goes with it): nothing here touches it afterwards.
    if (release) delete commandBuffer;
    if (handler) dispatch_async_f(gHandlers, context, handler);
}
bool pollNotifies() {
    std::vector<Notify> notifies;
    {
        std::lock_guard<std::mutex> lock(gCompletionLock);
        notifies.swap(gNotifies);
    }
    if (notifies.empty()) return false;
    std::vector<Notify> waiting;
    std::unordered_map<NmbEvent *, uint64_t> values;
    for (const Notify &n : notifies) {
        if (!values.count(n.event)) {
            Reply reply;
            values[n.event] = call(QM_EVENT_VALUE, n.event->handle, reply) ? replyWord(reply, 0) : 0;
        }
        if (values[n.event] >= n.value) n.handler(n.context, values[n.event]);
        else waiting.push_back(n);
    }
    std::lock_guard<std::mutex> lock(gCompletionLock);
    gNotifies.insert(gNotifies.end(), waiting.begin(), waiting.end());
    return !gNotifies.empty();
}
void *completionLoop(void *) {
    pthread_setname_np("qemu-metal completions");
    uint64_t read = 0;
    for (unsigned idle = 0;;) {
        std::vector<NmbCommandBuffer *> schedule;
        {
            std::lock_guard<std::mutex> lock(gCompletionLock);
            schedule.swap(gToSchedule);
        }
        for (NmbCommandBuffer *commandBuffer : schedule) deliverScheduled(commandBuffer);
        bool worked = !schedule.empty();
        const uint64_t written = load(&control()->completionsWritten);
        while (read < written) {
            QMCompletion record;
            memcpy(&record, gWindow + QM_COMPLETIONS + (read % QM_COMPLETION_SLOTS) * sizeof(QMCompletion), sizeof record);
            store(&control()->completionsRead, ++read);
            record.error[sizeof record.error - 1] = 0;
            NmbCommandBuffer *commandBuffer = nullptr;
            {
                std::lock_guard<std::mutex> lock(gCompletionLock);
                auto it = gCommitted.find(record.commandBuffer);
                if (it != gCommitted.end()) {commandBuffer = it->second; gCommitted.erase(it);}
            }
            if (commandBuffer) complete(commandBuffer, record.status, record.gpuStart, record.gpuEnd, record.error);
            worked = true;
        }
        if (pollNotifies()) worked = true, idle = 0;
        if (worked) {idle = 0; continue;}
        std::unique_lock<std::mutex> lock(gCompletionLock);
        if (gCommitted.empty() && gToSchedule.empty() && gNotifies.empty())
            gCompletionWake.wait_for(lock, std::chrono::milliseconds(50));
        else {
            lock.unlock();
            pause(64 + idle++);
        }
    }
    return nullptr;
}

// ---- device answers: the Mac's device answers the base class's limits; features only from a list the frontend implements ----
// Feature flags this device claims where the Mac's GPU has them: shader and format features the Mac runs on its own. Every other flag of
// the base class stays NO, so the OS keeps away from objects the frontend does not make (argument buffers, indirect commands, tile shaders,
// dynamic libraries, ray tracing, sparse resources, rasterization rate maps).
const char *const kFlags[] = {
    "supportsSIMDGroup", "supportsQuadGroup", "supportsSIMDReduction", "supportsSIMDShufflesAndBroadcast", "supportsQuadShufflesAndBroadcast",
    "isQuadDataSharingSupported", "supportsProgrammableBlending", "supportsRasterOrderGroups", "areRasterOrderGroupsSupported",
    "supportsMemorylessRenderTargets", "supportsBGR10A2", "supportsRGBA10A2Gamma", "isRGB10A2GammaSupported", "isASTCPixelFormatsSupported",
    "supportsASTCTextureCompression", "supportsSRGBwrites", "supportsSharedStorageTextures", "supportsBaseVertexInstanceDrawing",
    "supportsTextureCubeArray", "supportsRenderToLinearTextures", "supportsReadWriteTextureArguments", "isMsaa32bSupported", "supports32BitMSAA",
    "supports32BitFloatFiltering", "isFloat32FilteringSupported", "supportsSamplerCompareFunction", "supportsMirrorClampToEdgeSamplerMode",
    "supportsBlackOrWhiteSamplerBorderColors", "supportsDepthClipMode", "supportsInt64", "supportsNativeHardwareFP16", "supportsYCBCRFormats",
    "supportsYCBCRPackedFormats12", "supportsYCBCRFormats12", "supportsLimitedYUVFormats", "supportsAlphaYUVFormats", "supportsPullModelInterpolation",
    "supportsShaderLODAverage", "supportsQueryTextureLOD", "supportsInvariantVertexPosition", "supportsLargeFramebufferConfigs",
    "isLargeMRTSupported", "supportsAtomicFloat", "supportsFastMathInfNaNPropagation", "supportsFloat16InfNanFiltering",
    "areProgrammableSamplePositionsSupported", "supportsLayeredRendering", "supportsMSAADepthResolve", "supportsNonPrivateMSAATextures",
    "supportsNonPrivateDepthStencilTextures", "supportsTexture2DMultisampleArray", "supportsFragmentBufferWrites", "supportsRenderTextureWrites",
    "isFramebufferReadSupported", "supportsSIMDShuffleAndFill", "hasUnifiedMemory",
};
// Counts of the base class that are no limits of the GPU, or describe objects this device does not make.
const char *const kNotCounts[] = {
    "registryID", "deviceCreationFlags", "acceleratorPort", "currentCommandQueueCount", "commandQueueLimit", "argumentBuffersSupport",
    "commandBufferErrorOptions", "getSupportedCommandBufferErrorOptions", "maximumComputeSubstreams", "optionsForPipelineLibrarySerialization",
    "maxConcurrentExecutingCompilationTasks", "samplerReductionModeSupport", "currentAllocatedSize",
    "maxTransferRate", "location", "locationNumber", "peerGroupID", "peerIndex", "peerCount", "maxBufferLength", "sparseTileSizeInBytes",
    "maxIOCommandsInFlight", "bufferRobustnessSupport", "maxIndirectBuffers", "maxIndirectTextures", "maxIndirectSamplers",
    "maxIndirectSamplersPerDevice", "maxAccelerationStructureLevels",
};
// Queries with an argument, as the bridge has them (bridge/NativeMetalObjects.rs PUBLIC_ANSWERS).
const std::pair<const char *, uint32_t> kArgumentQueries[] = {
    {"minimumLinearTextureAlignmentForPixelFormat:", NMB_ANSWER_COUNT_ARG}, {"supportsTextureSampleCount:", NMB_ANSWER_FLAG_ARG},
    {"supportsVertexAmplificationCount:", NMB_ANSWER_FLAG_ARG}, {"supportsRasterizationRateMapWithLayerCount:", NMB_ANSWER_FLAG_ARG},
    {"supportsFamily:", NMB_ANSWER_FLAG_ARG}, {"supportsPrimitiveType:", NMB_ANSWER_FLAG_ARG},
    {"supportsTextureWriteRoundingMode:", NMB_ANSWER_FLAG_ARG},
};
// The target hardware's families (iPad7,1, A10X: Apple3 and below), as the bridge answers them: Core Image's Metal context needs Apple2,
// QuartzCore splits its frames into small command buffers where Apple3 is false.
bool targetFamily(uint64_t family) {return family == 1001 || family == 1002 || family == 1003 || family == 3001 || family == 3002;}
void askAnswers(NmbDevice *device) {
    std::set<std::string> flags(std::begin(kFlags), std::end(kFlags)), notCounts(std::begin(kNotCounts), std::end(kNotCounts));
    // The memory this process's GPU work has is its slot's heap, not the Mac's: Core Image sizes its caches from these.
    const uint64_t heap = gWindowBytes - QM_HEAP;
    const std::pair<const char *, uint64_t> memory[] = {{"sharedMemorySize", heap}, {"recommendedMaxWorkingSetSize", heap},
                                                        {"dedicatedMemorySize", 0}};
    std::set<std::string> seen;
    for (const auto &m : memory) {
        device->selectors.push_back({m.first, NMB_ANSWER_COUNT});
        device->answers[m.first] = m.second;
        seen.insert(m.first);
    }
    // The queries of the base class, and of the IOGPU driver class the target's own device derives from (Core Image asks it for its
    // memory sizes): the frontend's device derives from _MTLDevice alone. A process may not have loaded IOGPU yet.
    dlopen("/System/Library/PrivateFrameworks/IOGPU.framework/IOGPU", RTLD_LAZY | RTLD_LOCAL);
    std::vector<std::pair<std::string, uint32_t>> candidates;
    for (const char *className : {"_MTLDevice", "IOGPUMetalDevice"}) {
        Class base = objc_getClass(className);
        if (!base) continue;
        unsigned count = 0;
        Method *methods = class_copyMethodList(base, &count);
        for (unsigned i = 0; i < count; i++) {
            const char *name = sel_getName(method_getName(methods[i])), *types = method_getTypeEncoding(methods[i]);
            // IOGPU's own sizes of its kernel connection (…ShmemSize, cmdBufArgsSize) describe the driver's protocol, not the GPU.
            if (!types || strchr(name, ':') || strcmp(types + 1, "16@0:8") || strstr(name, "Shmem") || !strcmp(name, "cmdBufArgsSize") ||
                !seen.insert(name).second)
                continue;
            if (strchr("QqI", types[0]) && !notCounts.count(name)) candidates.push_back({name, NMB_ANSWER_COUNT});
            else if (strchr("Bc", types[0]) && flags.count(name)) candidates.push_back({name, NMB_ANSWER_FLAG});
        }
        free(methods);
    }
    std::vector<unsigned char> payload(8);
    uint64_t count = candidates.size();
    memcpy(payload.data(), &count, 8);
    for (const auto &c : candidates) payload.insert(payload.end(), c.first.c_str(), c.first.c_str() + c.first.size() + 1);
    Reply reply;
    if (call(QM_ANSWERS, payload.data(), payload.size(), reply) && reply.data.size() >= candidates.size() * sizeof(QMAnswer)) {
        for (size_t i = 0; i < candidates.size(); i++) {
            QMAnswer answer;
            memcpy(&answer, reply.data.data() + i * sizeof answer, sizeof answer);
            if (!answer.known) continue;
            device->selectors.push_back(candidates[i]);
            device->answers[candidates[i].first] = answer.value;
        }
    }
    for (const auto &query : kArgumentQueries) device->selectors.push_back({query.first, query.second});
}

NmbTexture *textureReply(const Reply &reply, NmbBuffer *import) {
    if (reply.data.size() < sizeof(QMTextureDescriptor)) return refuse<NmbTexture>("short texture reply");
    NmbTexture *texture = new NmbTexture;
    texture->handle = reply.handle;
    memcpy(&texture->descriptor, reply.data.data(), sizeof texture->descriptor);
    texture->import = import;
    return texture;
}
// A count, then that many NUL-terminated names.
std::vector<std::string> names(const Reply &reply) {
    std::vector<std::string> out;
    const uint64_t count = replyWord(reply, 0);
    for (size_t at = 8; out.size() < count && at < reply.data.size();) {
        const char *name = reinterpret_cast<const char *>(reply.data.data() + at);
        const size_t n = strnlen(name, reply.data.size() - at);
        out.emplace_back(name, n);
        at += n + 1;
    }
    return out;
}
NmbLibrary *libraryReply(const Reply &reply) {return new NmbLibrary{reply.handle, names(reply)};}
QMTextureDescriptor wire(const NmbTextureDescriptor *d) {
    return {d->type, d->format, d->width, d->height, d->depth, d->levels, d->samples, d->array_length, d->usage, d->options};
}
// The window offset of a buffer's bytes at offset.
uint64_t at(const NmbBuffer *buffer, uint64_t offset) {return buffer ? buffer->offset + offset : QM_NONE;}

// ---- recording ----
bool emit(NmbCommandBuffer *commandBuffer, uint32_t op, const void *fields, size_t bytes, const void *extra = nullptr, size_t extraBytes = 0) {
    if (!commandBuffer) return fail("no command buffer");
    if (commandBuffer->number) return fail("command buffer already committed");
    const QMCommandHeader header = {op, (uint32_t)align(sizeof(QMCommandHeader) + bytes + extraBytes, 8)};
    std::vector<unsigned char> &out = commandBuffer->commands;
    const size_t start = out.size();
    out.resize(start + header.bytes);
    memcpy(out.data() + start, &header, sizeof header);
    if (bytes) memcpy(out.data() + start + sizeof header, fields, bytes);
    if (extraBytes) memcpy(out.data() + start + sizeof header + bytes, extra, extraBytes);
    return true;
}
template <class T> bool emit(NmbEncoder *encoder, uint32_t op, const T &fields) {
    return encoder && emit(encoder->commandBuffer, op, &fields, sizeof fields);
}
void use(NmbCommandBuffer *commandBuffer, NmbBuffer *buffer, bool written) {
    if (!buffer) return;
    commandBuffer->uses.insert(buffer);
    if (buffer->guest) (written ? commandBuffer->writes : commandBuffer->reads).insert(buffer);
}
void use(NmbCommandBuffer *commandBuffer, NmbTexture *texture, bool written) {
    if (!texture) return;
    commandBuffer->uses.insert(texture);
    if (texture->import) use(commandBuffer, texture->import, written);
}
}  // namespace

extern "C" void qemu_metal_set_window(void *base, uint64_t bytes) {
    gWindow = static_cast<unsigned char *>(base);
    gWindowBytes = bytes;
}

// A line into the window's log ring, which qemu-metal-host copies into its log: the guest process's own view (the plugin, missing
// selectors, refusals) without a way into the guest. A line that does not fit in what the host has not read yet is dropped.
extern "C" void qemu_metal_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
extern "C" void qemu_metal_log(const char *format, ...) {
    if (!gWindow) return;
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    int n = vsnprintf(line, sizeof line - 1, format, arguments);
    va_end(arguments);
    if (n < 0) return;
    n = std::min<int>(n, (int)sizeof line - 2);
    line[n++] = '\n';
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    const uint64_t written = control()->logWritten;
    if (written + (uint64_t)n - load(&control()->logRead) > QM_LOG_BYTES) return;
    for (int i = 0; i < n; i++) gWindow[QM_LOG + (written + (uint64_t)i) % QM_LOG_BYTES] = (unsigned char)line[i];
    store(&control()->logWritten, written + (uint64_t)n);
}

// The Mac device's answer to an argument-less query the frontend lacks (QemuMetalPlugin.mm's gap answers).
extern "C" bool qemu_metal_answer(const char *selector, uint64_t *value) {
    return gDevice && nmb_device_answer(gDevice, selector, 0, value);
}

// Function stitching, which the bridge has no call for (QemuMetalPlugin.mm builds the request from MTLStitchedLibraryDescriptor):
// the Mac's handle of a function, and a library from a QM_STITCHED_LIBRARY request, as nmb_library_new's.
extern "C" uint64_t qemu_metal_function_handle(const void *function) {
    return function ? static_cast<const NmbFunction *>(function)->handle : 0;
}
extern "C" void *qemu_metal_stitched_library(const void *payload, size_t bytes) {
    Reply reply;
    return call(QM_STITCHED_LIBRARY, payload, bytes, reply) ? libraryReply(reply) : nullptr;
}
// A library compiled on the Mac from Metal source (QemuMetalLibraries.mm): the source in the window for the request, the options and the
// macros in the request itself.
extern "C" void *qemu_metal_source_library(const void *source, size_t length, const QMSource *options, const void *macros, size_t macroBytes) {
    Staging staging(length ? length : 1);
    if (!staging.offset) return refuse<void>("the window is full");
    memcpy(staging.data(), source, length);
    QMSource fields = *options;
    fields.offset = staging.offset;
    fields.length = length;
    std::vector<unsigned char> payload(sizeof fields);
    memcpy(payload.data(), &fields, sizeof fields);
    payload.insert(payload.end(), static_cast<const unsigned char *>(macros), static_cast<const unsigned char *>(macros) + macroBytes);
    Reply reply;
    return call(QM_SOURCE_LIBRARY, payload.data(), payload.size(), reply) ? libraryReply(reply) : nullptr;
}
// A library's externFunctionNames, NUL-separated into out; the count, or -1 where the Mac's library has no answer.
extern "C" long qemu_metal_extern_names(const void *library, char *out, size_t size) {
    if (!library) return -1;
    const uint64_t handle = static_cast<const NmbLibrary *>(library)->handle;
    Reply reply;
    if (!call(QM_EXTERN_NAMES, handle, reply)) return -1;
    size_t at = 0;
    const std::vector<std::string> list = names(reply);
    for (const std::string &name : list) {
        if (at + name.size() + 1 > size) break;
        memcpy(out + at, name.c_str(), name.size() + 1);
        at += name.size() + 1;
    }
    return (long)list.size();
}

// Selectors the frontend's classes lack, with the answer that holds for every object of this device: no texture here has a swizzle (the
// bridge's descriptor carries none), memory attribution to another process has no meaning on the Mac's GPU, and the short form of a
// compute pipeline from a descriptor is the long one without options or reflection.
namespace {
uint32_t identitySwizzle(id, SEL) {return 0x05040302u;}   // MTLTextureSwizzleChannels {red, green, blue, alpha}
bool notDrawable(id, SEL) {return false;}                 // no texture of this device is a drawable's
void ignoreProcess(id, SEL, int) {}
id computeWithDescriptor(id self, SEL, id descriptor, void *error) {
    return ((id (*)(id, SEL, id, uint64_t, void *, void *))objc_msgSend)(
        self, sel_registerName("newComputePipelineStateWithDescriptor:options:reflection:error:"), descriptor, 0, nullptr, error);
}
// A compute encoder's imageblock size: the bridge has no call for it, so the encoder's own handle (NBEncoder.handle) records it.
void imageblock(id self, SEL, unsigned long width, unsigned long height) {
    NmbEncoder *encoder = ((NmbEncoder * (*)(id, SEL))objc_msgSend)(self, sel_registerName("handle"));
    const uint64_t fields[2] = {width, height};
    if (encoder) emit(encoder->commandBuffer, QC_IMAGEBLOCK, fields, sizeof fields);
}
}
extern "C" void qemu_metal_complete_frontend(void *device) {
    for (const char *name : {"NBTexture", "NBBuffer", "NBHeap"}) {
        Class cls = objc_getClass(name);
        if (!cls) continue;
        if (!strcmp(name, "NBTexture")) {
            class_addMethod(cls, sel_registerName("swizzle"), (IMP)identitySwizzle, "{?=CCCC}16@0:8");
            class_addMethod(cls, sel_registerName("isDrawable"), (IMP)notDrawable, "B16@0:8");
        }
        class_addMethod(cls, sel_registerName("setResponsibleProcess:"), (IMP)ignoreProcess, "v20@0:8i16");
    }
    if (Class encoder = objc_getClass("NBComputeEncoder"))
        class_addMethod(encoder, sel_registerName("setImageblockWidth:height:"), (IMP)imageblock, "v32@0:8Q16Q24");
    if (device)
        class_addMethod(object_getClass((id)device), sel_registerName("newComputePipelineStateWithDescriptor:error:"), (IMP)computeWithDescriptor,
                        "@32@0:8@16^@24");
}

extern "C" {
const char *nmb_last_error(void) {return tError.c_str();}
const void *nmb_fixture_air(size_t *length) {if (length) *length = 0; return nullptr;}
const void *nmb_function_constant_air(size_t *length) {if (length) *length = 0; return nullptr;}

// loader and cache belong to the bridge's Vulkan runtime; the window comes from qemu_metal_set_window (the plugin, through the kernel), or
// on the Mac from the file QEMU_METAL_WINDOW names (the host instruments).
NmbDevice *nmb_device_open(const char *loader, const char *cache) {
    (void)loader; (void)cache;
    if (gDevice) return refuse<NmbDevice>("the device is open already");
    if (!gWindow) {
        const char *path = getenv("QEMU_METAL_WINDOW");
        int file = path ? open(path, O_RDWR) : -1;
        struct stat info = {};
        if (file < 0 || fstat(file, &info)) return refuse<NmbDevice>("no window (QEMU_METAL_WINDOW)");
        // Slot 0 of the file, which qemu-metal-host serves without --slot.
        uint64_t offset = 0, bytes = 0;
        qm_slot((uint64_t)info.st_size, 0, &offset, &bytes);
        void *base = mmap(nullptr, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, file, (off_t)offset);
        close(file);
        if (base == MAP_FAILED) return refuse<NmbDevice>(std::string("window: ") + strerror(errno));
        qemu_metal_set_window(base, bytes);
    }
    if (gWindowBytes < QM_HEAP + (64ull << 20)) return refuse<NmbDevice>("window too small");
    control()->magic = QM_MAGIC;
    control()->version = QM_VERSION;
    control()->windowBytes = gWindowBytes;
    gRequestNumber = load(&control()->reply);
    gFree.clear();
    gFree[QM_HEAP] = gWindowBytes - QM_HEAP;
    const QMOpen request = {((uint64_t)getpid() << 32) ^ clock_gettime_nsec_np(CLOCK_UPTIME_RAW), gWindowBytes};
    Reply reply;
    if (!call(QM_OPEN, request, reply) || reply.data.size() < sizeof(QMDeviceInfo)) return nullptr;
    store(&control()->completionsRead, 0);
    NmbDevice *device = new NmbDevice;
    memcpy(&device->info, reply.data.data(), sizeof device->info);
    askAnswers(device);
    gDevice = device;
    gHandlers = dispatch_queue_create("qemu-metal handlers", DISPATCH_QUEUE_SERIAL);
    pthread_t thread;
    pthread_create(&thread, nullptr, completionLoop, nullptr);
    pthread_detach(thread);
    return device;
}
void nmb_device_release(NmbDevice *) {}
size_t nmb_device_name(const NmbDevice *device, char *out, size_t size) {
    return device ? (size_t)snprintf(out, size, "%s (QEMU)", device->info.name) : 0;
}
uint64_t nmb_device_max_buffer_length(const NmbDevice *device) {
    return device ? std::min<uint64_t>(device->info.maxBufferLength, (gWindowBytes - QM_HEAP) / 2) : 0;
}
bool nmb_device_work_stats(const NmbDevice *, uint64_t out[10]) {memset(out, 0, 80); return true;}
bool nmb_device_shader_cache_stats(const NmbDevice *, struct NmbShaderCacheStats *out) {memset(out, 0, sizeof *out); return true;}
size_t nmb_last_warning(char *out, size_t size) {if (size) *out = 0; return 0;}
bool nmb_device_max_threads_per_threadgroup(const NmbDevice *device, uint64_t out[3]) {
    if (!device) return fail("no device");
    memcpy(out, device->info.maxThreads, 24);
    return true;
}
const char *nmb_device_answer_selector(size_t index, uint32_t *kind) {
    if (!gDevice || index >= gDevice->selectors.size()) return nullptr;
    if (kind) *kind = gDevice->selectors[index].second;
    return gDevice->selectors[index].first.c_str();
}
bool nmb_device_answer(const NmbDevice *device, const char *selector, uint64_t arg, uint64_t *out) {
    if (!device || !selector || !out) return fail("no device or selector");
    NmbDevice *d = const_cast<NmbDevice *>(device);
    auto known = d->answers.find(selector);
    if (known != d->answers.end()) {*out = known->second; return true;}
#if TARGET_OS_IPHONE
    if (!strcmp(selector, "supportsFamily:")) {*out = targetFamily(arg); return true;}
#else
    // The host instruments run this Mac's own QuartzCore, which asks for this Mac's families.
    if (!strcmp(selector, "supportsFamily:") && targetFamily(arg)) {*out = 1; return true;}
#endif
    {
        std::lock_guard<std::mutex> lock(d->answerLock);
        auto cached = d->argumentAnswers.find({selector, arg});
        if (cached != d->argumentAnswers.end()) {*out = cached->second; return true;}
    }
    QMAnswerRequest request = {};
    snprintf(request.selector, sizeof request.selector, "%s", selector);
    request.argument = arg;
    Reply reply;
    if (!call(QM_ANSWER, request, reply)) return false;
    *out = replyWord(reply, 0);
    std::lock_guard<std::mutex> lock(d->answerLock);
    d->argumentAnswers[{selector, arg}] = *out;
    return true;
}
size_t nmb_device_vendor_name(const NmbDevice *device, char *out, size_t size) {
    return device ? (size_t)snprintf(out, size, "%s", device->info.vendor) : 0;
}
size_t nmb_device_family_waivers(const NmbDevice *, char *out, size_t size) {
    return (size_t)snprintf(out, size, "Apple1 0:\nApple2 0:\nApple3 0:\nCommon1 0:\nCommon2 0:\n");
}
uint64_t nmb_device_linear_bytes_per_row(const NmbDevice *, uint64_t format, uint64_t width) {
    const QMHandleValue request = {format, width};
    Reply reply;
    return call(QM_LINEAR_BYTES_PER_ROW, request, reply) ? replyWord(reply, 0) : 0;
}

// Pipeline archives: the Mac keeps its own shader cache, so an archive records nothing and serializes nothing.
NmbBinaryArchive *nmb_binary_archive_new(NmbDevice *, const char *) {return new NmbBinaryArchive{};}
void nmb_binary_archive_release(NmbBinaryArchive *archive) {delete archive;}
bool nmb_binary_archive_serialize(NmbBinaryArchive *, const char *) {return true;}
bool nmb_binary_archive_add_render(NmbBinaryArchive *, const struct NmbRenderPipelineDescriptor *) {return true;}
bool nmb_binary_archive_add_compute(NmbBinaryArchive *, NmbFunction *) {return true;}
bool nmb_binary_archive_add_compute_descriptor(NmbBinaryArchive *, const struct NmbComputePipelineDescriptor *) {return true;}

// ---- buffers and heaps ----
NmbBuffer *nmb_buffer_new(NmbDevice *, uint64_t length, uint64_t options, const void *bytes) {
    (void)options;
    const uint64_t offset = length ? allocate(length) : 0;
    if (!offset) return refuse<NmbBuffer>(length ? "the window is full" : "empty buffer");
    NmbBuffer *buffer = new NmbBuffer;
    buffer->offset = offset;
    buffer->length = length;
    buffer->allocation = length;
    if (bytes) memcpy(gWindow + offset, bytes, length);
    else memset(gWindow + offset, 0, length);
    return buffer;
}
NmbBuffer *nmb_buffer_new_no_copy(NmbDevice *, void *pointer, uint64_t length, uint64_t options) {
    (void)options;
    if (!pointer || !length) return refuse<NmbBuffer>("no memory");
    const uint64_t offset = allocate(length);
    if (!offset) return refuse<NmbBuffer>("the window is full");
    NmbBuffer *buffer = new NmbBuffer;
    buffer->offset = offset;
    buffer->length = length;
    buffer->allocation = length;
    buffer->guest = pointer;
    return buffer;
}
void nmb_buffer_release(NmbBuffer *buffer) {
    if (!buffer) return;
    if (buffer->heap) buffer->heap->used -= align(buffer->allocation, QM_HEAP_ALIGNMENT);
    deallocateLater(buffer->offset, buffer->allocation);
    delete buffer;
}
void *nmb_buffer_contents(const NmbBuffer *buffer) {return buffer ? (buffer->guest ? buffer->guest : gWindow + buffer->offset) : nullptr;}
uint64_t nmb_buffer_length(const NmbBuffer *buffer) {return buffer ? buffer->length : 0;}
uint64_t nmb_buffer_gpu_address(const NmbBuffer *buffer) {
    return buffer && gDevice ? gDevice->info.heapAddress + (buffer->offset - QM_HEAP) : 0;
}
// Nothing here is ever purged: the answer is MTLPurgeableStateNonVolatile.
uint64_t nmb_buffer_set_purgeable_state(const NmbBuffer *, uint64_t) {return 2;}
NmbHeap *nmb_heap_new(NmbDevice *, uint64_t type, uint64_t size, uint64_t options) {return new NmbHeap{type, size, options};}
void nmb_heap_release(NmbHeap *heap) {delete heap;}
uint64_t nmb_heap_size(const NmbHeap *heap) {return heap ? heap->size : 0;}
uint64_t nmb_heap_used_size(const NmbHeap *heap) {return heap ? heap->used : 0;}
uint64_t nmb_heap_allocated_size(const NmbHeap *heap) {return heap ? heap->size : 0;}
uint64_t nmb_heap_max_available_size(const NmbHeap *heap, uint64_t alignment) {
    if (!heap || heap->used >= heap->size) return 0;
    const uint64_t left = heap->size - heap->used;
    return alignment > 1 ? left / alignment * alignment : left;
}
uint64_t nmb_heap_set_purgeable_state(const NmbHeap *, uint64_t) {return 2;}
NmbBuffer *nmb_heap_new_buffer(const NmbHeap *heap, uint64_t length, uint64_t options, uint64_t offset) {
    (void)offset;
    if (!heap) return refuse<NmbBuffer>("no heap");
    NmbHeap *h = const_cast<NmbHeap *>(heap);
    if (h->used + align(length, QM_HEAP_ALIGNMENT) > h->size) return refuse<NmbBuffer>("the heap is full");
    NmbBuffer *buffer = nmb_buffer_new(gDevice, length, options, nullptr);
    if (!buffer) return nullptr;
    buffer->heap = h;
    h->used += align(length, QM_HEAP_ALIGNMENT);
    return buffer;
}

// ---- textures ----
NmbTexture *nmb_texture_new(NmbDevice *, const struct NmbTextureDescriptor *descriptor) {
    if (!descriptor) return refuse<NmbTexture>("no descriptor");
    const QMTexture request = {wire(descriptor)};
    Reply reply;
    return call(QM_TEXTURE, request, reply) ? textureReply(reply, nullptr) : nullptr;
}
NmbTexture *nmb_heap_new_texture(const NmbHeap *heap, const struct NmbTextureDescriptor *descriptor, uint64_t offset) {
    (void)offset;
    if (!heap || !descriptor) return refuse<NmbTexture>("no heap or descriptor");
    // What the texture takes of the heap, roughly: four bytes a texel, a third more for its levels.
    const uint64_t bytes = align(descriptor->width * descriptor->height * std::max<uint64_t>(descriptor->depth, 1) *
                                 std::max<uint64_t>(descriptor->array_length, 1) * std::max<uint64_t>(descriptor->samples, 1) * 4 *
                                 (descriptor->levels > 1 ? 4 : 3) / 3, QM_HEAP_ALIGNMENT);
    NmbHeap *h = const_cast<NmbHeap *>(heap);
    if (h->used + bytes > h->size) return refuse<NmbTexture>("the heap is full");
    NmbTexture *texture = nmb_texture_new(gDevice, descriptor);
    if (!texture) return nullptr;
    texture->heap = h;
    texture->heapBytes = bytes;
    h->used += bytes;
    return texture;
}
static NmbTexture *textureOnBuffer(NmbBuffer *buffer, const struct NmbTextureDescriptor *descriptor, uint64_t offset, uint64_t bytesPerRow,
                                   uint64_t bytesPerImage, bool surface) {
    if (!buffer || !descriptor) return refuse<NmbTexture>("no buffer or descriptor");
    if (offset >= buffer->length) return refuse<NmbTexture>("texture offset past the buffer");
    const QMTextureOnBuffer request = {wire(descriptor), buffer->offset + offset, bytesPerRow, bytesPerImage, surface};
    Reply reply;
    return call(QM_TEXTURE_ON_BUFFER, request, reply) ? textureReply(reply, buffer->guest ? buffer : nullptr) : nullptr;
}
NmbTexture *nmb_texture_new_on_buffer(NmbBuffer *buffer, const struct NmbTextureDescriptor *descriptor, uint64_t offset, uint64_t bytes_per_row,
                                      bool iosurface) {
    return textureOnBuffer(buffer, descriptor, offset, bytes_per_row, 0, iosurface);
}
NmbTexture *nmb_texture_new_linear(NmbBuffer *buffer, const struct NmbTextureDescriptor *descriptor, uint64_t offset, uint64_t bytes_per_row,
                                   uint64_t bytes_per_image) {
    return textureOnBuffer(buffer, descriptor, offset, bytes_per_row, bytes_per_image, false);
}
NmbTexture *nmb_texture_new_view(NmbTexture *texture, uint64_t format, uint64_t kind, const uint64_t levels[2], const uint64_t slices[2], bool whole) {
    if (!texture) return refuse<NmbTexture>("no texture");
    QMTextureView request = {texture->handle, format, kind, {0, 0}, {0, 0}, whole};
    if (!whole && levels && slices) {
        memcpy(request.levels, levels, 16);
        memcpy(request.slices, slices, 16);
    }
    Reply reply;
    return call(QM_TEXTURE_VIEW, request, reply) ? textureReply(reply, texture->import) : nullptr;
}
uint64_t nmb_texture_set_purgeable_state(const NmbTexture *, uint64_t) {return 2;}
void nmb_texture_release(NmbTexture *texture) {
    if (!texture) return;
    if (texture->heap) texture->heap->used -= texture->heapBytes;
    releaseLater(texture->handle);
    delete texture;
}
void nmb_texture_describe(const NmbTexture *texture, struct NmbTextureDescriptor *out) {
    if (!texture || !out) return;
    const QMTextureDescriptor &d = texture->descriptor;
    *out = {d.type, d.format, d.width, d.height, d.depth, d.levels, d.samples, d.arrayLength, d.usage, d.options};
}
bool nmb_texture_replace(NmbTexture *texture, const uint64_t region[6], uint64_t level, uint64_t slice, const void *bytes, uint64_t bytes_per_row,
                         uint64_t bytes_per_image, uint64_t length) {
    if (!texture || !bytes || !length) return fail("no texture or bytes");
    Staging staging(length);
    if (!staging.offset) return fail("the window is full");
    memcpy(staging.data(), bytes, length);
    // A texture over shadowed memory: the CPU's bytes there are what the next command buffer copies in, so the Mac writes the shadow and the
    // shadow goes back out whole.
    if (texture->import) copyIn(texture->import);
    QMTextureBytes request = {texture->handle, {}, level, slice, staging.offset, bytes_per_row, bytes_per_image, length};
    memcpy(request.region, region, 48);
    Reply reply;
    if (!call(QM_TEXTURE_REPLACE, request, reply)) return false;
    if (texture->import) copyOut(texture->import);
    return true;
}
bool nmb_texture_get_bytes(const NmbTexture *texture, void *bytes, uint64_t bytes_per_row, uint64_t bytes_per_image, uint64_t length,
                           const uint64_t region[6], uint64_t level, uint64_t slice) {
    if (!texture || !bytes || !length) return fail("no texture or bytes");
    Staging staging(length);
    if (!staging.offset) return fail("the window is full");
    if (texture->import) copyIn(texture->import);
    QMTextureBytes request = {texture->handle, {}, level, slice, staging.offset, bytes_per_row, bytes_per_image, length};
    memcpy(request.region, region, 48);
    Reply reply;
    if (!call(QM_TEXTURE_GET_BYTES, request, reply)) return false;
    memcpy(bytes, staging.data(), length);
    return true;
}

// ---- libraries, functions, states ----
NmbLibrary *nmb_library_new(NmbDevice *, const void *bytes, size_t length, bool air) {
    if (air) return refuse<NmbLibrary>("bare AIR bitcode: the Mac's Metal takes metallibs only");
    if (!bytes || !length) return refuse<NmbLibrary>("empty library");
    Staging staging(length);
    if (!staging.offset) return refuse<NmbLibrary>("the window is full");
    memcpy(staging.data(), bytes, length);
    const QMLibrary request = {staging.offset, length};
    Reply reply;
    return call(QM_LIBRARY, request, reply) ? libraryReply(reply) : nullptr;
}
void nmb_library_release(NmbLibrary *library) {
    if (!library) return;
    releaseLater(library->handle);
    delete library;
}
uint32_t nmb_library_function_count(const NmbLibrary *library) {return library ? (uint32_t)library->names.size() : 0;}
size_t nmb_library_function_name(const NmbLibrary *library, uint32_t index, char *out, size_t size) {
    if (!library || index >= library->names.size()) return 0;
    return (size_t)snprintf(out, size, "%s", library->names[index].c_str());
}
NmbFunction *nmb_function_new_specialized(NmbLibrary *library, const char *name, const struct NmbConstant *constants, uint32_t count) {
    if (!library || !name) return refuse<NmbFunction>("no library or name");
    std::vector<unsigned char> payload(sizeof(QMFunction));
    const QMFunction header = {library->handle, strlen(name) + 1, count};
    memcpy(payload.data(), &header, sizeof header);
    auto append = [&](const void *bytes, size_t n) {
        payload.insert(payload.end(), static_cast<const unsigned char *>(bytes), static_cast<const unsigned char *>(bytes) + n);
        payload.resize(align(payload.size(), 8));
    };
    append(name, header.nameBytes);
    for (uint32_t i = 0; i < count; i++) {
        const NmbConstant &c = constants[i];
        const QMConstant constant = {c.type, c.index, c.name ? strlen(c.name) + 1 : 0, c.length};
        append(&constant, sizeof constant);
        if (c.name) append(c.name, constant.nameBytes);
        if (c.length) append(c.bytes, c.length);
    }
    Reply reply;
    if (!call(QM_FUNCTION, payload.data(), payload.size(), reply)) return nullptr;
    return new NmbFunction{reply.handle, replyWord(reply, 0)};
}
NmbFunction *nmb_function_new(NmbLibrary *library, const char *name) {return nmb_function_new_specialized(library, name, nullptr, 0);}
void nmb_function_release(NmbFunction *function) {
    if (!function) return;
    releaseLater(function->handle);
    delete function;
}
uint64_t nmb_function_type(const NmbFunction *function) {return function ? function->type : 0;}

static NmbComputePipeline *computePipeline(const QMComputePipeline &request) {
    Reply reply;
    if (!call(QM_COMPUTE_PIPELINE, request, reply)) return nullptr;
    return new NmbComputePipeline{reply.handle, replyWord(reply, 0), replyWord(reply, 1)};
}
NmbComputePipeline *nmb_compute_pipeline_new(NmbDevice *, NmbFunction *function) {
    if (!function) return refuse<NmbComputePipeline>("no function");
    QMComputePipeline request = {};
    request.function = function->handle;
    return computePipeline(request);
}
NmbComputePipeline *nmb_compute_pipeline_new_with_options(NmbDevice *device, NmbFunction *function, const struct NmbPipelineOptions *) {
    return nmb_compute_pipeline_new(device, function);
}
NmbComputePipeline *nmb_compute_pipeline_new_with_descriptor(NmbDevice *, const struct NmbComputePipelineDescriptor *d, const struct NmbPipelineOptions *) {
    if (!d || !d->function) return refuse<NmbComputePipeline>("no function");
    if (d->indirect) return refuse<NmbComputePipeline>("indirect command buffers are not supported");
    QMComputePipeline request = {d->function->handle, d->max_threads, d->multiple_of_execution_width, d->max_call_stack_depth, {}};
    memcpy(request.mutability, d->mutability, sizeof request.mutability);
    return computePipeline(request);
}
void nmb_compute_pipeline_release(NmbComputePipeline *pipeline) {
    if (!pipeline) return;
    releaseLater(pipeline->handle);
    delete pipeline;
}
uint64_t nmb_compute_pipeline_max_threads(const NmbComputePipeline *pipeline) {return pipeline ? pipeline->maxThreads : 0;}
uint64_t nmb_compute_pipeline_execution_width(const NmbComputePipeline *pipeline) {return pipeline ? pipeline->executionWidth : 0;}
NmbRenderPipeline *nmb_render_pipeline_new(NmbDevice *, const struct NmbRenderPipelineDescriptor *d) {
    if (!d || !d->vertex) return refuse<NmbRenderPipeline>("no vertex function");
    QMRenderPipeline request = {};
    request.vertex = d->vertex->handle;
    request.fragment = d->fragment ? d->fragment->handle : 0;
    static_assert(sizeof(QMColorAttachment) == sizeof(NmbColorAttachment) && sizeof(QMVertexAttribute) == sizeof(NmbVertexAttribute) &&
                  sizeof(QMVertexLayout) == sizeof(NmbVertexLayout), "the wire's descriptors are the bridge's");
    memcpy(request.colors, d->colors, sizeof request.colors);
    request.depthFormat = d->depth_format;
    request.stencilFormat = d->stencil_format;
    request.samples = d->samples;
    request.topology = d->topology;
    request.rasterization = d->rasterization;
    request.vertexInputs = d->vertex_inputs;
    memcpy(request.attributes, d->attributes, sizeof request.attributes);
    memcpy(request.layouts, d->layouts, sizeof request.layouts);
    Reply reply;
    return call(QM_RENDER_PIPELINE, request, reply) ? new NmbRenderPipeline{reply.handle} : nullptr;
}
NmbRenderPipeline *nmb_render_pipeline_new_with_options(NmbDevice *device, const struct NmbRenderPipelineDescriptor *d, const struct NmbPipelineOptions *) {
    return nmb_render_pipeline_new(device, d);
}
void nmb_render_pipeline_release(NmbRenderPipeline *pipeline) {
    if (!pipeline) return;
    releaseLater(pipeline->handle);
    delete pipeline;
}
NmbSampler *nmb_sampler_new(NmbDevice *, const struct NmbSamplerDescriptor *d) {
    if (!d) return refuse<NmbSampler>("no descriptor");
    const QMSampler request = {d->min_filter, d->mag_filter, d->mip_filter, d->s_address, d->t_address, d->r_address, d->border_color,
                               d->normalized, d->max_anisotropy, d->compare, d->lod_average, d->lod_min, d->lod_max};
    Reply reply;
    return call(QM_SAMPLER, request, reply) ? new NmbSampler{reply.handle} : nullptr;
}
void nmb_sampler_release(NmbSampler *sampler) {
    if (!sampler) return;
    releaseLater(sampler->handle);
    delete sampler;
}
NmbDepthStencil *nmb_depth_stencil_new(NmbDevice *, const struct NmbDepthStencilDescriptor *d) {
    if (!d) return refuse<NmbDepthStencil>("no descriptor");
    static_assert(sizeof(QMDepthStencil) == sizeof(NmbDepthStencilDescriptor), "the wire's descriptor is the bridge's");
    QMDepthStencil request;
    memcpy(&request, d, sizeof request);
    Reply reply;
    return call(QM_DEPTH_STENCIL, request, reply) ? new NmbDepthStencil{reply.handle} : nullptr;
}
void nmb_depth_stencil_release(NmbDepthStencil *state) {
    if (!state) return;
    releaseLater(state->handle);
    delete state;
}

// ---- queues, events, fences ----
NmbQueue *nmb_queue_new(NmbDevice *) {
    Reply reply;
    return call(QM_QUEUE, nullptr, 0, reply) ? new NmbQueue{reply.handle} : nullptr;
}
void nmb_queue_release(NmbQueue *queue) {
    if (!queue) return;
    releaseLater(queue->handle);
    delete queue;
}
NmbEvent *nmb_event_new(NmbDevice *, bool shared) {
    const uint64_t request = shared;
    Reply reply;
    return call(QM_EVENT, request, reply) ? new NmbEvent{reply.handle, shared} : nullptr;
}
void nmb_event_release(NmbEvent *event) {
    if (!event) return;
    {
        std::lock_guard<std::mutex> lock(gCompletionLock);
        for (const Notify &n : gNotifies)
            if (n.event == event) return;   // a pending notification keeps it: the frontend holds the event while it waits
    }
    releaseLater(event->handle);
    delete event;
}
uint64_t nmb_shared_event_value(const NmbEvent *event) {
    Reply reply;
    return event && call(QM_EVENT_VALUE, event->handle, reply) ? replyWord(reply, 0) : 0;
}
bool nmb_shared_event_set_value(NmbEvent *event, uint64_t value) {
    if (!event) return fail("no event");
    const QMHandleValue request = {event->handle, value};
    Reply reply;
    return call(QM_EVENT_SET_VALUE, request, reply);
}
bool nmb_shared_event_wait(NmbEvent *event, uint64_t value, uint64_t timeout_ms) {
    if (!event) return fail("no event");
    const uint64_t began = nowMs();
    for (unsigned round = 0;; round++) {
        if (nmb_shared_event_value(event) >= value) return true;
        if (timeout_ms && nowMs() - began >= timeout_ms) return false;
        usleep(round < 100 ? 100 : 1000);
    }
}
bool nmb_shared_event_notify(NmbEvent *event, uint64_t value, void (*handler)(void *, uint64_t), void *context) {
    if (!event || !handler) return fail("no event or handler");
    {
        std::lock_guard<std::mutex> lock(gCompletionLock);
        gNotifies.push_back({event, value, handler, context});
    }
    gCompletionWake.notify_all();
    return true;
}
NmbFence *nmb_fence_new(NmbDevice *) {
    Reply reply;
    return call(QM_FENCE, nullptr, 0, reply) ? new NmbFence{reply.handle} : nullptr;
}
void nmb_fence_release(NmbFence *fence) {
    if (!fence) return;
    releaseLater(fence->handle);
    delete fence;
}

// ---- command buffers ----
NmbCommandBuffer *nmb_command_buffer_new(NmbQueue *queue) {
    if (!queue) return refuse<NmbCommandBuffer>("no queue");
    NmbCommandBuffer *commandBuffer = new NmbCommandBuffer;
    commandBuffer->queue = queue;
    return commandBuffer;
}
void nmb_command_buffer_release(NmbCommandBuffer *commandBuffer) {
    if (!commandBuffer) return;
    {
        std::lock_guard<std::mutex> lock(commandBuffer->lock);
        // In flight: the completion deletes it.
        if (commandBuffer->number && commandBuffer->status < 4) {commandBuffer->released = true; return;}
    }
    delete commandBuffer->open;
    delete commandBuffer;
}
int nmb_command_buffer_uses_buffer(const NmbCommandBuffer *commandBuffer, const NmbBuffer *buffer) {
    if (!commandBuffer) return -1;
    NmbCommandBuffer *c = const_cast<NmbCommandBuffer *>(commandBuffer);
    std::lock_guard<std::mutex> lock(c->lock);
    return c->number && c->status < 4 && c->uses.count(buffer) ? 1 : 0;
}
int nmb_command_buffer_uses_texture(const NmbCommandBuffer *commandBuffer, const NmbTexture *texture) {
    if (!commandBuffer || !texture) return -1;
    NmbCommandBuffer *c = const_cast<NmbCommandBuffer *>(commandBuffer);
    std::lock_guard<std::mutex> lock(c->lock);
    return c->number && c->status < 4 && (c->uses.count(texture) || (texture->import && c->uses.count(texture->import))) ? 1 : 0;
}
bool nmb_command_buffer_on_completed(NmbCommandBuffer *commandBuffer, void (*handler)(void *), void *context) {
    if (!commandBuffer || commandBuffer->number) return fail("no command buffer, or committed");
    commandBuffer->onCompleted = handler;
    commandBuffer->completedContext = context;
    return true;
}
bool nmb_command_buffer_on_scheduled(NmbCommandBuffer *commandBuffer, void (*handler)(void *), void *context) {
    if (!commandBuffer || commandBuffer->number) return fail("no command buffer, or committed");
    commandBuffer->onScheduled = handler;
    commandBuffer->scheduledContext = context;
    return true;
}
bool nmb_command_buffer_commit(NmbCommandBuffer *commandBuffer) {
    if (!commandBuffer || commandBuffer->number) return fail("no command buffer, or committed twice");
    if (commandBuffer->open) return fail("an encoder is still open");
    // The shadows it uses get the guest's current bytes.
    for (NmbBuffer *buffer : commandBuffer->reads) copyIn(buffer);
    for (NmbBuffer *buffer : commandBuffer->writes) if (!commandBuffer->reads.count(buffer)) copyIn(buffer);
    const uint64_t length = commandBuffer->commands.size();
    Staging staging(length ? length : 8);
    if (!staging.offset) return fail("the window is full");
    if (length) memcpy(staging.data(), commandBuffer->commands.data(), length);
    {
        std::lock_guard<std::mutex> callLock(gCallLock);
        flushReleasesLocked();
        {
            std::lock_guard<std::mutex> lock(gFlightLock);
            commandBuffer->number = ++gLastCommitted;
            gInFlight.insert(commandBuffer->number);
        }
        {
            std::lock_guard<std::mutex> lock(commandBuffer->lock);
            commandBuffer->status = 2;
        }
        {
            std::lock_guard<std::mutex> lock(gCompletionLock);
            gCommitted[commandBuffer->number] = commandBuffer;
        }
        const QMCommit request = {commandBuffer->queue->handle, commandBuffer->number, staging.offset, length};
        Reply reply;
        if (!sendLocked(QM_COMMIT, &request, sizeof request, reply)) {
            // Refused before the Mac committed anything: no handler runs (the frontend finishes it), and no completion will come.
            std::lock_guard<std::mutex> lock(gCompletionLock);
            gCommitted.erase(commandBuffer->number);
            std::lock_guard<std::mutex> flight(gFlightLock);
            gInFlight.erase(commandBuffer->number);
            commandBuffer->status = 5;
            commandBuffer->error = tError;
            return false;
        }
    }
    commandBuffer->commands.clear();
    commandBuffer->commands.shrink_to_fit();
    {
        std::lock_guard<std::mutex> lock(gCompletionLock);
        gToSchedule.push_back(commandBuffer);
    }
    gCompletionWake.notify_all();
    // GPU work into shadowed memory (an IOSurface) is back in it before the commit returns. The display driver copies a swapped surface
    // when the swap is submitted (NativeLegacyDisplay, offline-legacy-cpu-present) and waits for no GPU, so the render server, which
    // swaps after it commits, must find the frame finished there.
    if (!commandBuffer->writes.empty()) {
        std::unique_lock<std::mutex> lock(commandBuffer->lock);
        commandBuffer->changed.wait(lock, [&] {return commandBuffer->copiedOut;});
    }
    return true;
}
bool nmb_command_buffer_wait_scheduled(NmbCommandBuffer *commandBuffer) {
    if (!commandBuffer || !commandBuffer->number) return fail("not committed");
    std::unique_lock<std::mutex> lock(commandBuffer->lock);
    commandBuffer->changed.wait(lock, [&] {return commandBuffer->scheduled || commandBuffer->status >= 4;});
    return true;
}
int nmb_command_buffer_wait(NmbCommandBuffer *commandBuffer, uint64_t timeout_ms) {
    if (!commandBuffer || !commandBuffer->number) return 0;
    std::unique_lock<std::mutex> lock(commandBuffer->lock);
    auto done = [&] {return commandBuffer->status >= 4;};
    if (!timeout_ms) {commandBuffer->changed.wait(lock, done); return 0;}
    return commandBuffer->changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), done) ? 0 : 1;
}
uint64_t nmb_command_buffer_status(const NmbCommandBuffer *commandBuffer) {
    if (!commandBuffer) return 0;
    NmbCommandBuffer *c = const_cast<NmbCommandBuffer *>(commandBuffer);
    std::lock_guard<std::mutex> lock(c->lock);
    return c->status;
}
size_t nmb_command_buffer_error(const NmbCommandBuffer *commandBuffer, char *out, size_t size) {
    if (!commandBuffer) return 0;
    NmbCommandBuffer *c = const_cast<NmbCommandBuffer *>(commandBuffer);
    std::lock_guard<std::mutex> lock(c->lock);
    return (size_t)snprintf(out, size, "%s", c->error.c_str());
}
double nmb_command_buffer_gpu_time(const NmbCommandBuffer *commandBuffer, bool end) {
    if (!commandBuffer) return 0;
    return end ? commandBuffer->gpuEnd : commandBuffer->gpuStart;
}
bool nmb_command_buffer_signal_event(NmbCommandBuffer *commandBuffer, NmbEvent *event, uint64_t value) {
    if (!event) return fail("no event");
    const QCEvent fields = {event->handle, value};
    return emit(commandBuffer, QC_SIGNAL_EVENT, &fields, sizeof fields);
}
bool nmb_command_buffer_wait_event(NmbCommandBuffer *commandBuffer, NmbEvent *event, uint64_t value) {
    if (!event) return fail("no event");
    const QCEvent fields = {event->handle, value};
    return emit(commandBuffer, QC_WAIT_EVENT, &fields, sizeof fields);
}

// ---- encoders ----
static NmbEncoder *openEncoder(NmbCommandBuffer *commandBuffer, int kind, uint32_t op, const void *fields, size_t bytes) {
    if (!commandBuffer) return refuse<NmbEncoder>("no command buffer");
    if (commandBuffer->open) return refuse<NmbEncoder>("an encoder is still open");
    if (!emit(commandBuffer, op, fields, bytes)) return nullptr;
    NmbEncoder *encoder = new NmbEncoder{commandBuffer, kind};
    commandBuffer->open = encoder;
    return encoder;
}
NmbEncoder *nmb_compute_encoder_new(NmbCommandBuffer *commandBuffer) {return openEncoder(commandBuffer, 0, QC_COMPUTE, nullptr, 0);}
NmbEncoder *nmb_blit_encoder_new(NmbCommandBuffer *commandBuffer) {return openEncoder(commandBuffer, 2, QC_BLIT, nullptr, 0);}
NmbEncoder *nmb_render_encoder_new(NmbCommandBuffer *commandBuffer, const struct NmbRenderPassDescriptor *d) {
    if (!commandBuffer || !d) return refuse<NmbEncoder>("no command buffer or pass");
    QCRenderPass pass = {};
    auto attachment = [&](const NmbPassAttachment &in, QCAttachment &out) {
        out = {in.texture ? in.texture->handle : 0, in.level, in.slice, in.depth_plane, in.load, in.store,
               {in.clear[0], in.clear[1], in.clear[2], in.clear[3]}};
        use(commandBuffer, in.texture, true);
    };
    for (unsigned i = 0; i < 8; i++) attachment(d->colors[i], pass.colors[i]);
    attachment(d->depth, pass.depth);
    attachment(d->stencil, pass.stencil);
    pass.width = d->width;
    pass.height = d->height;
    pass.arrayLength = d->array_length;
    pass.samples = d->samples;
    pass.visibility = d->visibility ? d->visibility->offset : QM_NONE;
    use(commandBuffer, d->visibility, true);
    return openEncoder(commandBuffer, 1, QC_RENDER, &pass, sizeof pass);
}
bool nmb_encoder_end(NmbEncoder *encoder) {
    if (!encoder) return fail("no encoder");
    NmbCommandBuffer *commandBuffer = encoder->commandBuffer;
    const bool ok = emit(commandBuffer, QC_END, nullptr, 0);
    commandBuffer->open = nullptr;
    delete encoder;
    return ok;
}
void nmb_encoder_discard(NmbEncoder *encoder) {
    if (!encoder) return;
    if (!encoder->commandBuffer->number) emit(encoder->commandBuffer, QC_END, nullptr, 0);
    encoder->commandBuffer->open = nullptr;
    delete encoder;
}
bool nmb_encoder_update_fence(NmbEncoder *encoder, NmbFence *fence, uint64_t stages) {
    if (!fence) return fail("no fence");
    return emit(encoder, QC_UPDATE_FENCE, QCFence{fence->handle, stages});
}
bool nmb_encoder_wait_fence(NmbEncoder *encoder, NmbFence *fence, uint64_t stages) {
    if (!fence) return fail("no fence");
    return emit(encoder, QC_WAIT_FENCE, QCFence{fence->handle, stages});
}
bool nmb_encoder_set_buffer(NmbEncoder *encoder, uint32_t stage, NmbBuffer *buffer, uint64_t offset, uint32_t index) {
    if (!encoder || stage > 2 || index >= 31) return fail("bad buffer binding");
    encoder->bound[stage][index] = buffer;
    encoder->boundOffset[stage][index] = offset;
    // A compute function may write any buffer it is given.
    use(encoder->commandBuffer, buffer, stage == 0);
    return emit(encoder, QC_BUFFER, QCBind{stage, index, at(buffer, offset)});
}
bool nmb_encoder_set_buffer_offset(NmbEncoder *encoder, uint32_t stage, uint64_t offset, uint32_t index) {
    if (!encoder || stage > 2 || index >= 31 || !encoder->bound[stage][index]) return fail("no buffer bound there");
    encoder->boundOffset[stage][index] = offset;
    return emit(encoder, QC_BUFFER, QCBind{stage, index, at(encoder->bound[stage][index], offset)});
}
bool nmb_encoder_set_bytes(NmbEncoder *encoder, uint32_t stage, const void *bytes, size_t length, uint32_t index) {
    if (!encoder || stage > 2 || index >= 31 || !bytes) return fail("bad bytes binding");
    encoder->bound[stage][index] = nullptr;
    const QCBind fields = {stage, index, length};
    return emit(encoder->commandBuffer, QC_BYTES, &fields, sizeof fields, bytes, length);
}
bool nmb_encoder_set_texture(NmbEncoder *encoder, uint32_t stage, NmbTexture *texture, uint32_t index) {
    if (!encoder || stage > 2) return fail("bad texture binding");
    use(encoder->commandBuffer, texture, stage == 0);
    return emit(encoder, QC_TEXTURE, QCBind{stage, index, texture ? texture->handle : 0});
}
bool nmb_encoder_set_sampler(NmbEncoder *encoder, uint32_t stage, NmbSampler *sampler, uint32_t index) {
    if (!encoder || stage > 2) return fail("bad sampler binding");
    return emit(encoder, QC_SAMPLER, QCBind{stage, index, sampler ? sampler->handle : 0});
}
bool nmb_compute_set_pipeline(NmbEncoder *encoder, NmbComputePipeline *pipeline) {
    if (!pipeline) return fail("no pipeline");
    return emit(encoder, QC_COMPUTE_PIPELINE, pipeline->handle);
}
bool nmb_compute_set_threadgroup_memory(NmbEncoder *encoder, uint32_t length, uint32_t index) {
    const uint64_t fields[2] = {length, index};
    return encoder && emit(encoder->commandBuffer, QC_THREADGROUP_MEMORY, fields, sizeof fields);
}
bool nmb_compute_dispatch(NmbEncoder *encoder, const uint64_t size[3], const uint64_t per_group[3], bool threads) {
    QCDispatch fields = {{size[0], size[1], size[2]}, {per_group[0], per_group[1], per_group[2]}, threads};
    return emit(encoder, QC_DISPATCH, fields);
}
bool nmb_render_set_pipeline(NmbEncoder *encoder, NmbRenderPipeline *pipeline) {
    if (!pipeline) return fail("no pipeline");
    return emit(encoder, QC_RENDER_PIPELINE, pipeline->handle);
}
bool nmb_render_set_depth_stencil(NmbEncoder *encoder, NmbDepthStencil *state) {
    return emit(encoder, QC_DEPTH_STENCIL, state ? state->handle : 0);
}
bool nmb_render_set_viewport(NmbEncoder *encoder, const double viewport[6]) {
    return encoder && emit(encoder->commandBuffer, QC_VIEWPORT, viewport, 48);
}
bool nmb_render_set_scissor(NmbEncoder *encoder, const uint64_t rect[4]) {
    return encoder && emit(encoder->commandBuffer, QC_SCISSOR, rect, 32);
}
bool nmb_render_set_raster(NmbEncoder *encoder, uint64_t cull_mode, uint64_t winding, uint64_t fill_mode) {
    const uint64_t fields[3] = {cull_mode, winding, fill_mode};
    return encoder && emit(encoder->commandBuffer, QC_RASTER, fields, sizeof fields);
}
bool nmb_render_set_stencil_reference(NmbEncoder *encoder, uint32_t front, uint32_t back) {
    const uint64_t fields[2] = {front, back};
    return encoder && emit(encoder->commandBuffer, QC_STENCIL_REFERENCE, fields, sizeof fields);
}
bool nmb_render_set_blend_color(NmbEncoder *encoder, const float color[4]) {
    const double fields[4] = {color[0], color[1], color[2], color[3]};
    return encoder && emit(encoder->commandBuffer, QC_BLEND_COLOR, fields, sizeof fields);
}
bool nmb_render_set_depth_bias(NmbEncoder *encoder, float bias, float slope_scale, float clamp) {
    const double fields[3] = {bias, slope_scale, clamp};
    return encoder && emit(encoder->commandBuffer, QC_DEPTH_BIAS, fields, sizeof fields);
}
bool nmb_render_set_line_width(NmbEncoder *encoder, float width) {
    // Metal draws lines one pixel wide; the width is Apple's SPI and has no effect on its GPUs either.
    (void)width;
    return encoder != nullptr || fail("no encoder");
}
bool nmb_render_set_visibility_mode(NmbEncoder *encoder, uint64_t mode, uint64_t offset) {
    const uint64_t fields[2] = {mode, offset};
    return encoder && emit(encoder->commandBuffer, QC_VISIBILITY, fields, sizeof fields);
}
bool nmb_render_draw(NmbEncoder *encoder, uint64_t primitive, uint32_t start, uint32_t count, uint32_t instances, uint32_t base_instance) {
    return emit(encoder, QC_DRAW, QCDraw{primitive, start, count, instances, base_instance});
}
bool nmb_render_draw_indexed(NmbEncoder *encoder, uint64_t primitive, uint32_t count, uint64_t index_type, NmbBuffer *indices, uint64_t offset,
                             uint32_t instances, int32_t base_vertex, uint32_t base_instance) {
    if (!encoder || !indices) return fail("no index buffer");
    use(encoder->commandBuffer, indices, false);
    return emit(encoder, QC_DRAW_INDEXED, QCDrawIndexed{primitive, count, index_type, at(indices, offset), instances, base_vertex, base_instance});
}
bool nmb_blit_copy_buffer(NmbEncoder *encoder, NmbBuffer *source, uint64_t source_offset, NmbBuffer *destination, uint64_t destination_offset,
                          uint64_t size) {
    if (!encoder || !source || !destination) return fail("no buffer");
    use(encoder->commandBuffer, source, false);
    use(encoder->commandBuffer, destination, true);
    const uint64_t fields[3] = {at(source, source_offset), at(destination, destination_offset), size};
    return emit(encoder->commandBuffer, QC_COPY_BUFFER, fields, sizeof fields);
}
bool nmb_blit_fill_buffer(NmbEncoder *encoder, NmbBuffer *buffer, uint64_t start, uint64_t end, uint8_t value) {
    if (!encoder || !buffer || end < start) return fail("no buffer");
    use(encoder->commandBuffer, buffer, true);
    const uint64_t fields[3] = {at(buffer, start), end - start, value};
    return emit(encoder->commandBuffer, QC_FILL_BUFFER, fields, sizeof fields);
}
bool nmb_blit_copy_texture(NmbEncoder *encoder, NmbTexture *source, uint64_t source_slice, uint64_t source_level, const uint64_t source_origin[3],
                           const uint64_t size[3], NmbTexture *destination, uint64_t destination_slice, uint64_t destination_level,
                           const uint64_t destination_origin[3]) {
    if (!encoder || !source || !destination) return fail("no texture");
    use(encoder->commandBuffer, source, false);
    use(encoder->commandBuffer, destination, true);
    QCCopyTexture fields = {source->handle, source_slice, source_level, {}, {}, destination->handle, destination_slice, destination_level, {}};
    memcpy(fields.sourceOrigin, source_origin, 24);
    memcpy(fields.size, size, 24);
    memcpy(fields.destinationOrigin, destination_origin, 24);
    return emit(encoder, QC_COPY_TEXTURE, fields);
}
static bool bufferTexture(NmbEncoder *encoder, uint32_t op, NmbBuffer *buffer, uint64_t offset, uint64_t bytesPerRow, uint64_t bytesPerImage,
                          const uint64_t size[3], NmbTexture *texture, uint64_t slice, uint64_t level, const uint64_t origin[3]) {
    if (!encoder || !buffer || !texture) return fail("no buffer or texture");
    use(encoder->commandBuffer, buffer, op == QC_TEXTURE_TO_BUFFER);
    use(encoder->commandBuffer, texture, op == QC_BUFFER_TO_TEXTURE);
    QCBufferTexture fields = {at(buffer, offset), bytesPerRow, bytesPerImage, {}, texture->handle, slice, level, {}};
    memcpy(fields.size, size, 24);
    memcpy(fields.origin, origin, 24);
    return emit(encoder, op, fields);
}
bool nmb_blit_buffer_to_texture(NmbEncoder *encoder, NmbBuffer *source, uint64_t offset, uint64_t bytes_per_row, uint64_t bytes_per_image,
                                const uint64_t size[3], NmbTexture *destination, uint64_t slice, uint64_t level, const uint64_t origin[3]) {
    return bufferTexture(encoder, QC_BUFFER_TO_TEXTURE, source, offset, bytes_per_row, bytes_per_image, size, destination, slice, level, origin);
}
bool nmb_blit_texture_to_buffer(NmbEncoder *encoder, NmbTexture *source, uint64_t slice, uint64_t level, const uint64_t origin[3],
                                const uint64_t size[3], NmbBuffer *destination, uint64_t offset, uint64_t bytes_per_row, uint64_t bytes_per_image) {
    return bufferTexture(encoder, QC_TEXTURE_TO_BUFFER, destination, offset, bytes_per_row, bytes_per_image, size, source, slice, level, origin);
}
bool nmb_blit_generate_mipmaps(NmbEncoder *encoder, NmbTexture *texture) {
    if (!encoder || !texture) return fail("no texture");
    use(encoder->commandBuffer, texture, true);
    return emit(encoder, QC_GENERATE_MIPMAPS, texture->handle);
}
}  // extern "C"
