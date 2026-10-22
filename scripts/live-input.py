#!/usr/bin/env python3
"""Drive NativeHidInput through UniXNU's live HID mailbox and take screenshots.

  live-input.py [--accel hvf|tcg] [--wait SECONDS] [--crash PROCESS] STEP...

gdb-stop.py boots the qemu_root_defconfig payload with the OS and SystemOS disks and --live-hid-dir: once a second,
when NativeHidInput's timer reaches native_hid_live_checkpoint, it applies the newest command of the mailbox
(build/live-input/command.json): Home, a touch at x 0..30480, y 0..20320 (the axes of TouchState.cpp's finger set, the
whole screen), or lift. This script waits until the display has presented frames and then SECONDS more, and runs the
steps in order:

  shot NAME            screenshot to build/live-input/NAME.png (QMP screendump of gdb-stop's --capture-screen socket)
  home                 press and release Home
  touch X Y            one finger down, or moved, at X Y
  lift                 the finger up
  swipe-up             touch at the bottom middle, move up in four steps, lift
  sleep N              wait N seconds

Every command takes effect at the next checkpoint, so a touch moves once a second. Then it stops the guest. Nothing
else in the guest is changed. --crash stops the guest instead when the named process (its 16-character kernel name)
reports its crash (task_exception_notify), with the exiting thread's saved user state in the report, as
snapshot-ui-threads.py --crash does. Run in the build tree.
"""
import argparse
import json
from pathlib import Path
import plistlib
import re
import socket
import subprocess
import sys
import time

from qemu_esp import ROOT, WORK, link_dyld, stage

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--accel', choices=('hvf', 'tcg'), default='hvf')
parser.add_argument('--wait', type=int, default=20, help='seconds after the first presented frames')
parser.add_argument('--crash', metavar='PROCESS', help='stop when this process reports its crash')
parser.add_argument('steps', nargs='+')
args = parser.parse_args()

volumes = ROOT / '.cache/firmware/target-volumes'
kernel = WORK / 'kernel/kernel.macho'
symbols = subprocess.check_output(['xcrun', 'nm', kernel], text=True)
image = volumes / '044-76040-038.dmg'
attached = [i for i in plistlib.loads(subprocess.check_output(['hdiutil', 'info', '-plist']))['images']
            if Path(i['image-path']).resolve() == image.resolve() and i.get('writeable') is False]
if not attached:
    subprocess.run(['hdiutil', 'attach', '-nomount', '-readonly', '-noverify', '-noautofsck', image], check=True)

out = ROOT / 'build/live-input'
mailbox = out / 'mailbox'
mailbox.mkdir(parents=True, exist_ok=True)
for name in ('command.json', 'status.json'):
    (mailbox / name).unlink(missing_ok=True)
stage(out / 'esp', kernel)


def address(name):
    return '0x' + re.search(rf'^([0-9a-f]{{16}}) \w {name}$', symbols, re.M)[1]


stop = [address('_panic')]
if args.crash:
    link_dyld()
    subprocess.run([sys.executable, ROOT / 'scripts/audit-target-display-query.py'], check=True)
    stop = [address('_task_exception_notify'), '--entry-process-name', args.crash, '--entry-max-skips', '65536',
            '--inspect-entry-user-state', '--process-layout-plan', ROOT / 'build/display-analysis/query-plan.json',
            '--system-registers']
guest = subprocess.Popen([sys.executable, ROOT / 'scripts/qemu-entry/gdb-stop.py', *stop, '--esp', out / 'esp',
                          '--kernel', kernel, '--fixture', out, '--qemu-ramfb', '--capture-screen', '--gic-version', '3',
                          '--qemu-memory-mib', '8192', '--qemu-accel', args.accel, '--live-hid-dir', mailbox,
                          '--virtio-udif-image', image, '--virtio-extra-block-image', volumes / '044-76034-038.dmg',
                          '--timeout', '1800'], stdout=(out / 'gdb-stop.out').open('w'), stderr=subprocess.STDOUT)


def status():
    try:
        return json.loads((mailbox / 'status.json').read_text())
    except (OSError, ValueError):
        return {}


def alive():
    if guest.poll() is not None:
        raise SystemExit(f'gdb-stop ended ({guest.returncode}); see {out / "gdb-stop.out"}')


command_id = 0


def send(**command):
    global command_id
    command_id += 1
    temporary = mailbox / 'command.tmp'
    temporary.write_text(json.dumps(dict(id=command_id, **command)))
    temporary.replace(mailbox / 'command.json')
    while status().get('accepted_command_id', 0) < command_id:
        alive()
        time.sleep(0.2)


def shot(name):
    sockets = sorted(Path('/tmp').glob('sheng-qmp-*/qmp.sock'), key=lambda p: p.stat().st_mtime)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(30)
        s.connect(str(sockets[-1]))
        stream = s.makefile('rwb', buffering=0)
        stream.readline()
        ppm = out / f'{name}.ppm'
        for request in ({'execute': 'qmp_capabilities'}, {'execute': 'screendump', 'arguments': {'filename': str(ppm)}}):
            stream.write((json.dumps(request) + '\n').encode())
            while 'return' not in (reply := json.loads(stream.readline())) and 'error' not in reply:
                pass
    subprocess.run(['sips', '-s', 'format', 'png', ppm, '--out', out / f'{name}.png'], check=True, capture_output=True)
    ppm.unlink()
    print('screenshot:', out / f'{name}.png', flush=True)


# The display has presented frames once the render server draws (the welcome screen).
while (status().get('observed') or {}).get('native_legacy_transfer_calls', 0) < 2:
    alive()
    time.sleep(1)
print('frames presented after', status().get('elapsed_seconds'), 'seconds', flush=True)
time.sleep(args.wait)
steps = list(args.steps)
while steps:
    step = steps.pop(0)
    alive()
    if step == 'shot':
        shot(steps.pop(0))
    elif step == 'home':
        send(action='home')
    elif step == 'touch':
        send(action='touch', x=int(steps.pop(0)), y=int(steps.pop(0)))
    elif step == 'lift':
        send(action='lift')
    elif step == 'swipe-up':
        for y in (19800, 16000, 12000, 8000, 4000):
            send(action='touch', x=15240, y=y)
        send(action='lift')
    elif step == 'sleep':
        time.sleep(float(steps.pop(0)))
    else:
        raise SystemExit(f'unknown step {step}')
    print('done:', step, flush=True)
send(action='stop')
guest.wait()
print('observed:', json.dumps(status().get('observed', {}).get('native_hid_input_events')), 'report:', out / 'gdb-stop.json')
