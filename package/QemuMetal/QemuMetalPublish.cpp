// SPDX-License-Identifier: BSD-2-Clause-Patent
// Publishes QemuMetalAccelerator.cpp's accelerator when run-qemu.py --shared-window gave the guest a window (SharedWindow.cpp), from the
// late probe, before launchd: IOGPUFamily is loaded (prelinked, but nothing else loads it), the staged classes are registered under it
// and the accelerator is attached to the platform with the keys the original Metal discovery reads, as sheng's publishMetal
// (UniXNU-sheng/package/GpuPowerProbe.cpp). Then a grant for each other process run-qemu.py names (QemuMetalAccelerator.cpp).
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>

extern "C" {
extern volatile uint32_t native_shared_window_stage;
extern kmod_info_t g_kernel_kmod_info;
volatile uint32_t native_qemu_metal_stage, native_qemu_metal_grants;
volatile int native_qemu_metal_status;
void native_qemu_metal_static_init(void) __asm__("__GLOBAL__sub_I_QemuMetalAccelerator.cpp");
IOService *native_qemu_metal_accelerator_allocate(void);
unsigned native_qemu_metal_accelerator_instance_size(void);
IOService *native_qemu_metal_grant_allocate(void);
bool native_fw_cfg_read(const char *name, char *text, size_t capacity);
void native_qemu_metal_publish(void);
}

// Stages: 1 IOGPUFamily load asked, 2 its IOAcceleratorES found, 3 own classes registered, 4 published.
void native_qemu_metal_publish(void)
{
    if (native_qemu_metal_stage || native_shared_window_stage < 4) return;
    native_qemu_metal_stage = 1;
    native_qemu_metal_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (native_qemu_metal_status) return;
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136) {native_qemu_metal_status = kIOReturnNotFound; return;}
    native_qemu_metal_stage = 2;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {native_qemu_metal_status = kIOReturnNoResources; return;}
    native_qemu_metal_static_init();
    native_qemu_metal_status = OSMetaClass::postModLoad(handle);
    if (native_qemu_metal_status) return;
    if (native_qemu_metal_accelerator_instance_size() != 136) {native_qemu_metal_status = kIOReturnInvalid; return;}
    native_qemu_metal_stage = 3;
    IOService *accelerator = native_qemu_metal_accelerator_allocate();
    if (!accelerator || !accelerator->init()) {OSSafeReleaseNULL(accelerator); native_qemu_metal_status = kIOReturnNoMemory; return;}
    // The keys the original _MTLRegisterDevices reads; the bundle exists through the addon mount (zzy-native-gpu-addon-mount.patch).
    accelerator->setProperty("MetalPluginName", "QemuMetal");
    accelerator->setProperty("MetalPluginClassName", "QemuMetalDevice");
    // The one process whose open of the accelerator and its client the kernel grants in place of the sandbox verdict
    // (zzw-native-adreno-sandbox-grant.patch): the render server.
    accelerator->setProperty("NativeSandboxGrantProcess", "backboardd");
    if (!accelerator->attach(IOService::getPlatform())) {accelerator->release(); native_qemu_metal_status = kIOReturnNotAttached; return;}
    accelerator->registerService();
    accelerator->release();
    native_qemu_metal_stage = 4;
    // A grant for each other process that gets a device (run-qemu.py --metal-clients: names separated by commas).
    char clients[256];
    if (!native_fw_cfg_read("opt/org.unixnu/metal-clients", clients, sizeof clients)) return;
    for (char *name = clients, *next; *name; name = next) {
        for (next = name; *next && *next != ','; next++) {}
        if (*next) *next++ = 0;
        if (!*name) continue;
        IOService *grant = native_qemu_metal_grant_allocate();
        if (!grant || !grant->init()) {OSSafeReleaseNULL(grant); continue;}
        grant->setProperty("NativeSandboxGrantProcess", name);
        if (grant->attach(IOService::getPlatform())) {
            grant->registerService();
            native_qemu_metal_grants = native_qemu_metal_grants + 1;
        }
        grant->release();
    }
}
