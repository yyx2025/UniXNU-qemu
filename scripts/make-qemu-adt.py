#!/usr/bin/env python3
"""Generate the Apple-format device tree of the QEMU virt machine that scripts/run-qemu.py starts.

It takes the [payload.adt] options the unixnu driver passes to every board tool. UniXNU's scripts/qemu-entry/make-dt.py
describes the machine (GICv3, timer, NVRAM proxy, memory window, root matching, and the VirtIO MMIO windows from the
device tree QEMU itself generates for the same machine); this script then adds what the board trees carry and
make-dt.py does not: the platform seed and, with --ipad-identity, the iPad7,1 identity of the sheng and pipa trees. It
removes the UFS nodes make-dt.py adds for UniXNU's own UFS tests, as QEMU has no UFS controller.
"""
from pathlib import Path
import argparse
import os
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
QEMU_ACCEL = os.environ.get('UNIXNU_QEMU_ACCEL', 'tcg')    # hvf reads the host counter frequency for the timebase


def parse(blob, at=0):
    """(name, [(key, value)], [children]), end offset."""
    count, children = struct.unpack_from('<II', blob, at)
    at += 8
    properties = []
    for _ in range(count):
        key = blob[at:at + 32].split(b'\0')[0].decode()
        (length,) = struct.unpack_from('<I', blob, at + 32)
        at += 36
        properties.append((key, blob[at:at + length]))
        at += length + (-length % 4)
    kids = []
    for _ in range(children):
        child, at = parse(blob, at)
        kids.append(child)
    return [dict(properties)['name'].rstrip(b'\0').decode(), properties, kids], at


def serialize(node):
    _, properties, children = node
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data)) + data + bytes(-len(data) % 4)
    return result + b''.join(serialize(child) for child in children)


def child(node, name):
    return next(c for c in node[2] if c[0] == name)


def put(node, key, value):
    """Set a property, replacing an existing one in place."""
    for index, (existing, _) in enumerate(node[1]):
        if existing == key:
            node[1][index] = (key, value)
            return
    node[1].append((key, value))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/qemu/boot/kernel.macho.adt')
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), required=True)
    parser.add_argument('--owned-memory-size', type=lambda value: int(value, 0), default=None)
    parser.add_argument('--root-match-volume-uuid', default=None)
    parser.add_argument('--unique-chip-id', type=lambda value: int(value, 16), default=None)
    parser.add_argument('--chip-id', type=lambda value: int(value, 16), default=None)
    parser.add_argument('--ipad-identity', action='store_true')
    parser.add_argument('--cpu-regs', default='0', help='Only the count is used: QEMU numbers its CPUs in Affinity0')
    parser.add_argument('--psci-method', choices=['smc', 'hvc'], default='hvc', help='Ignored: virt without EL2 uses hvc')
    parser.add_argument('--packed-data-ramdisk', action='store_true')
    parser.add_argument('--data-ramdisk', action='store_true')
    parser.add_argument('--trust-cache', action='store_true')
    args = parser.parse_args()
    if (args.unique_chip_id is None) != (args.chip_id is None):
        parser.error('--unique-chip-id and --chip-id must be supplied together')
    cpus = len(args.cpu_regs.split(','))

    # --qemu-vmm-present: /defaults/vmm-present, which XNU reports as kern.hv_vmm_present. The original watchdogd
    # exits normally in a VM without a watchdog service, and stops with SIGTRAP otherwise (check-watchdogd-branches.py).
    command = ['python3', ROOT / 'scripts/qemu-entry/make-dt.py', '--armvirt', '--armcpu', '--nvram-proxy', '--qemu-vmm-present',
               '--crypto-hash-method', 'sha2-384', '--amfi-platform-only', '--osenvironment', 'main',
               '--qemu-accel', QEMU_ACCEL, '--dram-size', str(args.dram_size)]
    if args.owned_memory_size is not None:
        command += ['--owned-memory-size', str(args.owned_memory_size)]
    if args.data_ramdisk:
        command.append('--data-ramdisk')
    if args.trust_cache:
        command.append('--trust-cache')
    if cpus > 1:
        command += ['--smp', str(cpus)]
    qemu = shutil.which('qemu-system-aarch64')
    if not qemu:
        raise SystemExit('qemu-system-aarch64 not found (brew install qemu)')
    with tempfile.TemporaryDirectory() as scratch:
        # The machine of run-qemu.py, whose memory is dram_size.
        fdt = Path(scratch) / 'virt.dtb'
        subprocess.run([qemu, '-M', f'virt,virtualization=off,secure=off,gic-version=3,dumpdtb={fdt}', '-cpu', 'cortex-a710',
                        '-m', str(args.dram_size >> 20), '-smp', str(cpus), '-display', 'none', '-nodefaults'],
                       check=True, capture_output=True)
        command += ['--virtio-mmio-fdt', fdt]
        if args.root_match_volume_uuid:
            command += ['--root-match-volume-uuid', args.root_match_volume_uuid]
        base = Path(scratch) / 'base.adt'
        subprocess.run([str(part) for part in command + ['--output', base]], check=True)
        tree, end = parse(base.read_bytes())
        if end != base.stat().st_size:
            raise SystemExit('make-dt.py wrote trailing bytes after the tree')

    chosen = child(tree, 'chosen')
    if args.unique_chip_id is not None:
        put(chosen, 'unique-chip-id', struct.pack('<Q', args.unique_chip_id))
        put(chosen, 'chip-id', struct.pack('<I', args.chip_id))
    if args.packed_data_ramdisk:
        put(chosen, 'ephemeral-storage', struct.pack('<I', 1))
        put(child(chosen, 'memory-map'), 'PackedRAMDisk', bytes(16))
    # /defaults/sleep-disabled: IOPMrootDomain refuses every system sleep (gSleepDisabledFlag, read once boot has
    # completed). ArmCpu cannot quiesce the CPU, and an idle sleep would end in its "unsupported quiesce" panic.
    put(child(tree, 'defaults'), 'sleep-disabled', struct.pack('<I', 1))
    # /defaults/kern.maxvnodes: the BCM2837 kernel configuration caps the vnodes at 1024 (CONFIG_VNODES), while the
    # iPadOS userland keeps about 2000 open. The kernel then reclaims vnodes by killing processes ("vnode jetsam"):
    # apps, backboardd and SpringBoard. XNU takes this value instead of its own sizing (bsd/dev/unix_startup.c), here
    # the one that sizing gives for the 4 GiB the kernel manages: 4 GiB / 64 KiB + 1024.
    put(child(tree, 'defaults'), 'kern.maxvnodes', struct.pack('<I', (4 << 30) // 65536 + 1024))
    arm_io = child(tree, 'arm-io')
    arm_io[2] = [c for c in arm_io[2] if c[0] not in ('ufshc', 'ufsphy')]
    if args.ipad_identity:
        # The values of make-sheng-adt.py and make-pipa-adt.py; MobileGestalt reads them from the tree.
        put(tree, 'model', b'iPad7,1\0')
        put(tree, 'target-type', b'J120\0')
        for key in ('certificate-production-status', 'effective-production-status-ap'):
            put(chosen, key, struct.pack('<I', 0))
        for key in ('certificate-security-mode', 'effective-security-mode-sep'):
            put(chosen, key, struct.pack('<I', 1))
        product = child(tree, 'product')
        for key, value in (('product-name', b'iPad Pro (12.9-inch) (2nd generation)\0'),
                           ('product-description', b'iPad Pro (12.9-inch) (2nd generation)\0'),
                           ('artwork-device-idiom', b'pad\0'), ('artwork-device-subtype', struct.pack('<I', 2732)),
                           ('artwork-scale-factor', struct.pack('<I', 2)), ('artwork-display-gamut', b'P3\0'),
                           ('compatible-device-fallback', b'iPad6,7\0'), ('thin-bezel', struct.pack('<I', 1))):
            put(product, key, value)
        if not any(c[0] == 'buttons' for c in tree[2]):
            tree[2].append(['buttons', [('name', b'buttons\0'), ('compatible', b'buttons\0'),
                                        ('device_type', b'buttons\0'), ('button-names', b'hold\0voldown\0volup\0'),
                                        ('home-button-type', struct.pack('<I', 2))], []])

    data = serialize(tree)
    if parse(data)[1] != len(data):
        raise SystemExit('The edited tree does not round-trip')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f'QEMU ADT: {len(data)} bytes, dram {args.dram_size:#x}, cpus {cpus}, accel {QEMU_ACCEL}')


if __name__ == '__main__':
    main()
