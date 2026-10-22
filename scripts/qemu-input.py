#!/usr/bin/env python3
"""Touch, press keys and take screenshots in a guest run-qemu.py --os-root is running, through its QMP socket.

  qemu-input.py [--run RUN_DIR] STEP...

  tap X Y              touch at screen pixel X Y and lift
  swipe X1 Y1 X2 Y2    touch at X1 Y1, move to X2 Y2 in eight steps, lift
  home                 press the Home key (VirtioInput.cpp: Home presses the Home button)
  shot NAME            screenshot to RUN_DIR/NAME.png
  sleep N              wait N seconds

The events go to QEMU's VirtIO tablet and keyboard as the cocoa window's would (input-send-event, send-key), so the guest's
VirtioInput.cpp makes them touches of NativeHidInput. Pixels are those of the guest's screen (run-qemu.py --screen).
"""
import argparse
import json
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT.parent

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--run', type=Path, help='run directory (default: the newest)')
parser.add_argument('steps', nargs='+')
args = parser.parse_args()
run = args.run or max((WORK / 'run').iterdir())
command = (run / 'command.txt').read_text()
path = re.search(r'-qmp unix:([^,\s]+)', command)[1]
size = re.search(r'ramfb-size,string=(\d+)x(\d+)', command)
width, height = (int(size[1]), int(size[2])) if size else (800, 600)

connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
connection.settimeout(30)
connection.connect(path)
stream = connection.makefile('rwb', buffering=0)
stream.readline()


def qmp(name, **arguments):
    stream.write((json.dumps({'execute': name, 'arguments': arguments} if arguments else {'execute': name}) + '\n').encode())
    while True:
        reply = json.loads(stream.readline())
        if 'return' in reply or 'error' in reply:
            if 'error' in reply:
                raise SystemExit(f'{name}: {reply["error"]}')
            return reply['return']


def position(x, y):
    return [{'type': 'abs', 'data': {'axis': 'x', 'value': round(float(x) * 0x7fff / (width - 1))}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': round(float(y) * 0x7fff / (height - 1))}}]


def button(down):
    return [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]


qmp('qmp_capabilities')
steps = list(args.steps)
while steps:
    step = steps.pop(0)
    if step == 'tap':
        x, y = steps.pop(0), steps.pop(0)
        qmp('input-send-event', events=position(x, y))
        qmp('input-send-event', events=button(True))
        time.sleep(0.15)
        qmp('input-send-event', events=button(False))
    elif step == 'swipe':
        x1, y1, x2, y2 = (float(steps.pop(0)) for _ in range(4))
        qmp('input-send-event', events=position(x1, y1) + button(True))
        for i in range(1, 9):
            time.sleep(0.05)
            qmp('input-send-event', events=position(x1 + (x2 - x1) * i / 8, y1 + (y2 - y1) * i / 8))
        time.sleep(0.05)
        qmp('input-send-event', events=button(False))
    elif step == 'home':
        qmp('send-key', keys=[{'type': 'qcode', 'data': 'home'}])
    elif step == 'shot':
        name = steps.pop(0)
        ppm = run / f'{name}.ppm'
        qmp('screendump', filename=str(ppm))
        subprocess.run(['sips', '-s', 'format', 'png', ppm, '--out', run / f'{name}.png'], check=True, capture_output=True)
        ppm.unlink()
        print(run / f'{name}.png')
    elif step == 'sleep':
        time.sleep(float(steps.pop(0)))
    else:
        sys.exit(f'unknown step {step}')
