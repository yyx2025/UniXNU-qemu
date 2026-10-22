// SPDX-License-Identifier: BSD-2-Clause-Patent
// The accelerator iPadOS's Metal discovery finds (IOServiceMatching("IOAcceleratorES") -> MetalPluginName ->
// /System/Library/Extensions/QemuMetal.bundle -> initWithAcceleratorPort:), and the connection that gives the plugin its window to the
// Mac: the memory of run-qemu.py --shared-window that SharedWindow.cpp found, which QemuMetalRemote.cpp and qemu-metal-host share.
//
// As sheng's Adreno accelerator (drivers/NativeAdreno/NativeAdrenoAccelerator.cpp), the class derives from the original IOAcceleratorES
// of IOGPUFamily (drivers/NativeMetal/OriginalAcceleratorABI.h, bound to the audited original addresses by
// build-native-legacy-display.py), and is named after Apple's accelerator class. Its static initializer is taken out of __mod_init_func
// and run by QemuMetalPublish.cpp once IOGPUFamily has loaded, since the parent metaclass exists only then. The open grant of backboardd
// is the accelerator's NativeSandboxGrantProcess property (patches/xnu/zzw-native-adreno-sandbox-grant.patch); every other process opens
// it under its own sandbox profile, which admits Metal's connection class: the connection is named AGXDeviceUserClient for that, as sheng's.
//
// Each connection owns one slot of the window (QemuMetalWire.h qm_slot), its own memory to the Mac: the render server slot 0, any other
// process the first free one of the rest. Open type kOpenType; memory type 0 is the slot, which the plugin maps write-back cacheable, as
// the kernel and the Mac do.
#include "OriginalAcceleratorABI.h"
#include "QemuMetalWire.h"
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <libkern/OSAtomic.h>
#include <kern/task.h>

extern "C" {
extern volatile uint64_t native_shared_window_base, native_shared_window_size;
extern volatile uint32_t native_shared_window_stage;
volatile uint32_t native_qemu_metal_opens, native_qemu_metal_refusals;
volatile char native_qemu_metal_owners[QM_SLOTS][17];   // the process of each slot, for qemu-ps.py --value
IOService *native_qemu_metal_accelerator_allocate(void);
unsigned native_qemu_metal_accelerator_instance_size(void);
IOService *native_qemu_metal_grant_allocate(void);
void proc_selfname(char *buffer, int size);
}

namespace {
constexpr UInt32 kOpenType = 0x514d;          // 'QM'
volatile UInt8 gSlots[QM_SLOTS];
}

class AGXAccelerator : public IOAcceleratorES {
    OSDeclareDefaultStructors(AGXAccelerator)
public:
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AGXAccelerator, IOAcceleratorES)

class AGXDeviceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AGXDeviceUserClient)
    int slot = -1;
    void giveBack();
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AGXDeviceUserClient, IOUserClient)

// A process besides the render server opens a grant of its own instead of the accelerator: the kernel's open path grants a service's
// connection in place of the sandbox verdict to the one process its NativeSandboxGrantProcess names (zzw-native-adreno-sandbox-grant.patch),
// and the profiles of processes such as iconservicesagent refuse AGXDeviceUserClient. No IOAcceleratorES: the discovery never sees it.
// QemuMetalPublish.cpp publishes one for each name run-qemu.py --metal-clients gives.
class QemuMetalGrant : public IOService {
    OSDeclareDefaultStructors(QemuMetalGrant)
public:
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(QemuMetalGrant, IOService)

static IOReturn openClient(IOService *provider, task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)
{
    if (type != kOpenType || !handler) {native_qemu_metal_refusals = native_qemu_metal_refusals + 1; return kIOReturnBadArgument;}
    AGXDeviceUserClient *client = new AGXDeviceUserClient;
    if (!client || !client->initWithTask(owningTask, securityID, type) || !client->attach(provider)) {
        OSSafeReleaseNULL(client);
        return kIOReturnNoMemory;
    }
    if (!client->start(provider)) {
        client->detach(provider);
        client->release();
        native_qemu_metal_refusals = native_qemu_metal_refusals + 1;
        return kIOReturnExclusiveAccess;
    }
    native_qemu_metal_opens = native_qemu_metal_opens + 1;
    *handler = client;
    return kIOReturnSuccess;
}
IOReturn AGXAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
    (void)properties;
    return openClient(this, owningTask, securityID, type, handler);
}
IOReturn QemuMetalGrant::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
    (void)properties;
    return openClient(this, owningTask, securityID, type, handler);
}

bool AGXDeviceUserClient::start(IOService *provider)
{
    if (!IOUserClient::start(provider)) return false;
    // A slot has one heap allocator, in its owner's process. The opener is the calling process.
    char name[17] = {};
    proc_selfname(name, sizeof name);
    const bool server = !strncmp(name, "backboardd", sizeof name);
    for (unsigned i = server ? 0 : 1; i < (server ? 1u : QM_SLOTS); i++) {
        if (!OSCompareAndSwap8(0, 1, &gSlots[i])) continue;
        slot = (int)i;
        for (unsigned c = 0; c < sizeof name; c++) native_qemu_metal_owners[i][c] = name[c];
        return true;
    }
    return false;
}
void AGXDeviceUserClient::giveBack()
{
    if (slot < 0) return;
    native_qemu_metal_owners[slot][0] = 0;
    gSlots[slot] = 0;
    slot = -1;
}
void AGXDeviceUserClient::stop(IOService *provider)
{
    giveBack();
    IOUserClient::stop(provider);
}
IOReturn AGXDeviceUserClient::clientClose()
{
    giveBack();
    terminate();
    return kIOReturnSuccess;
}
IOReturn AGXDeviceUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
    if (type != 0 || slot < 0 || !memory || native_shared_window_stage < 2) return kIOReturnBadArgument;
    uint64_t offset = 0, bytes = 0;
    qm_slot(native_shared_window_size, (unsigned)slot, &offset, &bytes);
    const IOOptionBits direction = static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryMapperNone);
    *memory = IOMemoryDescriptor::withAddressRange(native_shared_window_base + offset, bytes, direction, TASK_NULL);
    if (options) *options = 0;
    return *memory ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOService *native_qemu_metal_accelerator_allocate(void) {return new AGXAccelerator;}
unsigned native_qemu_metal_accelerator_instance_size(void) {return sizeof(AGXAccelerator);}
IOService *native_qemu_metal_grant_allocate(void) {return new QemuMetalGrant;}
