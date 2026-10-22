#!/usr/bin/env python3
"""Trace the original render server in backboardd: why the CPU renderer draws no frame.

  trace-render.py [--mode composition|gates|mode-state|trace|frame-path] [--accel hvf|tcg] [--timeout SECONDS]

UniXNU's prepare-original-cpu-render-observer.py writes the plan: up to four sites in QuartzCore's render path, checked
against the original cache bytes. gdb-stop.py boots the qemu_root_defconfig payload with the OS and SystemOS disks,
follows launchd to backboardd's shared-region start and stops at each site at most eight times, recording the
registers and the plan's memory reads. Nothing in the guest is changed.

The modes (sites in CA::WindowServer, QuartzCore):
- composition: Server::render_for_time after IOMFBDisplay::ignore_update_p (+0x1194, w0 is "ignore") and at the frame
  seed comparison (+0x1250, x8 and x9), AccelServer::render_update and Server::sw_renderer.
- gates: IOMFBDisplay::can_update and is_ready results, and render_update.
- mode-state: IOMFBDisplay::update_framebuffer_locked and ignore_update_p entries, render_update, sw_renderer.
- trace: IOMFBServer::timer_callback, render_for_time, render_update, sw_renderer.
- frame-path: sw_renderer's return, Display::render_display, SWContext::draw_elements, the frame buffer swap end.

Inputs the plan checks: the OS and SystemOS volumes mounted read-only at .cache/firmware/target-volumes/os-mount and
systemos-mount, dyld 1165.3 (feeds.toml of this repository) and the kernel objects of the kernel step. The payload
must be built for the accelerator (UNIXNU_QEMU_ACCEL). Run in the build tree.
"""
import argparse
import json
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import sys

from qemu_esp import ROOT, WORK, link_dyld, stage

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--mode', choices=('composition', 'gates', 'mode-state', 'trace', 'frame-path'), default='composition')
parser.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf')
parser.add_argument('--timeout', type=int, default=900, help='seconds until the guest stops, whatever was hit')
args = parser.parse_args()

volumes = ROOT / '.cache/firmware/target-volumes'
for name in ('os-mount/sbin/launchd', 'systemos-mount/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64'):
    if not (volumes / name).is_file():
        raise SystemExit(f'{volumes / name} missing: mount the volume read-only (see the header)')
kernel = WORK / 'kernel/kernel.macho'
if not kernel.is_file():
    raise SystemExit(f'{kernel} missing (unixnu build kernel)')
link_dyld()

# The process layout (struct offsets from the kernel objects' DWARF) and the plan of the selected sites.
subprocess.run([sys.executable, ROOT / 'scripts/audit-target-display-query.py'], check=True)
out = ROOT / 'build/cpu-render' / args.mode
plan = out.with_suffix('.plan.json')
subprocess.run([sys.executable, ROOT / 'scripts/prepare-original-cpu-render-observer.py', '--kernel', kernel,
                '--prelink-report', kernel.with_suffix('.json'), '--layout-plan',
                ROOT / 'build/display-analysis/query-plan.json', '--output', plan, f'--{args.mode}'], check=True)

# gdb-stop follows launchd from its main entry: the LC_MAIN offset of the original launchd.
launchd = (volumes / 'os-mount/sbin/launchd').read_bytes()
at, entry = 32, None
for _ in range(struct.unpack_from('<I', launchd, 16)[0]):
    command, size = struct.unpack_from('<2I', launchd, at)
    if command == 0x80000028:
        entry = struct.unpack_from('<Q', launchd, at + 8)[0]
    at += size
symbols = subprocess.check_output(['xcrun', 'nm', kernel], text=True)
setter = re.search(r'^([0-9a-f]{16}) \w _thread_setentrypoint$', symbols, re.M)[1]

# The OS disk: one read-only attachment, as run-qemu.py --os-root makes it.
image = volumes / '044-76040-038.dmg'
attached = [i for i in plistlib.loads(subprocess.check_output(['hdiutil', 'info', '-plist']))['images']
            if Path(i['image-path']).resolve() == image.resolve() and i.get('writeable') is False]
if not attached:
    subprocess.run(['hdiutil', 'attach', '-nomount', '-readonly', '-noverify', '-noautofsck', image], check=True)

stage(out / 'esp', kernel)
subprocess.run([sys.executable, ROOT / 'scripts/qemu-entry/gdb-stop.py', '0x' + setter, '--esp', out / 'esp',
                '--kernel', kernel, '--fixture', out, '--qemu-ramfb', '--gic-version', '3', '--qemu-memory-mib', '8192',
                '--qemu-accel', args.accel, '--system-registers', '--follow-entrypoint-arg', '1',
                '--follow-main-entry-offset', hex(entry), '--main-cache-query-plan', plan,
                '--virtio-udif-image', image, '--virtio-extra-block-image', volumes / '044-76034-038.dmg',
                '--timeout', str(args.timeout)], check=True)

report = json.loads((out / 'gdb-stop.json').read_text())
followed = report.get('followed_display_query', {})
print('backboardd:', followed.get('activated_process'), 'cache slide', followed.get('cache_slide'))
for event in followed.get('trace_events', []):
    registers = event['registers']
    print(f"{event['name']:24s} x0 {registers['x0']}  x8 {registers['x8']}  x9 {registers['x9']}  caller {event['caller']}")
print('never reached:', followed.get('trace_pending'))
print('report:', out / 'gdb-stop.json')
