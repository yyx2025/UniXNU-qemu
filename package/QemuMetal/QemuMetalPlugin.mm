// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Metal plugin the original discovery loads from /System/Library/Extensions/QemuMetal.bundle (the addon mount) for
// QemuMetalAccelerator.cpp's accelerator: IOServiceMatching("IOAcceleratorES") -> MetalPluginName -> MetalPluginClassName ->
// initWithAcceleratorPort:. In the render server (backboardd) it opens the accelerator, maps its slot of the window to the Mac write-back
// cacheable, and returns NativeMetal's frontend over QemuMetalRemote.cpp, which the discovery registers; CARenderServer then renders the
// system UI on the Mac's GPU. A process the kernel published a grant for (QemuMetalGrant, run-qemu.py --metal-clients) opens that grant
// instead, which its sandbox would otherwise refuse, and gets a device of its own the same way, in a slot of its own; every other process
// gets nil and keeps the software renderer.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>
#include "NativeBridgeMetalFrontend.h"

extern "C" void qemu_metal_set_window(void *base, uint64_t bytes);
extern "C" void qemu_metal_add_libraries(Class device);
extern "C" void qemu_metal_answer_gaps(Class cls);
extern "C" void qemu_metal_complete_frontend(void *device);
extern "C" void qemu_metal_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

@interface _MTLDevice : NSObject
@end
@interface QemuMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end

namespace {
constexpr uint32_t kOpenType = 0x514d;          // QemuMetalAccelerator.cpp
constexpr uint32_t kMapAnywhere = 0x1, kMapCopybackCache = 0x300;
using OpenFn = kern_return_t (*)(mach_port_t, task_port_t, uint32_t, mach_port_t *);
using MapFn = kern_return_t (*)(mach_port_t, uint32_t, task_port_t, mach_vm_address_t *, mach_vm_size_t *, uint32_t);
using RetainFn = kern_return_t (*)(mach_port_t);
using EntryFn = kern_return_t (*)(mach_port_t, uint64_t *);

void missing(const char *cls, const char *selector) {qemu_metal_log("missing selector: %s %s", cls, selector);}

// The service this process opens: the accelerator for the render server, the grant that names this process for any other (the kernel
// compares the first 16 characters of the name, as proc_selfname gives them), or none.
mach_port_t service(void *iokit, unsigned accelerator) {
    const char *self = getprogname();
    if (!strcmp(self, "backboardd")) return accelerator;
    // IOKit's objects are Mach ports (io_object_t, io_iterator_t).
    auto matching = (CFMutableDictionaryRef (*)(const char *))dlsym(iokit, "IOServiceMatching");
    auto services = (kern_return_t (*)(mach_port_t, CFDictionaryRef, mach_port_t *))dlsym(iokit, "IOServiceGetMatchingServices");
    auto next = (mach_port_t (*)(mach_port_t))dlsym(iokit, "IOIteratorNext");
    auto property = (CFTypeRef (*)(mach_port_t, CFStringRef, CFAllocatorRef, uint32_t))dlsym(iokit, "IORegistryEntryCreateCFProperty");
    auto release = (kern_return_t (*)(mach_port_t))dlsym(iokit, "IOObjectRelease");
    mach_port_t iterator = MACH_PORT_NULL;
    if (!matching || !services || !next || !property || !release || services(0, matching("QemuMetalGrant"), &iterator) != KERN_SUCCESS)
        return MACH_PORT_NULL;
    char name[17] = {};
    strlcpy(name, self, sizeof name);
    mach_port_t found = MACH_PORT_NULL;
    while (mach_port_t grant = next(iterator)) {
        char granted[64] = {};
        if (CFTypeRef value = property(grant, CFSTR("NativeSandboxGrantProcess"), kCFAllocatorDefault, 0)) {
            if (CFGetTypeID(value) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)value, granted, sizeof granted, kCFStringEncodingUTF8);
            CFRelease(value);
        }
        if (!found && !strncmp(granted, name, 16)) found = grant;
        else release(grant);
    }
    release(iterator);
    return found;
}

id open(unsigned port) {
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    auto serviceOpen = iokit ? (OpenFn)dlsym(iokit, "IOServiceOpen") : nullptr;
    auto map = iokit ? (MapFn)dlsym(iokit, "IOConnectMapMemory64") : nullptr;
    auto retain = iokit ? (RetainFn)dlsym(iokit, "IOObjectRetain") : nullptr;
    auto entryID = iokit ? (EntryFn)dlsym(iokit, "IORegistryEntryGetRegistryEntryID") : nullptr;
    if (!serviceOpen || !map || !retain || !entryID) return nil;
    const mach_port_t target = service(iokit, port);
    if (!target) return nil;
    // The discovery releases the accelerator port once this returns; the connection keeps the service.
    retain(port);
    mach_port_t connection = MACH_PORT_NULL;
    if (serviceOpen(target, mach_task_self(), kOpenType, &connection) != KERN_SUCCESS) return nil;
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    if (map(connection, 0, mach_task_self(), &address, &size, kMapAnywhere | kMapCopybackCache) != KERN_SUCCESS || !address) return nil;
    qemu_metal_set_window(reinterpret_cast<void *>(address), size);
    qemu_metal_log("%s[%d]: window of %llu MiB at %llx", getprogname(), getpid(), (unsigned long long)(size >> 20), (unsigned long long)address);
    uint64_t registryID = 0;
    entryID(port, &registryID);
    // The display driver takes plain 8-bit rows (NativeLegacyDisplay): no compressed or 10-bit framebuffer, as on sheng
    // (NativeAdrenoMetalPlugin.mm); the Data seed carries the same preferences for the process's start (build-qemu-metal.py seed).
    setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
    CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
    id device = nil;
    @try {
        device = native_bridge_metal_open(nullptr, nullptr, registryID, missing);
    } @catch (NSException *exception) {
        qemu_metal_log("device refused: %s", exception.reason.UTF8String ?: "?");
        return nil;
    }
    if (!device) {qemu_metal_log("no device"); return nil;}
    qemu_metal_complete_frontend((__bridge void *)device);
    qemu_metal_add_libraries(object_getClass(device));
    qemu_metal_answer_gaps(object_getClass(device));
    if (Class objects = objc_getClass("NBObject")) qemu_metal_answer_gaps(objects);
    qemu_metal_log("device %s registered for %s", [device name].UTF8String, getprogname());
    // The frontend's counters and its last refusals, every ten seconds, into the Mac's log.
    pthread_t monitor;
    pthread_create(&monitor, nullptr, [](void *) -> void * {
        pthread_setname_np("qemu-metal status");
        for (;;) {
            sleep(10);
            char status[512];
            native_bridge_metal_status(status, sizeof status);
            qemu_metal_log("status: %s", status);
        }
        return nullptr;
    }, nullptr);
    pthread_detach(monitor);
    return device;
}
}

@implementation QemuMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port {
    self = [super init];
    if (!self || !port) return nil;
    static id device;
    static bool tried;
    @synchronized([QemuMetalDevice class]) {
        if (!tried) {
            tried = true;
            device = open(port);
        }
        return device;
    }
}
@end
