#!/usr/bin/env python3
"""Check the memory window that run-qemu.py --shared-window shares between this Mac and the guest.

  guest-ram.py [--run RUN_DIR] [--rounds N] [--length BYTES]

QEMU maps RUN_DIR/shared.bin into the guest after its ordinary memory, and package/SharedWindow.cpp maps the same
memory in the kernel and writes a mailbox at its start. Each round writes random 64-bit words at 1 MiB and asks the
kernel (which polls every 1 ms) for their sum and for a block of its own after them; the round checks both and times
the round trip. This is the channel a Metal device in the guest whose work runs on this Mac will use for its buffers.
"""
import argparse
import mmap
import os
import random
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT.parent
GUEST_MAGIC = 0x314e495753584e55          # "UNXSWIN1", package/SharedWindow.cpp
BLOCKS = 1 << 20
MAILBOX = struct.Struct('<QIIQQQQQQ')       # magic, mode, ticks, request, answered, length, sum, seed, busy
MODES = {1: 'write-back', 2: 'device'}

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--run', type=Path, help='run directory (default: the newest)')
parser.add_argument('--rounds', type=int, default=20)
parser.add_argument('--length', type=int, default=4 << 20, help='bytes in each block (default 4 MiB)')
parser.add_argument('--wait', type=float, default=600, help='seconds to wait for the kernel\'s mailbox')
args = parser.parse_args()
run = args.run or max((WORK / 'run').iterdir())
if args.length % 8:
    raise SystemExit('--length must be a multiple of 8')
with (run / 'shared.bin').open('r+b') as handle, mmap.mmap(handle.fileno(), 0) as window:
    if BLOCKS + 2 * args.length > len(window):
        raise SystemExit(f'two blocks of {args.length} bytes do not fit the {len(window) >> 20} MiB window')

    def mailbox():
        return MAILBOX.unpack_from(window, 0)

    deadline = time.monotonic() + args.wait
    while mailbox()[0] != GUEST_MAGIC:
        if time.monotonic() > deadline:
            raise SystemExit(f'no mailbox from the kernel after {args.wait:.0f} s')
        time.sleep(0.5)
    magic, mode, ticks, request, answered, *_ = mailbox()
    time.sleep(0.5)
    print(f'kernel mailbox: {MODES.get(mode, mode)} mapping, {mailbox()[2] - ticks} polls in 0.5 s')
    count = args.length // 8
    trips, failures = [], 0
    for round_ in range(args.rounds):
        data = os.urandom(args.length)
        window[BLOCKS:BLOCKS + args.length] = data
        seed = random.getrandbits(63)
        request += 1
        struct.pack_into('<QQ', window, 0x20, args.length, 0)      # length, sum
        struct.pack_into('<Q', window, 0x30, seed)
        start = time.perf_counter()
        struct.pack_into('<Q', window, 0x10, request)
        while mailbox()[4] != request:
            if time.perf_counter() - start > 5:
                raise SystemExit(f'round {round_}: no answer after 5 s')
        trips.append(time.perf_counter() - start)
        expected = sum(struct.unpack(f'<{count}Q', data)) & (1 << 64) - 1
        guest = struct.unpack_from(f'<{count}Q', window, BLOCKS + args.length)
        wrong = sum(1 for i, word in enumerate(guest) if word != (seed + i) & (1 << 64) - 1)
        busy = mailbox()[8]
        if mailbox()[6] != expected or wrong:
            failures += 1
            print(f'round {round_}: sum {"ok" if mailbox()[6] == expected else "WRONG"}, {wrong} wrong words '
                  f'in the kernel\'s block')
    trips.sort()
    print(f'{args.rounds} rounds of {args.length >> 10} KiB each way: {failures} failed; round trip '
          f'min {trips[0] * 1e3:.2f} ms, median {trips[len(trips) // 2] * 1e3:.2f} ms, max {trips[-1] * 1e3:.2f} ms; '
          f'the kernel spent {busy / 24e6 * 1e3:.2f} ms reading and writing {2 * args.length >> 20} MiB in the last')
    sys.exit(1 if failures else 0)
