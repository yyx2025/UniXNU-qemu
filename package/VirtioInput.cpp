// SPDX-License-Identifier: BSD-2-Clause-Patent
// QEMU's VirtIO tablet and keyboard as the touch screen and the Home button of NativeHidInput. OASIS VirtIO 1.2
// sections 4.2 (MMIO transport) and 5.8 (input device); the events are Linux evdev (type, code, value).
//
// The tablet's left button down is one finger at the pointer, its motion moves the finger, the button up lifts it: the
// finger set of TouchState.cpp, which NativeHidInput reports as each frame is published. On the keyboard, Home and F1 press Home through
// NativeHidInput's command word. Every input device found is polled every 8 ms (the event queue asks for no interrupts).
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <kern/task.h>

extern "C" {
extern volatile uint32_t native_touch_seq, native_touch_count;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
extern volatile uint32_t native_hid_input_command;
void native_touch_published(void);
volatile uint32_t native_virtio_input_devices, native_virtio_input_events, native_virtio_input_touches,
    native_virtio_input_keys, native_virtio_input_failures;
void native_virtio_input_probe(void);
}

namespace {
constexpr unsigned kQueueSize = 64;
constexpr uint32_t kDeviceInput = 18;
constexpr uint16_t kEvSyn = 0, kEvKey = 1, kEvAbs = 3;
constexpr uint16_t kAbsX = 0, kAbsY = 1, kBtnLeft = 0x110, kBtnTouch = 0x14a, kKeyHome = 102, kKeyF1 = 59;
constexpr int kTouchWidth = 30480, kTouchHeight = 20320;   // the axes of TouchState.cpp's finger set

struct Descriptor { uint64_t address; uint32_t length; uint16_t flags, next; };
struct Used { uint32_t id, length; };
struct Event { uint16_t type, code; uint32_t value; };
static_assert(sizeof(Descriptor) == 16 && sizeof(Used) == 8 && sizeof(Event) == 8, "VirtIO wire sizes");
inline void dmaBarrier() { __asm__ volatile("dmb osh" ::: "memory"); }
inline uint32_t mmioRead32(uintptr_t address) {
    uint32_t value;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
    return value;
}

struct Device {
    IOMemoryMap *mapping = nullptr;
    uintptr_t registers = 0;
    IOBufferMemoryDescriptor *ring = nullptr, *events = nullptr;
    uint16_t available = 0, used = 0;
    uint32_t maxX = 0x7fff, maxY = 0x7fff;
    uint32_t x = 0, y = 0;
    bool button = false, touching = false;

    // Plain loads and stores only: under HVF, QEMU emulates a register access from the fault syndrome, which a load
    // with writeback or a pair does not carry (it aborts on "isv"), and the compiler may emit those for volatile accesses.
    uint32_t read32(unsigned at) const { return mmioRead32(registers + at); }
    uint8_t read8(unsigned at) const { uint32_t value; __asm__ volatile("ldrb %w0, [%1]" : "=r"(value) : "r"(registers + at) : "memory"); return (uint8_t)value; }
    void write8(unsigned at, uint8_t value) { __asm__ volatile("strb %w0, [%1]\n\tdsb oshst" :: "r"((uint32_t)value), "r"(registers + at) : "memory"); }
    void write32(unsigned at, uint32_t value) { __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(value), "r"(registers + at) : "memory"); }
    void write64(unsigned at, uint64_t value) { write32(at, (uint32_t)value); write32(at + 4, (uint32_t)(value >> 32)); }
    unsigned char *memory() const { return static_cast<unsigned char *>(ring->getBytesNoCopy()); }
    volatile uint16_t *availableRing() const { return reinterpret_cast<volatile uint16_t *>(memory() + 1024); }
    volatile uint16_t *usedHeader() const { return reinterpret_cast<volatile uint16_t *>(memory() + 2048); }
    volatile Used *usedRing() const { return reinterpret_cast<volatile Used *>(memory() + 2052); }

    // The maximum of an absolute axis (VIRTIO_INPUT_CFG_ABS_INFO); QEMU's tablet reports 0..0x7fff.
    uint32_t axisMaximum(uint8_t axis, uint32_t fallback) {
        write8(0x100, 0x12); write8(0x101, axis);
        if (read8(0x102) < 8) return fallback;
        uint32_t maximum = read32(0x10c);
        return maximum ? maximum : fallback;
    }
    bool start() {
        write32(0x70, 0);
        for (unsigned i = 0; read32(0x70) && i < 1000; i++) IODelay(10);
        write32(0x70, 1); write32(0x70, 3);
        write32(0x14, 1); if (!(read32(0x10) & 1)) return false;          // VIRTIO_F_VERSION_1
        write32(0x24, 0); write32(0x20, 0); write32(0x24, 1); write32(0x20, 1);
        write32(0x70, 11); if ((read32(0x70) & (8 | 64 | 128)) != 8) return false;
        maxX = axisMaximum(kAbsX, 0x7fff); maxY = axisMaximum(kAbsY, 0x7fff);
        write32(0x30, 0);                                                  // the event queue
        if (read32(0x44) || read32(0x34) < kQueueSize) return false;
        IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
        ring = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, options, 4096, 4096);
        events = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, options, 4096, 4096);
        if (!ring || !events) return false;
        IOByteCount ringLength = 0, eventLength = 0;
        uint64_t ringPhysical = ring->getPhysicalSegment(0, &ringLength), eventPhysical = events->getPhysicalSegment(0, &eventLength);
        if (!ringPhysical || !eventPhysical || ringLength < 4096 || eventLength < 4096 || ((ringPhysical | eventPhysical) & 4095)) return false;
        bzero(memory(), 4096); bzero(events->getBytesNoCopy(), 4096);
        Descriptor *descriptors = reinterpret_cast<Descriptor *>(memory());
        for (unsigned i = 0; i < kQueueSize; i++) {
            descriptors[i] = {eventPhysical + i * sizeof(Event), sizeof(Event), 2, 0};   // device-writable
            availableRing()[2 + i] = (uint16_t)i;
        }
        availableRing()[0] = 1;                                            // VIRTQ_AVAIL_F_NO_INTERRUPT: polled
        available = kQueueSize;
        dmaBarrier(); availableRing()[1] = available; dmaBarrier();
        write32(0x38, kQueueSize); write64(0x80, ringPhysical); write64(0x90, ringPhysical + 1024); write64(0xa0, ringPhysical + 2048);
        write32(0x44, 1);
        write32(0x70, 15);                                                 // DRIVER_OK: QEMU routes its input here
        if ((read32(0x70) & (15 | 64 | 128)) != 15) return false;
        write32(0x50, 0);
        return true;
    }
    void publish() {
        uint32_t seq = native_touch_seq;
        native_touch_seq = seq | 1; __asm__ volatile("dmb ish" ::: "memory");
        if (button) {
            native_touch_ids[0] = 1;
            native_touch_xs[0] = (int)((uint64_t)(x > maxX ? maxX : x) * (kTouchWidth - 1) / maxX);
            native_touch_ys[0] = (int)((uint64_t)(y > maxY ? maxY : y) * (kTouchHeight - 1) / maxY);
            native_touch_count = 1;
        } else {
            native_touch_count = 0;
        }
        __asm__ volatile("dmb ish" ::: "memory"); native_touch_seq = (seq | 1) + 1;
        native_virtio_input_touches = native_virtio_input_touches + 1;
        native_touch_published();
    }
    void handle(const Event &event) {
        native_virtio_input_events = native_virtio_input_events + 1;
        if (event.type == kEvAbs && event.code == kAbsX) x = event.value;
        else if (event.type == kEvAbs && event.code == kAbsY) y = event.value;
        else if (event.type == kEvKey && (event.code == kBtnLeft || event.code == kBtnTouch)) button = event.value != 0;
        else if (event.type == kEvKey && (event.code == kKeyHome || event.code == kKeyF1)) {
            native_virtio_input_keys = native_virtio_input_keys + 1;
            if (event.value == 1 && !native_hid_input_command) native_hid_input_command = 1;   // NativeHidInput presses and releases
        } else if (event.type == kEvSyn) {
            // A touch moves while the button is down, and ends once when it comes up.
            if (button || touching) publish();
            touching = button;
        }
    }
    void poll() {
        if (read32(0x70) & (64 | 128)) { native_virtio_input_failures = native_virtio_input_failures + 1; return; }
        bool returned = false;
        while (usedHeader()[1] != used) {
            dmaBarrier();
            uint32_t index = usedRing()[used % kQueueSize].id;
            ++used;
            if (index >= kQueueSize) { native_virtio_input_failures = native_virtio_input_failures + 1; continue; }
            Event event;
            memcpy(&event, static_cast<unsigned char *>(events->getBytesNoCopy()) + index * sizeof(Event), sizeof(event));
            handle(event);
            availableRing()[2 + available % kQueueSize] = (uint16_t)index;
            ++available; returned = true;
        }
        if (returned) { dmaBarrier(); availableRing()[1] = available; dmaBarrier(); write32(0x50, 0); }
        uint32_t pending = read32(0x60); if (pending & 3) write32(0x64, pending & 3);
    }
};

Device devices[4];
unsigned deviceCount;
IOWorkLoop *loop;
IOTimerEventSource *timer;

void tick(OSObject *, IOTimerEventSource *source) {
    for (unsigned i = 0; i < deviceCount; i++) devices[i].poll();
    source->setTimeoutMS(8);
}
}

void native_virtio_input_probe(void) {
    if (loop) return;
    OSDictionary *matching = IOService::nameMatching("virtio,mmio");
    OSIterator *services = matching ? IOService::getMatchingServices(matching) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!services) return;
    while (IOService *provider = OSDynamicCast(IOService, services->getNextObject())) {
        if (deviceCount == sizeof(devices) / sizeof(devices[0])) break;
        IOMemoryMap *mapping = provider->mapDeviceMemoryWithIndex(0);
        if (!mapping) continue;
        uintptr_t base = mapping->getVirtualAddress();
        if (mapping->getLength() < 0x200 || mmioRead32(base) != 0x74726976 || mmioRead32(base + 4) != 2 ||
            mmioRead32(base + 8) != kDeviceInput) {
            mapping->release();
            continue;
        }
        Device &device = devices[deviceCount];
        device.mapping = mapping; device.registers = base;
        if (device.start()) { deviceCount++; continue; }
        // Reset first: a device that got its queue may still write into it.
        native_virtio_input_failures = native_virtio_input_failures + 1;
        device.write32(0x70, 0);
        OSSafeReleaseNULL(device.ring); OSSafeReleaseNULL(device.events);
        mapping->release();
        device = Device();
    }
    services->release();
    native_virtio_input_devices = deviceCount;
    if (!deviceCount) return;
    loop = IOWorkLoop::workLoop();
    timer = loop ? IOTimerEventSource::timerEventSource(loop, &tick) : nullptr;
    if (timer && loop->addEventSource(timer) == kIOReturnSuccess) timer->setTimeoutMS(8);
}
