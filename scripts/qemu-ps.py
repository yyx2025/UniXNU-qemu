#!/usr/bin/env python3
"""List the processes of the kernel that a run-qemu.py run is executing, through its QEMU monitor.

  qemu-ps.py [--run RUN_DIR] [--value SYMBOL[:BYTES] ...] [--log FILE] [--threads FILE]

Walks the kernel's allproc list and prints PID, state and name. --value prints kernel globals instead, as unsigned
little-endian numbers of BYTES bytes (default 4), for example the display's native_legacy_stage. --log writes the
kernel log, which this board shows neither on the screen nor on the serial port: the message buffer (msgbufp), in
order. --threads writes the threads of the UI processes as JSON (UniXNU's thread_snapshot.py: saved user registers
and frame records). RUN_DIR defaults to the newest out/qemu/run/<time>; the run must still be going. The struct proc
offsets are the defaults of UniXNU's scripts/qemu-entry/gdb-stop.py; --threads takes the layout of
audit-target-display-query.py (build/display-analysis/query-plan.json, for this kernel).

Every read is QMP pmemsave of guest physical memory, which needs nothing from the virtual CPU. Kernel virtual
addresses are translated here through the kernel's own tables: the root is cpu_ttep, read where DarwinBoot loaded the
kernel (the "kernel va ... pa ..." line of the serial log). The monitor's "x" would ask the virtual CPU for its state
on every read; under HVF that request is sometimes not seen, and QEMU's main loop, its window and its monitor then
wait for good.
"""
import argparse
import json
from pathlib import Path
import re
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT.parent
LIST, PID, STAT, COMM = 0, 96, 100, 1468          # p_list.le_next, p_pid, p_stat, p_comm
STATES = {1: 'idle', 2: 'run', 3: 'stop', 4: 'zombie', 5: 'reaped'}
TCR = 0x22a51a251a                                  # the kernel's translation control: 4 KiB granules, 38-bit halves

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--run', type=Path, help='run directory (default: the newest)')
parser.add_argument('--value', action='append', default=[], metavar='SYMBOL[:BYTES]', help='print a kernel global')
parser.add_argument('--log', type=Path, metavar='FILE', help='write the kernel log')
parser.add_argument('--threads', type=Path, metavar='FILE',
                    help="write the threads of the UI processes (UniXNU's thread_snapshot.py) as JSON")
parser.add_argument('--process', action='append', default=[], metavar='NAME',
                    help='with --threads: these processes instead of the UI processes (at most 16)')
args = parser.parse_args()
run = args.run or max((WORK / 'run').iterdir())
match = re.search(r'-qmp unix:([^,]+),', (run / 'command.txt').read_text())
if not match:
    raise SystemExit(f'no QMP socket in {run}/command.txt')
sock = match[1]
loaded = re.search(rb'kernel +va 0x([0-9A-Fa-f]+) +pa 0x([0-9A-Fa-f]+)', (run / 'serial.log').read_bytes())
if not loaded:
    raise SystemExit(f'{run}/serial.log has no kernel load address yet')
kernel_va, kernel_pa = int(loaded[1], 16), int(loaded[2], 16)


monitor = None


def qmp(name, **arguments):
    # One connection for the whole run: QEMU takes one QMP client at a time and is slow to take the next one, so a
    # connection per read would often be refused. run-qemu.py's screenshots wait while this one is open.
    global monitor
    if monitor is None:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(60)
        for attempt in range(100):
            try:
                s.connect(sock)
                break
            except ConnectionRefusedError:
                if attempt == 99:
                    raise
                time.sleep(0.2)
        monitor = s.makefile('rwb', buffering=0)
        monitor.readline()
        monitor.write(b'{"execute": "qmp_capabilities"}\n')
        while 'return' not in json.loads(monitor.readline()):
            pass
    monitor.write((json.dumps({'execute': name, 'arguments': arguments}) + '\n').encode())
    while True:
        reply = json.loads(monitor.readline())
        if 'return' in reply or 'error' in reply:
            break
    if 'error' in reply:
        raise SystemExit(f'{name} {arguments}: {reply["error"]}')
    return reply['return']


scratch = run / 'pmem.bin'


def physical(address, count):
    qmp('pmemsave', val=address, size=count, filename=str(scratch))
    data = scratch.read_bytes()
    if len(data) != count:
        raise SystemExit(f'cannot read physical {address:#x}')
    return data


tables = {}


def table(address):
    if address not in tables:
        tables[address] = physical(address, 4096)
    return tables[address]


root = None


def translate(va):
    """The physical address of a kernel virtual address, by the kernel's 4 KiB tables (levels 1 to 3)."""
    global root
    if root is None:
        root = int.from_bytes(physical(kernel_pa + symbol('cpu_ttep') - kernel_va, 8), 'little')
    level_table = root & 0x0000fffffffff000
    for level, shift in ((1, 30), (2, 21), (3, 12)):
        index = (va >> shift) & (0xff if level == 1 else 0x1ff)
        descriptor = int.from_bytes(table(level_table)[index * 8:index * 8 + 8], 'little')
        if not descriptor & 1:
            raise SystemExit(f'{va:#x} is not mapped')
        output = descriptor & 0x0000fffffffff000
        if level == 3 or descriptor & 3 == 1:       # a page, or a block at level 1 or 2
            return (output & ~((1 << shift) - 1)) | (va & ((1 << shift) - 1))
        level_table = output
    raise SystemExit(f'{va:#x} has no leaf')


def read(address, count):
    data = b''
    while count:
        chunk = min(count, 4096 - (address & 4095))
        data += physical(translate(address), chunk)
        address += chunk
        count -= chunk
    return data


symbols = subprocess.check_output(['xcrun', 'nm', WORK / 'kernel/kernel.macho'], text=True)


def symbol(name):
    found = re.search(rf'^([0-9a-f]{{16}}) \w _{re.escape(name)}$', symbols, re.M)
    if not found:
        raise SystemExit(f'{name} is not a kernel symbol')
    return int(found[1], 16)


try:
    if args.value:
        for request in args.value:
            name, _, size = request.partition(':')
            value = int.from_bytes(read(symbol(name), int(size or 4)), 'little')
            print(f'{name} = {value} ({value:#x})')
        raise SystemExit(0)
    if args.log:
        # struct msgbuf: magic, size, write index, read index (ints), then the buffer pointer. The buffer is a ring.
        buffer = int.from_bytes(read(symbol('msgbufp'), 8), 'little')
        header = read(buffer, 24)
        size, write = (int.from_bytes(header[at:at + 4], 'little') for at in (4, 8))
        data = read(int.from_bytes(header[16:24], 'little'), size)
        args.log.write_bytes((data[write:] + data[:write]).replace(b'\0', b''))
        print(f'kernel log: {args.log}')
        raise SystemExit(0)
    proc = int.from_bytes(read(symbol('allproc'), 8), 'little')
    rows = []
    while proc and len(rows) < 1024:
        pid = int.from_bytes(read(proc + PID, 4), 'little', signed=True)
        state = int.from_bytes(read(proc + STAT, 1), 'little')
        name = read(proc + COMM, 17).split(b'\0')[0].decode(errors='replace')
        rows.append((pid, STATES.get(state, str(state)), name, proc))
        proc = int.from_bytes(read(proc + LIST, 8), 'little')
    if args.threads:
        # thread_snapshot.py reads through a GDB stub: "m" packets of kernel virtual addresses, and of physical ones
        # while qemu.PhyMemMode is set (the user stacks, which it translates itself through the task's tables).
        import importlib.util
        plan = json.loads((ROOT / 'build/display-analysis/query-plan.json').read_text())
        spec = importlib.util.spec_from_file_location('thread_snapshot', ROOT / 'scripts/qemu-entry/thread_snapshot.py')
        snapshot = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(snapshot)
        physical_mode = False

        def packet(request):
            global physical_mode
            if request == 'qqemu.PhyMemMode':
                return '1' if physical_mode else '0'
            if request.startswith('Qqemu.PhyMemMode:'):
                physical_mode = request.endswith('1')
                return 'OK'
            address, size = (int(part, 16) for part in request[1:].split(','))
            try:
                return (physical(address, size) if physical_mode else read(address, size)).hex()
            except SystemExit:
                return 'E01'

        processes = [dict(pid=pid, name=name, address=hex(address)) for pid, _, name, address in rows]
        if args.process:
            # thread_snapshot.py picks processes by its own list of names; the requested ones go in under one of
            # them and get their names back afterwards.
            processes = [dict(p, name='PreBoard', real_name=p['name']) for p in processes if p['name'] in args.process]
        result = snapshot.capture(packet, processes, plan, {'TCR_EL1': hex(TCR)})
        for row in result['processes']:
            if 'real_name' in row['process']:
                row['process']['name'] = row['process'].pop('real_name')
        args.threads.write_text(json.dumps({'ui_thread_snapshots': result}, indent=1))
        print(f'threads: {args.threads}')
        raise SystemExit(0)
    for pid, state, name, _ in sorted(rows):
        print(f'{pid:5d}  {state:6s}  {name}')
    print(f'{len(rows)} processes')
finally:
    scratch.unlink(missing_ok=True)
