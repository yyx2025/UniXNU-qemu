// SPDX-License-Identifier: BSD-2-Clause-Patent
// The memory window that run-qemu.py --shared-window shares with a process on the Mac: guest physical memory that QEMU
// maps from a file, outside DarwinBoot's window, so the kernel never manages it. A Metal device whose work runs on the
// Mac will hand it buffers and IOSurfaces through this window; for now it only proves the two sides see each other.
//
// run-qemu.py names the window in the fw_cfg item opt/org.unixnu/shared-window as "BASE:SIZE:MODE" (hexadecimal, MODE
// "wb" for write-back cacheable or "device"); without the item nothing happens. The kernel maps the window and writes
// the mailbox at its start; every 1 ms it answers a new host request: the sum of the host's 64-bit words at 1 MiB and
// a block of its own after them, so scripts/guest-ram.py can check both directions and time the round trip.
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
boolean_t pmap_valid_address(uint64_t addr);
uint64_t mach_absolute_time(void);
volatile uint64_t native_shared_window_base, native_shared_window_size, native_shared_window_answers;
volatile uint32_t native_shared_window_stage;
void native_shared_window_probe(void);
bool native_fw_cfg_read(const char *name, char *text, size_t capacity);
}

namespace {
constexpr uintptr_t kFwCfg = 0x09020000;                   // QEMU virt: data +0, selector +8 (big-endian)
constexpr uint16_t kFwCfgSignature = 0x0000, kFwCfgFileDir = 0x0019;
constexpr uint64_t kGuestMagic = 0x314e495753584e55;      // "UNXSWIN1"
constexpr uint64_t kBlocks = 1 << 20;                     // the host's block, then the guest's, start at 1 MiB

struct Mailbox {
    uint64_t magic;            // guest: kGuestMagic once mapped
    uint32_t mode;             // guest: 1 write-back, 2 device
    uint32_t ticks;            // guest: one more every poll
    uint64_t request;          // host: a new number asks for one round
    uint64_t answered;         // guest: the last request handled
    uint64_t length;           // host: bytes in each block, a multiple of 8
    uint64_t sum;              // guest: the sum of the host block's 64-bit words
    uint64_t seed;             // host: the guest block holds seed + index
    uint64_t busy;             // guest: mach_absolute_time ticks the round took
};
static_assert(sizeof(Mailbox) == 64, "mailbox layout of scripts/guest-ram.py");

// Plain loads and stores: fw_cfg is emulated by QEMU, which needs the fault syndrome of the access under HVF.
uint8_t fwRead8(uintptr_t base) { uint32_t value; __asm__ volatile("ldrb %w0, [%1]" : "=r"(value) : "r"(base) : "memory"); return (uint8_t)value; }
void fwSelect(uintptr_t base, uint16_t item) {
    __asm__ volatile("strh %w0, [%1]\n\tdsb sy" :: "r"((uint32_t)__builtin_bswap16(item)), "r"(base + 8) : "memory");
}
void fwRead(uintptr_t base, void *out, size_t length) {
    for (size_t i = 0; i < length; i++) static_cast<uint8_t *>(out)[i] = fwRead8(base);
}

uint64_t hex(const char *&text) {
    uint64_t value = 0;
    for (;; text++) {
        char c = *text;
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return value;
        value = value << 4 | (uint64_t)digit;
    }
}

}

// The text of the fw_cfg item, or false: the signature must be QEMU's, as on QEMU virt. Also for QemuMetalPublish.cpp.
bool native_fw_cfg_read(const char *name, char *text, size_t capacity) {
    static uintptr_t base;
    if (!base) base = ml_io_map(kFwCfg, 0x1000);
    if (!base) return false;
    char signature[4];
    fwSelect(base, kFwCfgSignature); fwRead(base, signature, 4);
    if (memcmp(signature, "QEMU", 4)) return false;
    uint32_t count;
    fwSelect(base, kFwCfgFileDir); fwRead(base, &count, 4);
    count = __builtin_bswap32(count);
    struct { uint32_t size; uint16_t select, reserved; char name[56]; } file;
    for (uint32_t i = 0; i < count && i < 256; i++) {
        fwRead(base, &file, sizeof(file));
        if (strncmp(file.name, name, sizeof(file.name))) continue;
        size_t size = __builtin_bswap32(file.size);
        if (!size || size >= capacity) return false;
        fwSelect(base, __builtin_bswap16(file.select)); fwRead(base, text, size);
        text[size] = 0;
        return true;
    }
    return false;
}

namespace {

IOMemoryMap *mapping;
volatile Mailbox *mailbox;
volatile uint64_t *words;
uint64_t windowSize;
IOWorkLoop *loop;
IOTimerEventSource *timer;

void tick(OSObject *, IOTimerEventSource *source) {
    mailbox->ticks = mailbox->ticks + 1;
    uint64_t request = mailbox->request;
    if (request != mailbox->answered) {
        __asm__ volatile("dmb ish" ::: "memory");
        uint64_t start = mach_absolute_time(), length = mailbox->length, sum = 0, seed = mailbox->seed;
        if (length % 8 == 0 && kBlocks + 2 * length <= windowSize) {
            const uint64_t count = length / 8;
            for (uint64_t i = 0; i < count; i++) sum += words[i];
            for (uint64_t i = 0; i < count; i++) words[count + i] = seed + i;
        }
        mailbox->sum = sum;
        mailbox->busy = mach_absolute_time() - start;
        __asm__ volatile("dsb sy" ::: "memory");
        mailbox->answered = request;
        native_shared_window_answers = native_shared_window_answers + 1;
    }
    source->setTimeoutMS(1);
}
}

void native_shared_window_probe(void) {
    if (loop) return;
    char text[64];
    if (!native_fw_cfg_read("opt/org.unixnu/shared-window", text, sizeof(text))) return;
    native_shared_window_stage = 1;
    const char *cursor = text;
    uint64_t base = hex(cursor);
    if (*cursor++ != ':') return;
    uint64_t size = hex(cursor);
    if (*cursor++ != ':') return;
    bool writeBack = !strcmp(cursor, "wb");
    if (!writeBack && strcmp(cursor, "device")) return;
    // Page aligned, room for both blocks, and memory the kernel does not manage.
    if ((base | size) & 0x3fff || size < 2 * kBlocks || pmap_valid_address(base) || pmap_valid_address(base + size - 1)) return;
    native_shared_window_base = base; native_shared_window_size = size; native_shared_window_stage = 2;
    IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryMapperNone);
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(base, size, options, TASK_NULL);
    mapping = memory ? memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | (writeBack ? kIOMapCopybackCache : kIOMapInhibitCache)) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!mapping) return;
    native_shared_window_stage = 3;
    mailbox = reinterpret_cast<volatile Mailbox *>(mapping->getVirtualAddress());
    words = reinterpret_cast<volatile uint64_t *>(mapping->getVirtualAddress() + kBlocks);
    windowSize = size;
    mailbox->mode = writeBack ? 1 : 2;
    mailbox->answered = mailbox->request;
    __asm__ volatile("dsb sy" ::: "memory");
    mailbox->magic = kGuestMagic;
    loop = IOWorkLoop::workLoop();
    timer = loop ? IOTimerEventSource::timerEventSource(loop, &tick) : nullptr;
    if (timer && loop->addEventSource(timer) == kIOReturnSuccess) { timer->setTimeoutMS(1); native_shared_window_stage = 4; }
}
