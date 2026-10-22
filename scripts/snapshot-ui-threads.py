#!/usr/bin/env python3
"""Stop the iPadOS userland once and read the threads of its UI processes.

  snapshot-ui-threads.py [--seconds N] [--crash PROCESS] [--accel hvf|tcg]

gdb-stop.py boots the qemu_root_defconfig payload with the OS and SystemOS disks, lets it run for N seconds (its
primary breakpoint is panic, which a healthy boot never reaches), interrupts the guest and reads, through UniXNU's
thread_snapshot.py, the threads of SpringBoard, backboardd, PreBoard, Setup and the other UI processes it lists:
state, wait event, saved user registers and up to 32 user frame records. Nothing in the guest is changed.

--crash stops instead when the named process (one of thread_snapshot.py's list, such as liveactivitiesd) reports its
crash: task_exception_notify, which a process that dies of an exception or a signal calls with EXC_CRASH on its way
out. Its threads then still hold the crashing state.

The process layout comes from audit-target-display-query.py (see trace-render.py for its inputs). The report is
build/ui-threads/gdb-stop.json; symbolize-ui-threads.py names the frames. Run in the build tree.
"""
import argparse
import plistlib
from pathlib import Path
import re
import subprocess
import sys

from qemu_esp import ROOT, WORK, link_dyld, stage

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--seconds', type=int, default=180, help='how long the userland runs before the snapshot')
parser.add_argument('--crash', metavar='PROCESS', help='stop when this process reports its crash')
parser.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf')
args = parser.parse_args()

volumes = ROOT / '.cache/firmware/target-volumes'
kernel = WORK / 'kernel/kernel.macho'
if not kernel.is_file():
    raise SystemExit(f'{kernel} missing (unixnu build kernel)')
plan = ROOT / 'build/display-analysis/query-plan.json'
link_dyld()
subprocess.run([sys.executable, ROOT / 'scripts/audit-target-display-query.py'], check=True)

symbols = subprocess.check_output(['xcrun', 'nm', kernel], text=True)


def symbol(name):
    return re.search(rf'^([0-9a-f]{{16}}) \w {re.escape(name)}$', symbols, re.M)[1]


image = volumes / '044-76040-038.dmg'
attached = [i for i in plistlib.loads(subprocess.check_output(['hdiutil', 'info', '-plist']))['images']
            if Path(i['image-path']).resolve() == image.resolve() and i.get('writeable') is False]
if not attached:
    subprocess.run(['hdiutil', 'attach', '-nomount', '-readonly', '-noverify', '-noautofsck', image], check=True)

out = ROOT / 'build/ui-threads' / (f'crash-{args.crash}' if args.crash else 'interrupt')
stage(out / 'esp', kernel)
# At the crash the exiting thread's own saved state (PC, LR, frame pointer, ESR, FAR) is read as well: the process may
# not be one of thread_snapshot.py's names.
stop = ['0x' + symbol('_task_exception_notify'), '--entry-process-name', args.crash, '--entry-max-skips', '65536',
        '--inspect-entry-user-state'] if args.crash else ['0x' + symbol('_panic')]
subprocess.run([sys.executable, ROOT / 'scripts/qemu-entry/gdb-stop.py', *stop, '--esp', out / 'esp',
                '--kernel', kernel, '--fixture', out, '--qemu-ramfb', '--gic-version', '3', '--qemu-memory-mib', '8192',
                '--qemu-accel', args.accel, '--system-registers', '--process-layout-plan', plan,
                '--allproc-address', '0x' + symbol('_allproc'), '--proc-list-max', '512', '--inspect-ui-threads',
                '--virtio-udif-image', image, '--virtio-extra-block-image', volumes / '044-76034-038.dmg',
                '--timeout', str(args.seconds)], check=True)
print('report:', out / 'gdb-stop.json')
